//===- OpenMPToLLVMIRTranslation.cpp - Translate OpenMP dialect to LLVM IR-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a translation between the MLIR OpenMP dialect and LLVM
// IR.
//
//===----------------------------------------------------------------------===//
#include "mlir/Target/LLVMIR/Dialect/OpenMP/OpenMPToLLVMIRTranslation.h"
#include "mlir/Dialect/OpenMP/OpenMPDialect.h"
#include "mlir/Dialect/OpenMP/OpenMPInterfaces.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Operation.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Target/LLVMIR/Dialect/OpenMPCommon.h"
#include "mlir/Target/LLVMIR/ModuleTranslation.h"
#include "mlir/Transforms/RegionUtils.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Frontend/OpenMP/OMPConstants.h"
#include "llvm/Frontend/OpenMP/OMPIRBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/FileSystem.h"

using namespace mlir;

namespace {
static llvm::omp::ScheduleKind
convertToScheduleKind(std::optional<omp::ClauseScheduleKind> schedKind) {
  if (!schedKind.has_value())
    return llvm::omp::OMP_SCHEDULE_Default;
  switch (schedKind.value()) {
  case omp::ClauseScheduleKind::Static:
    return llvm::omp::OMP_SCHEDULE_Static;
  case omp::ClauseScheduleKind::Dynamic:
    return llvm::omp::OMP_SCHEDULE_Dynamic;
  case omp::ClauseScheduleKind::Guided:
    return llvm::omp::OMP_SCHEDULE_Guided;
  case omp::ClauseScheduleKind::Auto:
    return llvm::omp::OMP_SCHEDULE_Auto;
  case omp::ClauseScheduleKind::Runtime:
    return llvm::omp::OMP_SCHEDULE_Runtime;
  }
  llvm_unreachable("unhandled schedule clause argument");
}

/// ModuleTranslation stack frame for OpenMP operations. This keeps track of the
/// insertion points for allocas.
class OpenMPAllocaStackFrame
    : public LLVM::ModuleTranslation::StackFrameBase<OpenMPAllocaStackFrame> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(OpenMPAllocaStackFrame)

  explicit OpenMPAllocaStackFrame(llvm::OpenMPIRBuilder::InsertPointTy allocaIP)
      : allocaInsertPoint(allocaIP) {}
  llvm::OpenMPIRBuilder::InsertPointTy allocaInsertPoint;
};

/// ModuleTranslation stack frame containing the partial mapping between MLIR
/// values and their LLVM IR equivalents.
class OpenMPVarMappingStackFrame
    : public LLVM::ModuleTranslation::StackFrameBase<
          OpenMPVarMappingStackFrame> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(OpenMPVarMappingStackFrame)

  explicit OpenMPVarMappingStackFrame(
      const DenseMap<Value, llvm::Value *> &mapping)
      : mapping(mapping) {}

  DenseMap<Value, llvm::Value *> mapping;
};
} // namespace

/// Find the insertion point for allocas given the current insertion point for
/// normal operations in the builder.
static llvm::OpenMPIRBuilder::InsertPointTy
findAllocaInsertPoint(llvm::IRBuilderBase &builder,
                      const LLVM::ModuleTranslation &moduleTranslation) {
  // If there is an alloca insertion point on stack, i.e. we are in a nested
  // operation and a specific point was provided by some surrounding operation,
  // use it.
  llvm::OpenMPIRBuilder::InsertPointTy allocaInsertPoint;
  WalkResult walkResult = moduleTranslation.stackWalk<OpenMPAllocaStackFrame>(
      [&](const OpenMPAllocaStackFrame &frame) {
        allocaInsertPoint = frame.allocaInsertPoint;
        return WalkResult::interrupt();
      });
  if (walkResult.wasInterrupted())
    return allocaInsertPoint;

  // Otherwise, insert to the entry block of the surrounding function.
  // If the current IRBuilder InsertPoint is the function's entry, it cannot
  // also be used for alloca insertion which would result in insertion order
  // confusion. Create a new BasicBlock for the Builder and use the entry block
  // for the allocs.
  // TODO: Create a dedicated alloca BasicBlock at function creation such that
  // we do not need to move the current InertPoint here.
  if (builder.GetInsertBlock() ==
      &builder.GetInsertBlock()->getParent()->getEntryBlock()) {
    assert(builder.GetInsertPoint() == builder.GetInsertBlock()->end() &&
           "Assuming end of basic block");
    llvm::BasicBlock *entryBB = llvm::BasicBlock::Create(
        builder.getContext(), "entry", builder.GetInsertBlock()->getParent(),
        builder.GetInsertBlock()->getNextNode());
    builder.CreateBr(entryBB);
    builder.SetInsertPoint(entryBB);
  }

  llvm::BasicBlock &funcEntryBlock =
      builder.GetInsertBlock()->getParent()->getEntryBlock();
  return llvm::OpenMPIRBuilder::InsertPointTy(
      &funcEntryBlock, funcEntryBlock.getFirstInsertionPt());
}

/// Converts the given region that appears within an OpenMP dialect operation to
/// LLVM IR, creating a branch from the `sourceBlock` to the entry block of the
/// region, and a branch from any block with an successor-less OpenMP terminator
/// to `continuationBlock`. Populates `continuationBlockPHIs` with the PHI nodes
/// of the continuation block if provided.
static llvm::BasicBlock *convertOmpOpRegions(
    Region &region, StringRef blockName, llvm::IRBuilderBase &builder,
    LLVM::ModuleTranslation &moduleTranslation, LogicalResult &bodyGenStatus,
    SmallVectorImpl<llvm::PHINode *> *continuationBlockPHIs = nullptr) {
  llvm::BasicBlock *continuationBlock =
      splitBB(builder, true, "omp.region.cont");
  llvm::BasicBlock *sourceBlock = builder.GetInsertBlock();

  llvm::LLVMContext &llvmContext = builder.getContext();
  for (Block &bb : region) {
    llvm::BasicBlock *llvmBB = llvm::BasicBlock::Create(
        llvmContext, blockName, builder.GetInsertBlock()->getParent(),
        builder.GetInsertBlock()->getNextNode());
    moduleTranslation.mapBlock(&bb, llvmBB);
  }

  llvm::Instruction *sourceTerminator = sourceBlock->getTerminator();

  // Terminators (namely YieldOp) may be forwarding values to the region that
  // need to be available in the continuation block. Collect the types of these
  // operands in preparation of creating PHI nodes.
  SmallVector<llvm::Type *> continuationBlockPHITypes;
  bool operandsProcessed = false;
  unsigned numYields = 0;
  for (Block &bb : region.getBlocks()) {
    if (omp::YieldOp yield = dyn_cast<omp::YieldOp>(bb.getTerminator())) {
      if (!operandsProcessed) {
        for (unsigned i = 0, e = yield->getNumOperands(); i < e; ++i) {
          continuationBlockPHITypes.push_back(
              moduleTranslation.convertType(yield->getOperand(i).getType()));
        }
        operandsProcessed = true;
      } else {
        assert(continuationBlockPHITypes.size() == yield->getNumOperands() &&
               "mismatching number of values yielded from the region");
        for (unsigned i = 0, e = yield->getNumOperands(); i < e; ++i) {
          llvm::Type *operandType =
              moduleTranslation.convertType(yield->getOperand(i).getType());
          (void)operandType;
          assert(continuationBlockPHITypes[i] == operandType &&
                 "values of mismatching types yielded from the region");
        }
      }
      numYields++;
    }
  }

  // Insert PHI nodes in the continuation block for any values forwarded by the
  // terminators in this region.
  if (!continuationBlockPHITypes.empty())
    assert(
        continuationBlockPHIs &&
        "expected continuation block PHIs if converted regions yield values");
  if (continuationBlockPHIs) {
    llvm::IRBuilderBase::InsertPointGuard guard(builder);
    continuationBlockPHIs->reserve(continuationBlockPHITypes.size());
    builder.SetInsertPoint(continuationBlock, continuationBlock->begin());
    for (llvm::Type *ty : continuationBlockPHITypes)
      continuationBlockPHIs->push_back(builder.CreatePHI(ty, numYields));
  }

  // Convert blocks one by one in topological order to ensure
  // defs are converted before uses.
  SetVector<Block *> blocks =
      LLVM::detail::getTopologicallySortedBlocks(region);
  for (Block *bb : blocks) {
    llvm::BasicBlock *llvmBB = moduleTranslation.lookupBlock(bb);
    // Retarget the branch of the entry block to the entry block of the
    // converted region (regions are single-entry).
    if (bb->isEntryBlock()) {
      assert(sourceTerminator->getNumSuccessors() == 1 &&
             "provided entry block has multiple successors");
      assert(sourceTerminator->getSuccessor(0) == continuationBlock &&
             "ContinuationBlock is not the successor of the entry block");
      sourceTerminator->setSuccessor(0, llvmBB);
    }

    llvm::IRBuilderBase::InsertPointGuard guard(builder);
    if (failed(
            moduleTranslation.convertBlock(*bb, bb->isEntryBlock(), builder))) {
      bodyGenStatus = failure();
      return continuationBlock;
    }

    // Special handling for `omp.yield` and `omp.terminator` (we may have more
    // than one): they return the control to the parent OpenMP dialect operation
    // so replace them with the branch to the continuation block. We handle this
    // here to avoid relying inter-function communication through the
    // ModuleTranslation class to set up the correct insertion point. This is
    // also consistent with MLIR's idiom of handling special region terminators
    // in the same code that handles the region-owning operation.
    Operation *terminator = bb->getTerminator();
    if (isa<omp::TerminatorOp, omp::YieldOp>(terminator)) {
      builder.CreateBr(continuationBlock);

      for (unsigned i = 0, e = terminator->getNumOperands(); i < e; ++i)
        (*continuationBlockPHIs)[i]->addIncoming(
            moduleTranslation.lookupValue(terminator->getOperand(i)), llvmBB);
    }
  }
  // After all blocks have been traversed and values mapped, connect the PHI
  // nodes to the results of preceding blocks.
  LLVM::detail::connectPHINodes(region, moduleTranslation);

  // Remove the blocks and values defined in this region from the mapping since
  // they are not visible outside of this region. This allows the same region to
  // be converted several times, that is cloned, without clashes, and slightly
  // speeds up the lookups.
  moduleTranslation.forgetMapping(region);

  return continuationBlock;
}

/// Convert ProcBindKind from MLIR-generated enum to LLVM enum.
static llvm::omp::ProcBindKind getProcBindKind(omp::ClauseProcBindKind kind) {
  switch (kind) {
  case omp::ClauseProcBindKind::Close:
    return llvm::omp::ProcBindKind::OMP_PROC_BIND_close;
  case omp::ClauseProcBindKind::Master:
    return llvm::omp::ProcBindKind::OMP_PROC_BIND_master;
  case omp::ClauseProcBindKind::Primary:
    return llvm::omp::ProcBindKind::OMP_PROC_BIND_primary;
  case omp::ClauseProcBindKind::Spread:
    return llvm::omp::ProcBindKind::OMP_PROC_BIND_spread;
  }
  llvm_unreachable("Unknown ClauseProcBindKind kind");
}

/// Converts the OpenMP parallel operation to LLVM IR.
static LogicalResult
convertOmpParallel(omp::ParallelOp opInst, llvm::IRBuilderBase &builder,
                   LLVM::ModuleTranslation &moduleTranslation) {
  using InsertPointTy = llvm::OpenMPIRBuilder::InsertPointTy;
  // TODO: support error propagation in OpenMPIRBuilder and use it instead of
  // relying on captured variables.
  LogicalResult bodyGenStatus = success();

  auto bodyGenCB = [&](InsertPointTy allocaIP, InsertPointTy codeGenIP) {
    // Save the alloca insertion point on ModuleTranslation stack for use in
    // nested regions.
    LLVM::ModuleTranslation::SaveStack<OpenMPAllocaStackFrame> frame(
        moduleTranslation, allocaIP);

    // ParallelOp has only one region associated with it.
    builder.restoreIP(codeGenIP);
    convertOmpOpRegions(opInst.getRegion(), "omp.par.region", builder,
                        moduleTranslation, bodyGenStatus);
  };

  // TODO: Perform appropriate actions according to the data-sharing
  // attribute (shared, private, firstprivate, ...) of variables.
  // Currently defaults to shared.
  auto privCB = [&](InsertPointTy allocaIP, InsertPointTy codeGenIP,
                    llvm::Value &, llvm::Value &vPtr,
                    llvm::Value *&replacementValue) -> InsertPointTy {
    replacementValue = &vPtr;

    return codeGenIP;
  };

  // TODO: Perform finalization actions for variables. This has to be
  // called for variables which have destructors/finalizers.
  auto finiCB = [&](InsertPointTy codeGenIP) {};

  llvm::Value *ifCond = nullptr;
  if (auto ifExprVar = opInst.getIfExprVar())
    ifCond = moduleTranslation.lookupValue(ifExprVar);
  llvm::Value *numThreads = nullptr;
  if (auto numThreadsVar = opInst.getNumThreadsVar())
    numThreads = moduleTranslation.lookupValue(numThreadsVar);
  auto pbKind = llvm::omp::OMP_PROC_BIND_default;
  if (auto bind = opInst.getProcBindVal())
    pbKind = getProcBindKind(*bind);
  // TODO: Is the Parallel construct cancellable?
  bool isCancellable = false;

  llvm::OpenMPIRBuilder::InsertPointTy allocaIP =
      findAllocaInsertPoint(builder, moduleTranslation);
  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  builder.restoreIP(moduleTranslation.getOpenMPBuilder()->createParallel(
      ompLoc, allocaIP, bodyGenCB, privCB, finiCB, ifCond, numThreads, pbKind,
      isCancellable));

  return bodyGenStatus;
}

/// Converts an OpenMP 'master' operation into LLVM IR using OpenMPIRBuilder.
static LogicalResult
convertOmpMaster(Operation &opInst, llvm::IRBuilderBase &builder,
                 LLVM::ModuleTranslation &moduleTranslation) {
  using InsertPointTy = llvm::OpenMPIRBuilder::InsertPointTy;
  // TODO: support error propagation in OpenMPIRBuilder and use it instead of
  // relying on captured variables.
  LogicalResult bodyGenStatus = success();

  auto bodyGenCB = [&](InsertPointTy allocaIP, InsertPointTy codeGenIP) {
    // MasterOp has only one region associated with it.
    auto &region = cast<omp::MasterOp>(opInst).getRegion();
    builder.restoreIP(codeGenIP);
    convertOmpOpRegions(region, "omp.master.region", builder, moduleTranslation,
                        bodyGenStatus);
  };

  // TODO: Perform finalization actions for variables. This has to be
  // called for variables which have destructors/finalizers.
  auto finiCB = [&](InsertPointTy codeGenIP) {};

  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  builder.restoreIP(moduleTranslation.getOpenMPBuilder()->createMaster(
      ompLoc, bodyGenCB, finiCB));
  return success();
}

/// Converts an OpenMP 'critical' operation into LLVM IR using OpenMPIRBuilder.
static LogicalResult
convertOmpCritical(Operation &opInst, llvm::IRBuilderBase &builder,
                   LLVM::ModuleTranslation &moduleTranslation) {
  using InsertPointTy = llvm::OpenMPIRBuilder::InsertPointTy;
  auto criticalOp = cast<omp::CriticalOp>(opInst);
  // TODO: support error propagation in OpenMPIRBuilder and use it instead of
  // relying on captured variables.
  LogicalResult bodyGenStatus = success();

  auto bodyGenCB = [&](InsertPointTy allocaIP, InsertPointTy codeGenIP) {
    // CriticalOp has only one region associated with it.
    auto &region = cast<omp::CriticalOp>(opInst).getRegion();
    builder.restoreIP(codeGenIP);
    convertOmpOpRegions(region, "omp.critical.region", builder,
                        moduleTranslation, bodyGenStatus);
  };

  // TODO: Perform finalization actions for variables. This has to be
  // called for variables which have destructors/finalizers.
  auto finiCB = [&](InsertPointTy codeGenIP) {};

  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  llvm::LLVMContext &llvmContext = moduleTranslation.getLLVMContext();
  llvm::Constant *hint = nullptr;

  // If it has a name, it probably has a hint too.
  if (criticalOp.getNameAttr()) {
    // The verifiers in OpenMP Dialect guarentee that all the pointers are
    // non-null
    auto symbolRef = cast<SymbolRefAttr>(criticalOp.getNameAttr());
    auto criticalDeclareOp =
        SymbolTable::lookupNearestSymbolFrom<omp::CriticalDeclareOp>(criticalOp,
                                                                     symbolRef);
    hint = llvm::ConstantInt::get(
        llvm::Type::getInt32Ty(llvmContext),
        static_cast<int>(criticalDeclareOp.getHintVal()));
  }
  builder.restoreIP(moduleTranslation.getOpenMPBuilder()->createCritical(
      ompLoc, bodyGenCB, finiCB, criticalOp.getName().value_or(""), hint));
  return success();
}

/// Returns a reduction declaration that corresponds to the given reduction
/// operation in the given container. Currently only supports reductions inside
/// WsLoopOp but can be easily extended.
static omp::ReductionDeclareOp findReductionDecl(omp::WsLoopOp container,
                                                 omp::ReductionOp reduction) {
  SymbolRefAttr reductionSymbol;
  for (unsigned i = 0, e = container.getNumReductionVars(); i < e; ++i) {
    if (container.getReductionVars()[i] != reduction.getAccumulator())
      continue;
    reductionSymbol = cast<SymbolRefAttr>((*container.getReductions())[i]);
    break;
  }
  assert(reductionSymbol &&
         "reduction operation must be associated with a declaration");

  return SymbolTable::lookupNearestSymbolFrom<omp::ReductionDeclareOp>(
      container, reductionSymbol);
}

/// Populates `reductions` with reduction declarations used in the given loop.
static void
collectReductionDecls(omp::WsLoopOp loop,
                      SmallVectorImpl<omp::ReductionDeclareOp> &reductions) {
  std::optional<ArrayAttr> attr = loop.getReductions();
  if (!attr)
    return;

  reductions.reserve(reductions.size() + loop.getNumReductionVars());
  for (auto symbolRef : attr->getAsRange<SymbolRefAttr>()) {
    reductions.push_back(
        SymbolTable::lookupNearestSymbolFrom<omp::ReductionDeclareOp>(
            loop, symbolRef));
  }
}

/// Translates the blocks contained in the given region and appends them to at
/// the current insertion point of `builder`. The operations of the entry block
/// are appended to the current insertion block, which is not expected to have a
/// terminator. If set, `continuationBlockArgs` is populated with translated
/// values that correspond to the values omp.yield'ed from the region.
static LogicalResult inlineConvertOmpRegions(
    Region &region, StringRef blockName, llvm::IRBuilderBase &builder,
    LLVM::ModuleTranslation &moduleTranslation,
    SmallVectorImpl<llvm::Value *> *continuationBlockArgs = nullptr) {
  if (region.empty())
    return success();

  // Special case for single-block regions that don't create additional blocks:
  // insert operations without creating additional blocks.
  if (llvm::hasSingleElement(region)) {
    moduleTranslation.mapBlock(&region.front(), builder.GetInsertBlock());
    if (failed(moduleTranslation.convertBlock(
            region.front(), /*ignoreArguments=*/true, builder)))
      return failure();

    // The continuation arguments are simply the translated terminator operands.
    if (continuationBlockArgs)
      llvm::append_range(
          *continuationBlockArgs,
          moduleTranslation.lookupValues(region.front().back().getOperands()));

    // Drop the mapping that is no longer necessary so that the same region can
    // be processed multiple times.
    moduleTranslation.forgetMapping(region);
    return success();
  }

  LogicalResult bodyGenStatus = success();
  SmallVector<llvm::PHINode *> phis;
  llvm::BasicBlock *continuationBlock = convertOmpOpRegions(
      region, blockName, builder, moduleTranslation, bodyGenStatus, &phis);
  if (failed(bodyGenStatus))
    return failure();
  if (continuationBlockArgs)
    llvm::append_range(*continuationBlockArgs, phis);
  builder.SetInsertPoint(continuationBlock,
                         continuationBlock->getFirstInsertionPt());
  return success();
}

namespace {
/// Owning equivalents of OpenMPIRBuilder::(Atomic)ReductionGen that are used to
/// store lambdas with capture.
using OwningReductionGen = std::function<llvm::OpenMPIRBuilder::InsertPointTy(
    llvm::OpenMPIRBuilder::InsertPointTy, llvm::Value *, llvm::Value *,
    llvm::Value *&)>;
using OwningAtomicReductionGen =
    std::function<llvm::OpenMPIRBuilder::InsertPointTy(
        llvm::OpenMPIRBuilder::InsertPointTy, llvm::Type *, llvm::Value *,
        llvm::Value *)>;
} // namespace

/// Create an OpenMPIRBuilder-compatible reduction generator for the given
/// reduction declaration. The generator uses `builder` but ignores its
/// insertion point.
static OwningReductionGen
makeReductionGen(omp::ReductionDeclareOp decl, llvm::IRBuilderBase &builder,
                 LLVM::ModuleTranslation &moduleTranslation) {
  // The lambda is mutable because we need access to non-const methods of decl
  // (which aren't actually mutating it), and we must capture decl by-value to
  // avoid the dangling reference after the parent function returns.
  OwningReductionGen gen =
      [&, decl](llvm::OpenMPIRBuilder::InsertPointTy insertPoint,
                llvm::Value *lhs, llvm::Value *rhs,
                llvm::Value *&result) mutable {
        Region &reductionRegion = decl.getReductionRegion();
        moduleTranslation.mapValue(reductionRegion.front().getArgument(0), lhs);
        moduleTranslation.mapValue(reductionRegion.front().getArgument(1), rhs);
        builder.restoreIP(insertPoint);
        SmallVector<llvm::Value *> phis;
        if (failed(inlineConvertOmpRegions(reductionRegion,
                                           "omp.reduction.nonatomic.body",
                                           builder, moduleTranslation, &phis)))
          return llvm::OpenMPIRBuilder::InsertPointTy();
        assert(phis.size() == 1);
        result = phis[0];
        return builder.saveIP();
      };
  return gen;
}

/// Create an OpenMPIRBuilder-compatible atomic reduction generator for the
/// given reduction declaration. The generator uses `builder` but ignores its
/// insertion point. Returns null if there is no atomic region available in the
/// reduction declaration.
static OwningAtomicReductionGen
makeAtomicReductionGen(omp::ReductionDeclareOp decl,
                       llvm::IRBuilderBase &builder,
                       LLVM::ModuleTranslation &moduleTranslation) {
  if (decl.getAtomicReductionRegion().empty())
    return OwningAtomicReductionGen();

  // The lambda is mutable because we need access to non-const methods of decl
  // (which aren't actually mutating it), and we must capture decl by-value to
  // avoid the dangling reference after the parent function returns.
  OwningAtomicReductionGen atomicGen =
      [&, decl](llvm::OpenMPIRBuilder::InsertPointTy insertPoint, llvm::Type *,
                llvm::Value *lhs, llvm::Value *rhs) mutable {
        Region &atomicRegion = decl.getAtomicReductionRegion();
        moduleTranslation.mapValue(atomicRegion.front().getArgument(0), lhs);
        moduleTranslation.mapValue(atomicRegion.front().getArgument(1), rhs);
        builder.restoreIP(insertPoint);
        SmallVector<llvm::Value *> phis;
        if (failed(inlineConvertOmpRegions(atomicRegion,
                                           "omp.reduction.atomic.body", builder,
                                           moduleTranslation, &phis)))
          return llvm::OpenMPIRBuilder::InsertPointTy();
        assert(phis.empty());
        return builder.saveIP();
      };
  return atomicGen;
}

/// Converts an OpenMP 'ordered' operation into LLVM IR using OpenMPIRBuilder.
static LogicalResult
convertOmpOrdered(Operation &opInst, llvm::IRBuilderBase &builder,
                  LLVM::ModuleTranslation &moduleTranslation) {
  auto orderedOp = cast<omp::OrderedOp>(opInst);

  omp::ClauseDepend dependType = *orderedOp.getDependTypeVal();
  bool isDependSource = dependType == omp::ClauseDepend::dependsource;
  unsigned numLoops = *orderedOp.getNumLoopsVal();
  SmallVector<llvm::Value *> vecValues =
      moduleTranslation.lookupValues(orderedOp.getDependVecVars());

  size_t indexVecValues = 0;
  while (indexVecValues < vecValues.size()) {
    SmallVector<llvm::Value *> storeValues;
    storeValues.reserve(numLoops);
    for (unsigned i = 0; i < numLoops; i++) {
      storeValues.push_back(vecValues[indexVecValues]);
      indexVecValues++;
    }
    llvm::OpenMPIRBuilder::InsertPointTy allocaIP =
        findAllocaInsertPoint(builder, moduleTranslation);
    llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
    builder.restoreIP(moduleTranslation.getOpenMPBuilder()->createOrderedDepend(
        ompLoc, allocaIP, numLoops, storeValues, ".cnt.addr", isDependSource));
  }
  return success();
}

/// Converts an OpenMP 'ordered_region' operation into LLVM IR using
/// OpenMPIRBuilder.
static LogicalResult
convertOmpOrderedRegion(Operation &opInst, llvm::IRBuilderBase &builder,
                        LLVM::ModuleTranslation &moduleTranslation) {
  using InsertPointTy = llvm::OpenMPIRBuilder::InsertPointTy;
  auto orderedRegionOp = cast<omp::OrderedRegionOp>(opInst);

  // TODO: The code generation for ordered simd directive is not supported yet.
  if (orderedRegionOp.getSimd())
    return failure();

  // TODO: support error propagation in OpenMPIRBuilder and use it instead of
  // relying on captured variables.
  LogicalResult bodyGenStatus = success();

  auto bodyGenCB = [&](InsertPointTy allocaIP, InsertPointTy codeGenIP) {
    // OrderedOp has only one region associated with it.
    auto &region = cast<omp::OrderedRegionOp>(opInst).getRegion();
    builder.restoreIP(codeGenIP);
    convertOmpOpRegions(region, "omp.ordered.region", builder,
                        moduleTranslation, bodyGenStatus);
  };

  // TODO: Perform finalization actions for variables. This has to be
  // called for variables which have destructors/finalizers.
  auto finiCB = [&](InsertPointTy codeGenIP) {};

  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  builder.restoreIP(
      moduleTranslation.getOpenMPBuilder()->createOrderedThreadsSimd(
          ompLoc, bodyGenCB, finiCB, !orderedRegionOp.getSimd()));
  return bodyGenStatus;
}

static LogicalResult
convertOmpSections(Operation &opInst, llvm::IRBuilderBase &builder,
                   LLVM::ModuleTranslation &moduleTranslation) {
  using InsertPointTy = llvm::OpenMPIRBuilder::InsertPointTy;
  using StorableBodyGenCallbackTy =
      llvm::OpenMPIRBuilder::StorableBodyGenCallbackTy;

  auto sectionsOp = cast<omp::SectionsOp>(opInst);

  // TODO: Support the following clauses: private, firstprivate, lastprivate,
  // reduction, allocate
  if (!sectionsOp.getReductionVars().empty() || sectionsOp.getReductions() ||
      !sectionsOp.getAllocateVars().empty() ||
      !sectionsOp.getAllocatorsVars().empty())
    return emitError(sectionsOp.getLoc())
           << "reduction and allocate clauses are not supported for sections "
              "construct";

  LogicalResult bodyGenStatus = success();
  SmallVector<StorableBodyGenCallbackTy> sectionCBs;

  for (Operation &op : *sectionsOp.getRegion().begin()) {
    auto sectionOp = dyn_cast<omp::SectionOp>(op);
    if (!sectionOp) // omp.terminator
      continue;

    Region &region = sectionOp.getRegion();
    auto sectionCB = [&region, &builder, &moduleTranslation, &bodyGenStatus](
                         InsertPointTy allocaIP, InsertPointTy codeGenIP) {
      builder.restoreIP(codeGenIP);
      convertOmpOpRegions(region, "omp.section.region", builder,
                          moduleTranslation, bodyGenStatus);
    };
    sectionCBs.push_back(sectionCB);
  }

  // No sections within omp.sections operation - skip generation. This situation
  // is only possible if there is only a terminator operation inside the
  // sections operation
  if (sectionCBs.empty())
    return success();

  assert(isa<omp::SectionOp>(*sectionsOp.getRegion().op_begin()));

  // TODO: Perform appropriate actions according to the data-sharing
  // attribute (shared, private, firstprivate, ...) of variables.
  // Currently defaults to shared.
  auto privCB = [&](InsertPointTy, InsertPointTy codeGenIP, llvm::Value &,
                    llvm::Value &vPtr,
                    llvm::Value *&replacementValue) -> InsertPointTy {
    replacementValue = &vPtr;
    return codeGenIP;
  };

  // TODO: Perform finalization actions for variables. This has to be
  // called for variables which have destructors/finalizers.
  auto finiCB = [&](InsertPointTy codeGenIP) {};

  llvm::OpenMPIRBuilder::InsertPointTy allocaIP =
      findAllocaInsertPoint(builder, moduleTranslation);
  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  builder.restoreIP(moduleTranslation.getOpenMPBuilder()->createSections(
      ompLoc, allocaIP, sectionCBs, privCB, finiCB, false,
      sectionsOp.getNowait()));
  return bodyGenStatus;
}

/// Converts an OpenMP single construct into LLVM IR using OpenMPIRBuilder.
static LogicalResult
convertOmpSingle(omp::SingleOp &singleOp, llvm::IRBuilderBase &builder,
                 LLVM::ModuleTranslation &moduleTranslation) {
  using InsertPointTy = llvm::OpenMPIRBuilder::InsertPointTy;
  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  LogicalResult bodyGenStatus = success();
  auto bodyCB = [&](InsertPointTy allocaIP, InsertPointTy codegenIP) {
    builder.restoreIP(codegenIP);
    convertOmpOpRegions(singleOp.getRegion(), "omp.single.region", builder,
                        moduleTranslation, bodyGenStatus);
  };
  auto finiCB = [&](InsertPointTy codeGenIP) {};
  builder.restoreIP(moduleTranslation.getOpenMPBuilder()->createSingle(
      ompLoc, bodyCB, finiCB, singleOp.getNowait(), /*DidIt=*/nullptr));
  return bodyGenStatus;
}

/// Converts an OpenMP task construct into LLVM IR using OpenMPIRBuilder.
static LogicalResult
convertOmpTaskOp(omp::TaskOp taskOp, llvm::IRBuilderBase &builder,
                 LLVM::ModuleTranslation &moduleTranslation) {
  using InsertPointTy = llvm::OpenMPIRBuilder::InsertPointTy;
  LogicalResult bodyGenStatus = success();
  if (taskOp.getIfExpr() || taskOp.getFinalExpr() || taskOp.getUntiedAttr() ||
      taskOp.getMergeableAttr() || taskOp.getInReductions() ||
      taskOp.getPriority() || !taskOp.getAllocateVars().empty()) {
    return taskOp.emitError("unhandled clauses for translation to LLVM IR");
  }
  auto bodyCB = [&](InsertPointTy allocaIP, InsertPointTy codegenIP) {
    // Save the alloca insertion point on ModuleTranslation stack for use in
    // nested regions.
    LLVM::ModuleTranslation::SaveStack<OpenMPAllocaStackFrame> frame(
        moduleTranslation, allocaIP);

    builder.restoreIP(codegenIP);
    convertOmpOpRegions(taskOp.getRegion(), "omp.task.region", builder,
                        moduleTranslation, bodyGenStatus);
  };

  SmallVector<llvm::OpenMPIRBuilder::DependData> dds;
  if (!taskOp.getDependVars().empty() && taskOp.getDepends()) {
    for (auto dep :
         llvm::zip(taskOp.getDependVars(), taskOp.getDepends()->getValue())) {
      llvm::omp::RTLDependenceKindTy type;
      switch (
          cast<mlir::omp::ClauseTaskDependAttr>(std::get<1>(dep)).getValue()) {
      case mlir::omp::ClauseTaskDepend::taskdependin:
        type = llvm::omp::RTLDependenceKindTy::DepIn;
        break;
      // The OpenMP runtime requires that the codegen for 'depend' clause for
      // 'out' dependency kind must be the same as codegen for 'depend' clause
      // with 'inout' dependency.
      case mlir::omp::ClauseTaskDepend::taskdependout:
      case mlir::omp::ClauseTaskDepend::taskdependinout:
        type = llvm::omp::RTLDependenceKindTy::DepInOut;
        break;
      };
      llvm::Value *depVal = moduleTranslation.lookupValue(std::get<0>(dep));
      llvm::OpenMPIRBuilder::DependData dd(type, depVal->getType(), depVal);
      dds.emplace_back(dd);
    }
  }

  llvm::OpenMPIRBuilder::InsertPointTy allocaIP =
      findAllocaInsertPoint(builder, moduleTranslation);
  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  builder.restoreIP(moduleTranslation.getOpenMPBuilder()->createTask(
      ompLoc, allocaIP, bodyCB, !taskOp.getUntied(), /*Final*/ nullptr,
      /*IfCondition*/ nullptr, dds));
  return bodyGenStatus;
}

/// Converts an OpenMP taskgroup construct into LLVM IR using OpenMPIRBuilder.
static LogicalResult
convertOmpTaskgroupOp(omp::TaskGroupOp tgOp, llvm::IRBuilderBase &builder,
                      LLVM::ModuleTranslation &moduleTranslation) {
  using InsertPointTy = llvm::OpenMPIRBuilder::InsertPointTy;
  LogicalResult bodyGenStatus = success();
  if (!tgOp.getTaskReductionVars().empty() || !tgOp.getAllocateVars().empty()) {
    return tgOp.emitError("unhandled clauses for translation to LLVM IR");
  }
  auto bodyCB = [&](InsertPointTy allocaIP, InsertPointTy codegenIP) {
    builder.restoreIP(codegenIP);
    convertOmpOpRegions(tgOp.getRegion(), "omp.taskgroup.region", builder,
                        moduleTranslation, bodyGenStatus);
  };
  InsertPointTy allocaIP = findAllocaInsertPoint(builder, moduleTranslation);
  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  builder.restoreIP(moduleTranslation.getOpenMPBuilder()->createTaskgroup(
      ompLoc, allocaIP, bodyCB));
  return bodyGenStatus;
}

/// Converts an OpenMP workshare loop into LLVM IR using OpenMPIRBuilder.
static LogicalResult
convertOmpWsLoop(Operation &opInst, llvm::IRBuilderBase &builder,
                 LLVM::ModuleTranslation &moduleTranslation) {
  auto loop = cast<omp::WsLoopOp>(opInst);
  // TODO: this should be in the op verifier instead.
  if (loop.getLowerBound().empty())
    return failure();

  // Static is the default.
  auto schedule =
      loop.getScheduleVal().value_or(omp::ClauseScheduleKind::Static);

  // Find the loop configuration.
  llvm::Value *step = moduleTranslation.lookupValue(loop.getStep()[0]);
  llvm::Type *ivType = step->getType();
  llvm::Value *chunk = nullptr;
  if (loop.getScheduleChunkVar()) {
    llvm::Value *chunkVar =
        moduleTranslation.lookupValue(loop.getScheduleChunkVar());
    chunk = builder.CreateSExtOrTrunc(chunkVar, ivType);
  }

  SmallVector<omp::ReductionDeclareOp> reductionDecls;
  collectReductionDecls(loop, reductionDecls);
  llvm::OpenMPIRBuilder::InsertPointTy allocaIP =
      findAllocaInsertPoint(builder, moduleTranslation);

  // Allocate space for privatized reduction variables.
  SmallVector<llvm::Value *> privateReductionVariables;
  DenseMap<Value, llvm::Value *> reductionVariableMap;
  unsigned numReductions = loop.getNumReductionVars();
  privateReductionVariables.reserve(numReductions);
  if (numReductions != 0) {
    llvm::IRBuilderBase::InsertPointGuard guard(builder);
    builder.restoreIP(allocaIP);
    for (unsigned i = 0; i < numReductions; ++i) {
      llvm::Value *var = builder.CreateAlloca(
          moduleTranslation.convertType(reductionDecls[i].getType()));
      privateReductionVariables.push_back(var);
      reductionVariableMap.try_emplace(loop.getReductionVars()[i], var);
    }
  }

  // Store the mapping between reduction variables and their private copies on
  // ModuleTranslation stack. It can be then recovered when translating
  // omp.reduce operations in a separate call.
  LLVM::ModuleTranslation::SaveStack<OpenMPVarMappingStackFrame> mappingGuard(
      moduleTranslation, reductionVariableMap);

  // Before the loop, store the initial values of reductions into reduction
  // variables. Although this could be done after allocas, we don't want to mess
  // up with the alloca insertion point.
  for (unsigned i = 0; i < numReductions; ++i) {
    SmallVector<llvm::Value *> phis;
    if (failed(inlineConvertOmpRegions(reductionDecls[i].getInitializerRegion(),
                                       "omp.reduction.neutral", builder,
                                       moduleTranslation, &phis)))
      return failure();
    assert(phis.size() == 1 && "expected one value to be yielded from the "
                               "reduction neutral element declaration region");
    builder.CreateStore(phis[0], privateReductionVariables[i]);
  }

  // Set up the source location value for OpenMP runtime.
  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);

  // Generator of the canonical loop body.
  // TODO: support error propagation in OpenMPIRBuilder and use it instead of
  // relying on captured variables.
  SmallVector<llvm::CanonicalLoopInfo *> loopInfos;
  SmallVector<llvm::OpenMPIRBuilder::InsertPointTy> bodyInsertPoints;
  LogicalResult bodyGenStatus = success();
  auto bodyGen = [&](llvm::OpenMPIRBuilder::InsertPointTy ip, llvm::Value *iv) {
    // Make sure further conversions know about the induction variable.
    moduleTranslation.mapValue(
        loop.getRegion().front().getArgument(loopInfos.size()), iv);

    // Capture the body insertion point for use in nested loops. BodyIP of the
    // CanonicalLoopInfo always points to the beginning of the entry block of
    // the body.
    bodyInsertPoints.push_back(ip);

    if (loopInfos.size() != loop.getNumLoops() - 1)
      return;

    // Convert the body of the loop.
    builder.restoreIP(ip);
    convertOmpOpRegions(loop.getRegion(), "omp.wsloop.region", builder,
                        moduleTranslation, bodyGenStatus);
  };

  // Delegate actual loop construction to the OpenMP IRBuilder.
  // TODO: this currently assumes WsLoop is semantically similar to SCF loop,
  // i.e. it has a positive step, uses signed integer semantics. Reconsider
  // this code when WsLoop clearly supports more cases.
  llvm::OpenMPIRBuilder *ompBuilder = moduleTranslation.getOpenMPBuilder();
  for (unsigned i = 0, e = loop.getNumLoops(); i < e; ++i) {
    llvm::Value *lowerBound =
        moduleTranslation.lookupValue(loop.getLowerBound()[i]);
    llvm::Value *upperBound =
        moduleTranslation.lookupValue(loop.getUpperBound()[i]);
    llvm::Value *step = moduleTranslation.lookupValue(loop.getStep()[i]);

    // Make sure loop trip count are emitted in the preheader of the outermost
    // loop at the latest so that they are all available for the new collapsed
    // loop will be created below.
    llvm::OpenMPIRBuilder::LocationDescription loc = ompLoc;
    llvm::OpenMPIRBuilder::InsertPointTy computeIP = ompLoc.IP;
    if (i != 0) {
      loc = llvm::OpenMPIRBuilder::LocationDescription(bodyInsertPoints.back());
      computeIP = loopInfos.front()->getPreheaderIP();
    }
    loopInfos.push_back(ompBuilder->createCanonicalLoop(
        loc, bodyGen, lowerBound, upperBound, step,
        /*IsSigned=*/true, loop.getInclusive(), computeIP));

    if (failed(bodyGenStatus))
      return failure();
  }

  // Collapse loops. Store the insertion point because LoopInfos may get
  // invalidated.
  llvm::IRBuilderBase::InsertPoint afterIP = loopInfos.front()->getAfterIP();
  llvm::CanonicalLoopInfo *loopInfo =
      ompBuilder->collapseLoops(ompLoc.DL, loopInfos, {});

  allocaIP = findAllocaInsertPoint(builder, moduleTranslation);

  // TODO: Handle doacross loops when the ordered clause has a parameter.
  bool isOrdered = loop.getOrderedVal().has_value();
  std::optional<omp::ScheduleModifier> scheduleModifier =
      loop.getScheduleModifier();
  bool isSimd = loop.getSimdModifier();

  ompBuilder->applyWorkshareLoop(
      ompLoc.DL, loopInfo, allocaIP, !loop.getNowait(),
      convertToScheduleKind(schedule), chunk, isSimd,
      scheduleModifier == omp::ScheduleModifier::monotonic,
      scheduleModifier == omp::ScheduleModifier::nonmonotonic, isOrdered);

  // Continue building IR after the loop. Note that the LoopInfo returned by
  // `collapseLoops` points inside the outermost loop and is intended for
  // potential further loop transformations. Use the insertion point stored
  // before collapsing loops instead.
  builder.restoreIP(afterIP);

  // Process the reductions if required.
  if (numReductions == 0)
    return success();

  // Create the reduction generators. We need to own them here because
  // ReductionInfo only accepts references to the generators.
  SmallVector<OwningReductionGen> owningReductionGens;
  SmallVector<OwningAtomicReductionGen> owningAtomicReductionGens;
  for (unsigned i = 0; i < numReductions; ++i) {
    owningReductionGens.push_back(
        makeReductionGen(reductionDecls[i], builder, moduleTranslation));
    owningAtomicReductionGens.push_back(
        makeAtomicReductionGen(reductionDecls[i], builder, moduleTranslation));
  }

  // Collect the reduction information.
  SmallVector<llvm::OpenMPIRBuilder::ReductionInfo> reductionInfos;
  reductionInfos.reserve(numReductions);
  for (unsigned i = 0; i < numReductions; ++i) {
    llvm::OpenMPIRBuilder::AtomicReductionGenTy atomicGen = nullptr;
    if (owningAtomicReductionGens[i])
      atomicGen = owningAtomicReductionGens[i];
    llvm::Value *variable =
        moduleTranslation.lookupValue(loop.getReductionVars()[i]);
    reductionInfos.push_back(
        {moduleTranslation.convertType(reductionDecls[i].getType()), variable,
         privateReductionVariables[i], owningReductionGens[i], atomicGen});
  }

  // The call to createReductions below expects the block to have a
  // terminator. Create an unreachable instruction to serve as terminator
  // and remove it later.
  llvm::UnreachableInst *tempTerminator = builder.CreateUnreachable();
  builder.SetInsertPoint(tempTerminator);
  llvm::OpenMPIRBuilder::InsertPointTy contInsertPoint =
      ompBuilder->createReductions(builder.saveIP(), allocaIP, reductionInfos,
                                   loop.getNowait());
  if (!contInsertPoint.getBlock())
    return loop->emitOpError() << "failed to convert reductions";
  auto nextInsertionPoint =
      ompBuilder->createBarrier(contInsertPoint, llvm::omp::OMPD_for);
  tempTerminator->eraseFromParent();
  builder.restoreIP(nextInsertionPoint);

  return success();
}

/// Converts an OpenMP simd loop into LLVM IR using OpenMPIRBuilder.
static LogicalResult
convertOmpSimdLoop(Operation &opInst, llvm::IRBuilderBase &builder,
                   LLVM::ModuleTranslation &moduleTranslation) {
  auto loop = cast<omp::SimdLoopOp>(opInst);

  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);

  // Generator of the canonical loop body.
  // TODO: support error propagation in OpenMPIRBuilder and use it instead of
  // relying on captured variables.
  SmallVector<llvm::CanonicalLoopInfo *> loopInfos;
  SmallVector<llvm::OpenMPIRBuilder::InsertPointTy> bodyInsertPoints;
  LogicalResult bodyGenStatus = success();
  auto bodyGen = [&](llvm::OpenMPIRBuilder::InsertPointTy ip, llvm::Value *iv) {
    // Make sure further conversions know about the induction variable.
    moduleTranslation.mapValue(
        loop.getRegion().front().getArgument(loopInfos.size()), iv);

    // Capture the body insertion point for use in nested loops. BodyIP of the
    // CanonicalLoopInfo always points to the beginning of the entry block of
    // the body.
    bodyInsertPoints.push_back(ip);

    if (loopInfos.size() != loop.getNumLoops() - 1)
      return;

    // Convert the body of the loop.
    builder.restoreIP(ip);
    convertOmpOpRegions(loop.getRegion(), "omp.simdloop.region", builder,
                        moduleTranslation, bodyGenStatus);
  };

  // Delegate actual loop construction to the OpenMP IRBuilder.
  // TODO: this currently assumes SimdLoop is semantically similar to SCF loop,
  // i.e. it has a positive step, uses signed integer semantics. Reconsider
  // this code when SimdLoop clearly supports more cases.
  llvm::OpenMPIRBuilder *ompBuilder = moduleTranslation.getOpenMPBuilder();
  for (unsigned i = 0, e = loop.getNumLoops(); i < e; ++i) {
    llvm::Value *lowerBound =
        moduleTranslation.lookupValue(loop.getLowerBound()[i]);
    llvm::Value *upperBound =
        moduleTranslation.lookupValue(loop.getUpperBound()[i]);
    llvm::Value *step = moduleTranslation.lookupValue(loop.getStep()[i]);

    // Make sure loop trip count are emitted in the preheader of the outermost
    // loop at the latest so that they are all available for the new collapsed
    // loop will be created below.
    llvm::OpenMPIRBuilder::LocationDescription loc = ompLoc;
    llvm::OpenMPIRBuilder::InsertPointTy computeIP = ompLoc.IP;
    if (i != 0) {
      loc = llvm::OpenMPIRBuilder::LocationDescription(bodyInsertPoints.back(),
                                                       ompLoc.DL);
      computeIP = loopInfos.front()->getPreheaderIP();
    }
    loopInfos.push_back(ompBuilder->createCanonicalLoop(
        loc, bodyGen, lowerBound, upperBound, step,
        /*IsSigned=*/true, /*Inclusive=*/true, computeIP));

    if (failed(bodyGenStatus))
      return failure();
  }

  // Collapse loops.
  llvm::IRBuilderBase::InsertPoint afterIP = loopInfos.front()->getAfterIP();
  llvm::CanonicalLoopInfo *loopInfo =
      ompBuilder->collapseLoops(ompLoc.DL, loopInfos, {});

  llvm::ConstantInt *simdlen = nullptr;
  if (std::optional<uint64_t> simdlenVar = loop.getSimdlen())
    simdlen = builder.getInt64(simdlenVar.value());

  llvm::ConstantInt *safelen = nullptr;
  if (std::optional<uint64_t> safelenVar = loop.getSafelen())
    safelen = builder.getInt64(safelenVar.value());

  llvm::MapVector<llvm::Value *, llvm::Value *> alignedVars;
  ompBuilder->applySimd(
      loopInfo, alignedVars,
      loop.getIfExpr() ? moduleTranslation.lookupValue(loop.getIfExpr())
                       : nullptr,
      llvm::omp::OrderKind::OMP_ORDER_unknown, simdlen, safelen);

  builder.restoreIP(afterIP);
  return success();
}

/// Convert an Atomic Ordering attribute to llvm::AtomicOrdering.
llvm::AtomicOrdering
convertAtomicOrdering(std::optional<omp::ClauseMemoryOrderKind> ao) {
  if (!ao)
    return llvm::AtomicOrdering::Monotonic; // Default Memory Ordering

  switch (*ao) {
  case omp::ClauseMemoryOrderKind::Seq_cst:
    return llvm::AtomicOrdering::SequentiallyConsistent;
  case omp::ClauseMemoryOrderKind::Acq_rel:
    return llvm::AtomicOrdering::AcquireRelease;
  case omp::ClauseMemoryOrderKind::Acquire:
    return llvm::AtomicOrdering::Acquire;
  case omp::ClauseMemoryOrderKind::Release:
    return llvm::AtomicOrdering::Release;
  case omp::ClauseMemoryOrderKind::Relaxed:
    return llvm::AtomicOrdering::Monotonic;
  }
  llvm_unreachable("Unknown ClauseMemoryOrderKind kind");
}

/// Convert omp.atomic.read operation to LLVM IR.
static LogicalResult
convertOmpAtomicRead(Operation &opInst, llvm::IRBuilderBase &builder,
                     LLVM::ModuleTranslation &moduleTranslation) {

  auto readOp = cast<omp::AtomicReadOp>(opInst);
  llvm::OpenMPIRBuilder *ompBuilder = moduleTranslation.getOpenMPBuilder();

  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);

  llvm::AtomicOrdering AO = convertAtomicOrdering(readOp.getMemoryOrderVal());
  llvm::Value *x = moduleTranslation.lookupValue(readOp.getX());
  llvm::Value *v = moduleTranslation.lookupValue(readOp.getV());

  llvm::Type *elementType =
      moduleTranslation.convertType(readOp.getElementType());

  llvm::OpenMPIRBuilder::AtomicOpValue V = {v, elementType, false, false};
  llvm::OpenMPIRBuilder::AtomicOpValue X = {x, elementType, false, false};
  builder.restoreIP(ompBuilder->createAtomicRead(ompLoc, X, V, AO));
  return success();
}

/// Converts an omp.atomic.write operation to LLVM IR.
static LogicalResult
convertOmpAtomicWrite(Operation &opInst, llvm::IRBuilderBase &builder,
                      LLVM::ModuleTranslation &moduleTranslation) {
  auto writeOp = cast<omp::AtomicWriteOp>(opInst);
  llvm::OpenMPIRBuilder *ompBuilder = moduleTranslation.getOpenMPBuilder();

  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  llvm::AtomicOrdering ao = convertAtomicOrdering(writeOp.getMemoryOrderVal());
  llvm::Value *expr = moduleTranslation.lookupValue(writeOp.getValue());
  llvm::Value *dest = moduleTranslation.lookupValue(writeOp.getAddress());
  llvm::Type *ty = moduleTranslation.convertType(writeOp.getValue().getType());
  llvm::OpenMPIRBuilder::AtomicOpValue x = {dest, ty, /*isSigned=*/false,
                                            /*isVolatile=*/false};
  builder.restoreIP(ompBuilder->createAtomicWrite(ompLoc, x, expr, ao));
  return success();
}

/// Converts an LLVM dialect binary operation to the corresponding enum value
/// for `atomicrmw` supported binary operation.
llvm::AtomicRMWInst::BinOp convertBinOpToAtomic(Operation &op) {
  return llvm::TypeSwitch<Operation *, llvm::AtomicRMWInst::BinOp>(&op)
      .Case([&](LLVM::AddOp) { return llvm::AtomicRMWInst::BinOp::Add; })
      .Case([&](LLVM::SubOp) { return llvm::AtomicRMWInst::BinOp::Sub; })
      .Case([&](LLVM::AndOp) { return llvm::AtomicRMWInst::BinOp::And; })
      .Case([&](LLVM::OrOp) { return llvm::AtomicRMWInst::BinOp::Or; })
      .Case([&](LLVM::XOrOp) { return llvm::AtomicRMWInst::BinOp::Xor; })
      .Case([&](LLVM::UMaxOp) { return llvm::AtomicRMWInst::BinOp::UMax; })
      .Case([&](LLVM::UMinOp) { return llvm::AtomicRMWInst::BinOp::UMin; })
      .Case([&](LLVM::FAddOp) { return llvm::AtomicRMWInst::BinOp::FAdd; })
      .Case([&](LLVM::FSubOp) { return llvm::AtomicRMWInst::BinOp::FSub; })
      .Default(llvm::AtomicRMWInst::BinOp::BAD_BINOP);
}

/// Converts an OpenMP atomic update operation using OpenMPIRBuilder.
static LogicalResult
convertOmpAtomicUpdate(omp::AtomicUpdateOp &opInst,
                       llvm::IRBuilderBase &builder,
                       LLVM::ModuleTranslation &moduleTranslation) {
  llvm::OpenMPIRBuilder *ompBuilder = moduleTranslation.getOpenMPBuilder();

  // Convert values and types.
  auto &innerOpList = opInst.getRegion().front().getOperations();
  if (innerOpList.size() != 2)
    return opInst.emitError("exactly two operations are allowed inside an "
                            "atomic update region while lowering to LLVM IR");

  Operation &innerUpdateOp = innerOpList.front();

  if (innerUpdateOp.getNumOperands() != 2 ||
      !llvm::is_contained(innerUpdateOp.getOperands(),
                          opInst.getRegion().getArgument(0)))
    return opInst.emitError(
        "the update operation inside the region must be a binary operation and "
        "that update operation must have the region argument as an operand");

  llvm::AtomicRMWInst::BinOp binop = convertBinOpToAtomic(innerUpdateOp);

  bool isXBinopExpr =
      innerUpdateOp.getNumOperands() > 0 &&
      innerUpdateOp.getOperand(0) == opInst.getRegion().getArgument(0);

  mlir::Value mlirExpr = (isXBinopExpr ? innerUpdateOp.getOperand(1)
                                       : innerUpdateOp.getOperand(0));
  llvm::Value *llvmExpr = moduleTranslation.lookupValue(mlirExpr);
  llvm::Value *llvmX = moduleTranslation.lookupValue(opInst.getX());
  llvm::Type *llvmXElementType = moduleTranslation.convertType(
      opInst.getRegion().getArgument(0).getType());
  llvm::OpenMPIRBuilder::AtomicOpValue llvmAtomicX = {llvmX, llvmXElementType,
                                                      /*isSigned=*/false,
                                                      /*isVolatile=*/false};

  llvm::AtomicOrdering atomicOrdering =
      convertAtomicOrdering(opInst.getMemoryOrderVal());

  // Generate update code.
  LogicalResult updateGenStatus = success();
  auto updateFn = [&opInst, &moduleTranslation, &updateGenStatus](
                      llvm::Value *atomicx,
                      llvm::IRBuilder<> &builder) -> llvm::Value * {
    Block &bb = *opInst.getRegion().begin();
    moduleTranslation.mapValue(*opInst.getRegion().args_begin(), atomicx);
    moduleTranslation.mapBlock(&bb, builder.GetInsertBlock());
    if (failed(moduleTranslation.convertBlock(bb, true, builder))) {
      updateGenStatus = (opInst.emitError()
                         << "unable to convert update operation to llvm IR");
      return nullptr;
    }
    omp::YieldOp yieldop = dyn_cast<omp::YieldOp>(bb.getTerminator());
    assert(yieldop && yieldop.getResults().size() == 1 &&
           "terminator must be omp.yield op and it must have exactly one "
           "argument");
    return moduleTranslation.lookupValue(yieldop.getResults()[0]);
  };

  // Handle ambiguous alloca, if any.
  auto allocaIP = findAllocaInsertPoint(builder, moduleTranslation);
  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  builder.restoreIP(ompBuilder->createAtomicUpdate(
      ompLoc, allocaIP, llvmAtomicX, llvmExpr, atomicOrdering, binop, updateFn,
      isXBinopExpr));
  return updateGenStatus;
}

static LogicalResult
convertOmpAtomicCapture(omp::AtomicCaptureOp atomicCaptureOp,
                        llvm::IRBuilderBase &builder,
                        LLVM::ModuleTranslation &moduleTranslation) {
  llvm::OpenMPIRBuilder *ompBuilder = moduleTranslation.getOpenMPBuilder();
  mlir::Value mlirExpr;
  bool isXBinopExpr = false, isPostfixUpdate = false;
  llvm::AtomicRMWInst::BinOp binop = llvm::AtomicRMWInst::BinOp::BAD_BINOP;

  omp::AtomicUpdateOp atomicUpdateOp = atomicCaptureOp.getAtomicUpdateOp();
  omp::AtomicWriteOp atomicWriteOp = atomicCaptureOp.getAtomicWriteOp();

  assert((atomicUpdateOp || atomicWriteOp) &&
         "internal op must be an atomic.update or atomic.write op");

  if (atomicWriteOp) {
    isPostfixUpdate = true;
    mlirExpr = atomicWriteOp.getValue();
  } else {
    isPostfixUpdate = atomicCaptureOp.getSecondOp() ==
                      atomicCaptureOp.getAtomicUpdateOp().getOperation();
    auto &innerOpList = atomicUpdateOp.getRegion().front().getOperations();
    if (innerOpList.size() != 2)
      return atomicUpdateOp.emitError(
          "exactly two operations are allowed inside an "
          "atomic update region while lowering to LLVM IR");
    Operation *innerUpdateOp = atomicUpdateOp.getFirstOp();
    if (innerUpdateOp->getNumOperands() != 2 ||
        !llvm::is_contained(innerUpdateOp->getOperands(),
                            atomicUpdateOp.getRegion().getArgument(0)))
      return atomicUpdateOp.emitError(
          "the update operation inside the region must be a binary operation "
          "and that update operation must have the region argument as an "
          "operand");
    binop = convertBinOpToAtomic(*innerUpdateOp);

    isXBinopExpr = innerUpdateOp->getOperand(0) ==
                   atomicUpdateOp.getRegion().getArgument(0);

    mlirExpr = (isXBinopExpr ? innerUpdateOp->getOperand(1)
                             : innerUpdateOp->getOperand(0));
  }

  llvm::Value *llvmExpr = moduleTranslation.lookupValue(mlirExpr);
  llvm::Value *llvmX =
      moduleTranslation.lookupValue(atomicCaptureOp.getAtomicReadOp().getX());
  llvm::Value *llvmV =
      moduleTranslation.lookupValue(atomicCaptureOp.getAtomicReadOp().getV());
  llvm::Type *llvmXElementType = moduleTranslation.convertType(
      atomicCaptureOp.getAtomicReadOp().getElementType());
  llvm::OpenMPIRBuilder::AtomicOpValue llvmAtomicX = {llvmX, llvmXElementType,
                                                      /*isSigned=*/false,
                                                      /*isVolatile=*/false};
  llvm::OpenMPIRBuilder::AtomicOpValue llvmAtomicV = {llvmV, llvmXElementType,
                                                      /*isSigned=*/false,
                                                      /*isVolatile=*/false};

  llvm::AtomicOrdering atomicOrdering =
      convertAtomicOrdering(atomicCaptureOp.getMemoryOrderVal());

  LogicalResult updateGenStatus = success();
  auto updateFn = [&](llvm::Value *atomicx,
                      llvm::IRBuilder<> &builder) -> llvm::Value * {
    if (atomicWriteOp)
      return moduleTranslation.lookupValue(atomicWriteOp.getValue());
    Block &bb = *atomicUpdateOp.getRegion().begin();
    moduleTranslation.mapValue(*atomicUpdateOp.getRegion().args_begin(),
                               atomicx);
    moduleTranslation.mapBlock(&bb, builder.GetInsertBlock());
    if (failed(moduleTranslation.convertBlock(bb, true, builder))) {
      updateGenStatus = (atomicUpdateOp.emitError()
                         << "unable to convert update operation to llvm IR");
      return nullptr;
    }
    omp::YieldOp yieldop = dyn_cast<omp::YieldOp>(bb.getTerminator());
    assert(yieldop && yieldop.getResults().size() == 1 &&
           "terminator must be omp.yield op and it must have exactly one "
           "argument");
    return moduleTranslation.lookupValue(yieldop.getResults()[0]);
  };

  // Handle ambiguous alloca, if any.
  auto allocaIP = findAllocaInsertPoint(builder, moduleTranslation);
  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  builder.restoreIP(ompBuilder->createAtomicCapture(
      ompLoc, allocaIP, llvmAtomicX, llvmAtomicV, llvmExpr, atomicOrdering,
      binop, updateFn, atomicUpdateOp, isPostfixUpdate, isXBinopExpr));
  return updateGenStatus;
}

/// Converts an OpenMP reduction operation using OpenMPIRBuilder. Expects the
/// mapping between reduction variables and their private equivalents to have
/// been stored on the ModuleTranslation stack. Currently only supports
/// reduction within WsLoopOp, but can be easily extended.
static LogicalResult
convertOmpReductionOp(omp::ReductionOp reductionOp,
                      llvm::IRBuilderBase &builder,
                      LLVM::ModuleTranslation &moduleTranslation) {
  // Find the declaration that corresponds to the reduction op.
  auto reductionContainer = reductionOp->getParentOfType<omp::WsLoopOp>();
  omp::ReductionDeclareOp declaration =
      findReductionDecl(reductionContainer, reductionOp);
  assert(declaration && "could not find reduction declaration");

  // Retrieve the mapping between reduction variables and their private
  // equivalents.
  const DenseMap<Value, llvm::Value *> *reductionVariableMap = nullptr;
  moduleTranslation.stackWalk<OpenMPVarMappingStackFrame>(
      [&](const OpenMPVarMappingStackFrame &frame) {
        reductionVariableMap = &frame.mapping;
        return WalkResult::interrupt();
      });
  assert(reductionVariableMap && "couldn't find private reduction variables");

  // Translate the reduction operation by emitting the body of the corresponding
  // reduction declaration.
  Region &reductionRegion = declaration.getReductionRegion();
  llvm::Value *privateReductionVar =
      reductionVariableMap->lookup(reductionOp.getAccumulator());
  llvm::Value *reductionVal = builder.CreateLoad(
      moduleTranslation.convertType(reductionOp.getOperand().getType()),
      privateReductionVar);

  moduleTranslation.mapValue(reductionRegion.front().getArgument(0),
                             reductionVal);
  moduleTranslation.mapValue(
      reductionRegion.front().getArgument(1),
      moduleTranslation.lookupValue(reductionOp.getOperand()));

  SmallVector<llvm::Value *> phis;
  if (failed(inlineConvertOmpRegions(reductionRegion, "omp.reduction.body",
                                     builder, moduleTranslation, &phis)))
    return failure();
  assert(phis.size() == 1 && "expected one value to be yielded from "
                             "the reduction body declaration region");
  builder.CreateStore(phis[0], privateReductionVar);
  return success();
}

/// Converts an OpenMP Threadprivate operation into LLVM IR using
/// OpenMPIRBuilder.
static LogicalResult
convertOmpThreadprivate(Operation &opInst, llvm::IRBuilderBase &builder,
                        LLVM::ModuleTranslation &moduleTranslation) {
  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  auto threadprivateOp = cast<omp::ThreadprivateOp>(opInst);

  Value symAddr = threadprivateOp.getSymAddr();
  auto *symOp = symAddr.getDefiningOp();
  if (!isa<LLVM::AddressOfOp>(symOp))
    return opInst.emitError("Addressing symbol not found");
  LLVM::AddressOfOp addressOfOp = dyn_cast<LLVM::AddressOfOp>(symOp);

  LLVM::GlobalOp global =
      addressOfOp.getGlobal(moduleTranslation.symbolTable());
  llvm::GlobalValue *globalValue = moduleTranslation.lookupGlobal(global);
  llvm::Value *data =
      builder.CreateBitCast(globalValue, builder.getInt8PtrTy());
  llvm::Type *type = globalValue->getValueType();
  llvm::TypeSize typeSize =
      builder.GetInsertBlock()->getModule()->getDataLayout().getTypeStoreSize(
          type);
  llvm::ConstantInt *size = builder.getInt64(typeSize.getFixedValue());
  llvm::StringRef suffix = llvm::StringRef(".cache", 6);
  std::string cacheName = (Twine(global.getSymName()).concat(suffix)).str();
  // Emit runtime function and bitcast its type (i8*) to real data type.
  llvm::Value *callInst =
      moduleTranslation.getOpenMPBuilder()->createCachedThreadPrivate(
          ompLoc, data, size, cacheName);
  llvm::Value *result = builder.CreateBitCast(callInst, globalValue->getType());
  moduleTranslation.mapValue(opInst.getResult(0), result);
  return success();
}

/// Process MapOperands for Target Data directives.
static LogicalResult processMapOperand(
    llvm::IRBuilderBase &builder, LLVM::ModuleTranslation &moduleTranslation,
    const SmallVector<Value> &mapOperands, const ArrayAttr &mapTypes,
    SmallVector<uint64_t> &mapTypeFlags,
    SmallVectorImpl<llvm::Constant *> &mapNames,
    struct llvm::OpenMPIRBuilder::MapperAllocas &mapperAllocas) {
  auto numMapOperands = mapOperands.size();
  llvm::OpenMPIRBuilder *ompBuilder = moduleTranslation.getOpenMPBuilder();
  llvm::PointerType *i8PtrTy = builder.getInt8PtrTy();
  llvm::ArrayType *arrI8PtrTy = llvm::ArrayType::get(i8PtrTy, numMapOperands);
  llvm::IntegerType *i64Ty = builder.getInt64Ty();
  llvm::ArrayType *arrI64Ty = llvm::ArrayType::get(i64Ty, numMapOperands);

  unsigned index = 0;
  for (const auto &mapOp : mapOperands) {
    const auto &mapTypeOp = mapTypes[index];

    llvm::Value *mapOpValue = moduleTranslation.lookupValue(mapOp);
    llvm::Value *mapOpPtrBase;
    llvm::Value *mapOpPtr;
    llvm::Value *mapOpSize;

    if (isa<LLVM::LLVMPointerType>(mapOp.getType())) {
      mapOpPtrBase = mapOpValue;
      mapOpPtr = mapOpValue;
      mapOpSize = ompBuilder->getSizeInBytes(mapOpValue);
    } else {
      return failure();
    }

    // Store base pointer extracted from operand into the i-th position of
    // argBase.
    llvm::Value *ptrBaseGEP = builder.CreateInBoundsGEP(
        arrI8PtrTy, mapperAllocas.ArgsBase,
        {builder.getInt32(0), builder.getInt32(index)});
    llvm::Value *ptrBaseCast = builder.CreateBitCast(
        ptrBaseGEP, mapOpPtrBase->getType()->getPointerTo());
    builder.CreateStore(mapOpPtrBase, ptrBaseCast);

    // Store pointer extracted from operand into the i-th position of args.
    llvm::Value *ptrGEP = builder.CreateInBoundsGEP(
        arrI8PtrTy, mapperAllocas.Args,
        {builder.getInt32(0), builder.getInt32(index)});
    llvm::Value *ptrCast =
        builder.CreateBitCast(ptrGEP, mapOpPtr->getType()->getPointerTo());
    builder.CreateStore(mapOpPtr, ptrCast);

    // Store size extracted from operand into the i-th position of argSizes.
    llvm::Value *sizeGEP = builder.CreateInBoundsGEP(
        arrI64Ty, mapperAllocas.ArgSizes,
        {builder.getInt32(0), builder.getInt32(index)});
    builder.CreateStore(mapOpSize, sizeGEP);

    mapTypeFlags.push_back(dyn_cast<mlir::IntegerAttr>(mapTypeOp).getInt());
    llvm::Constant *mapName =
        mlir::LLVM::createMappingInformation(mapOp.getLoc(), *ompBuilder);
    mapNames.push_back(mapName);
    ++index;
  }

  return success();
}

static LogicalResult
convertOmpTargetData(Operation *op, llvm::IRBuilderBase &builder,
                     LLVM::ModuleTranslation &moduleTranslation) {
  unsigned numMapOperands;
  llvm::Value *ifCond = nullptr;
  int64_t deviceID = llvm::omp::OMP_DEVICEID_UNDEF;
  SmallVector<Value> mapOperands;
  ArrayAttr mapTypes;

  llvm::OpenMPIRBuilder *ompBuilder = moduleTranslation.getOpenMPBuilder();

  LogicalResult result =
      llvm::TypeSwitch<Operation *, LogicalResult>(op)
          .Case([&](omp::DataOp dataOp) {
            if (!dataOp.getUseDeviceAddr().empty() ||
                !dataOp.getUseDevicePtr().empty())
              return failure();

            if (auto ifExprVar = dataOp.getIfExpr())
              ifCond = moduleTranslation.lookupValue(ifExprVar);

            if (auto devId = dataOp.getDevice())
              if (auto constOp = mlir::dyn_cast<mlir::LLVM::ConstantOp>(
                      devId.getDefiningOp()))
                if (auto intAttr =
                        dyn_cast<mlir::IntegerAttr>(constOp.getValue()))
                  deviceID = intAttr.getInt();

            numMapOperands = dataOp.getMapOperands().size();
            mapOperands = dataOp.getMapOperands();
            mapTypes = dataOp.getMapTypes();
            return success();
          })
          .Case([&](omp::EnterDataOp enterDataOp) {
            if (enterDataOp.getNowait())
              return failure();

            if (auto ifExprVar = enterDataOp.getIfExpr())
              ifCond = moduleTranslation.lookupValue(ifExprVar);

            if (auto devId = enterDataOp.getDevice())
              if (auto constOp = mlir::dyn_cast<mlir::LLVM::ConstantOp>(
                      devId.getDefiningOp()))
                if (auto intAttr =
                        dyn_cast<mlir::IntegerAttr>(constOp.getValue()))
                  deviceID = intAttr.getInt();

            numMapOperands = enterDataOp.getMapOperands().size();
            mapOperands = enterDataOp.getMapOperands();
            mapTypes = enterDataOp.getMapTypes();
            return success();
          })
          .Case([&](omp::ExitDataOp exitDataOp) {
            if (exitDataOp.getNowait())
              return failure();

            if (auto ifExprVar = exitDataOp.getIfExpr())
              ifCond = moduleTranslation.lookupValue(ifExprVar);

            if (auto devId = exitDataOp.getDevice())
              if (auto constOp = mlir::dyn_cast<mlir::LLVM::ConstantOp>(
                      devId.getDefiningOp()))
                if (auto intAttr =
                        dyn_cast<mlir::IntegerAttr>(constOp.getValue()))
                  deviceID = intAttr.getInt();

            numMapOperands = exitDataOp.getMapOperands().size();
            mapOperands = exitDataOp.getMapOperands();
            mapTypes = exitDataOp.getMapTypes();
            return success();
          })
          .Default([&](Operation *op) {
            return op->emitError("unsupported OpenMP operation: ")
                   << op->getName();
          });

  if (failed(result))
    return failure();

  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  llvm::OpenMPIRBuilder::InsertPointTy allocaIP =
      findAllocaInsertPoint(builder, moduleTranslation);

  struct llvm::OpenMPIRBuilder::MapperAllocas mapperAllocas;
  SmallVector<uint64_t> mapTypeFlags;
  SmallVector<llvm::Constant *> mapNames;
  ompBuilder->createMapperAllocas(builder.saveIP(), allocaIP, numMapOperands,
                                  mapperAllocas);

  using InsertPointTy = llvm::OpenMPIRBuilder::InsertPointTy;
  LogicalResult processMapOpStatus = success();
  auto processMapOpCB = [&](InsertPointTy allocaIP, InsertPointTy codeGenIP) {
    builder.restoreIP(codeGenIP);
    processMapOpStatus =
        processMapOperand(builder, moduleTranslation, mapOperands, mapTypes,
                          mapTypeFlags, mapNames, mapperAllocas);
  };

  LogicalResult bodyGenStatus = success();
  auto bodyCB = [&](InsertPointTy allocaIP, InsertPointTy codeGenIP) {
    // DataOp has only one region associated with it.
    auto &region = cast<omp::DataOp>(op).getRegion();
    builder.restoreIP(codeGenIP);
    bodyGenStatus = inlineConvertOmpRegions(region, "omp.data.region", builder,
                                            moduleTranslation);
  };

  if (isa<omp::DataOp>(op)) {
    builder.restoreIP(ompBuilder->createTargetData(
        ompLoc, builder.saveIP(), mapTypeFlags, mapNames, mapperAllocas,
        /*IsBegin=*/false, deviceID, ifCond, processMapOpCB, bodyCB));
  } else {
    builder.restoreIP(ompBuilder->createTargetData(
        ompLoc, builder.saveIP(), mapTypeFlags, mapNames, mapperAllocas,
        isa<omp::EnterDataOp>(op), deviceID, ifCond, processMapOpCB));
  }

  if (failed(processMapOpStatus))
    return processMapOpStatus;
  return bodyGenStatus;
}

/// Lowers the FlagsAttr which is applied to the module on the device
/// pass when offloading, this attribute contains OpenMP RTL globals that can
/// be passed as flags to the frontend, otherwise they are set to default
LogicalResult convertFlagsAttr(Operation *op, mlir::omp::FlagsAttr attribute,
                               LLVM::ModuleTranslation &moduleTranslation) {
  if (!cast<mlir::ModuleOp>(op))
    return failure();
  
  llvm::OpenMPIRBuilder *ompBuilder = moduleTranslation.getOpenMPBuilder();

  ompBuilder->createGlobalFlag(
      attribute.getDebugKind() /*LangOpts().OpenMPTargetDebug*/,
      "__omp_rtl_debug_kind");
  ompBuilder->createGlobalFlag(
      attribute
          .getAssumeTeamsOversubscription() /*LangOpts().OpenMPTeamSubscription*/
      ,
      "__omp_rtl_assume_teams_oversubscription");
  ompBuilder->createGlobalFlag(
      attribute
          .getAssumeThreadsOversubscription() /*LangOpts().OpenMPThreadSubscription*/
      ,
      "__omp_rtl_assume_threads_oversubscription");
  ompBuilder->createGlobalFlag(
      attribute.getAssumeNoThreadState() /*LangOpts().OpenMPNoThreadState*/,
      "__omp_rtl_assume_no_thread_state");
  ompBuilder->createGlobalFlag(
      attribute
          .getAssumeNoNestedParallelism() /*LangOpts().OpenMPNoNestedParallelism*/
      ,
      "__omp_rtl_assume_no_nested_parallelism");
  ompBuilder->M.addModuleFlag(llvm::Module::Max, "openmp-device",
                              attribute.getOpenmpDeviceVersion());
  return success();
}

static bool getTargetEntryUniqueInfo(llvm::TargetRegionEntryInfo &targetInfo,
                                     omp::TargetOp targetOp,
                                     llvm::StringRef parentName = "") {
  auto fileLoc = targetOp.getLoc()->findInstanceOf<FileLineColLoc>();

  assert(fileLoc && "No file found from location");
  StringRef fileName = fileLoc.getFilename().getValue();

  llvm::sys::fs::UniqueID id;
  if (auto ec = llvm::sys::fs::getUniqueID(fileName, id)) {
    targetOp.emitError("Unable to get unique ID for file");
    return false;
  }

  uint64_t line = fileLoc.getLine();
  targetInfo = llvm::TargetRegionEntryInfo(parentName, id.getDevice(),
                                           id.getFile(), line);
  return true;
}

static bool targetOpSupported(Operation &opInst) {
  auto targetOp = cast<omp::TargetOp>(opInst);
  if (targetOp.getIfExpr()) {
    opInst.emitError("If clause not yet supported");
    return false;
  }

  if (targetOp.getDevice()) {
    opInst.emitError("Device clause not yet supported");
    return false;
  }

  if (targetOp.getThreadLimit()) {
    opInst.emitError("Thread limit clause not yet supported");
    return false;
  }

  if (targetOp.getNowait()) {
    opInst.emitError("Nowait clause not yet supported");
    return false;
  }

  return true;
}

static LogicalResult
convertOmpTarget(Operation &opInst, llvm::IRBuilderBase &builder,
                 LLVM::ModuleTranslation &moduleTranslation) {

  if (!targetOpSupported(opInst))
    return failure();

  auto targetOp = cast<omp::TargetOp>(opInst);
  auto &targetRegion = targetOp.getRegion();

  llvm::SetVector<Value> operandSet;
  getUsedValuesDefinedAbove(targetRegion, operandSet);

  // Collect the input arguments.
  llvm::SmallVector<llvm::Value *> inputs;
  for (Value operand : operandSet)
    inputs.push_back(moduleTranslation.lookupValue(operand));

  LogicalResult bodyGenStatus = success();

  using InsertPointTy = llvm::OpenMPIRBuilder::InsertPointTy;
  auto bodyCB = [&](InsertPointTy allocaIP,
                    InsertPointTy codeGenIP) -> InsertPointTy {
    builder.restoreIP(codeGenIP);
    llvm::BasicBlock *exitBlock = convertOmpOpRegions(
        targetRegion, "omp.target", builder, moduleTranslation, bodyGenStatus);
    builder.SetInsertPoint(exitBlock);
    return builder.saveIP();
  };

  llvm::OpenMPIRBuilder::LocationDescription ompLoc(builder);
  StringRef parentName = opInst.getParentOfType<LLVM::LLVMFuncOp>().getName();
  llvm::TargetRegionEntryInfo entryInfo;

  if (!getTargetEntryUniqueInfo(entryInfo, targetOp, parentName))
    return failure();

  int32_t defaultValTeams = -1;
  int32_t defaultValThreads = -1;

  builder.restoreIP(moduleTranslation.getOpenMPBuilder()->createTarget(
      ompLoc, builder.saveIP(), entryInfo, defaultValTeams, defaultValThreads,
      inputs, bodyCB));

  return bodyGenStatus;
}

namespace {

/// Implementation of the dialect interface that converts operations belonging
/// to the OpenMP dialect to LLVM IR.
class OpenMPDialectLLVMIRTranslationInterface
    : public LLVMTranslationDialectInterface {
public:
  using LLVMTranslationDialectInterface::LLVMTranslationDialectInterface;

  /// Translates the given operation to LLVM IR using the provided IR builder
  /// and saving the state in `moduleTranslation`.
  LogicalResult
  convertOperation(Operation *op, llvm::IRBuilderBase &builder,
                   LLVM::ModuleTranslation &moduleTranslation) const final;

  LogicalResult
  amendOperation(Operation *op, NamedAttribute attribute,
                 LLVM::ModuleTranslation &moduleTranslation) const final;
};

} // namespace

/// Given an OpenMP MLIR attribute, create the corresponding LLVM-IR, runtime
/// calls, or operation amendments
LogicalResult OpenMPDialectLLVMIRTranslationInterface::amendOperation(
    Operation *op, NamedAttribute attribute,
    LLVM::ModuleTranslation &moduleTranslation) const {

  return llvm::TypeSwitch<Attribute, LogicalResult>(attribute.getValue())
      .Case([&](mlir::omp::FlagsAttr rtlAttr) {
        return convertFlagsAttr(op, rtlAttr, moduleTranslation);
      })
      .Case([&](mlir::omp::VersionAttr versionAttr) {
        llvm::OpenMPIRBuilder *ompBuilder =
            moduleTranslation.getOpenMPBuilder();
        ompBuilder->M.addModuleFlag(llvm::Module::Max, "openmp",
                                    versionAttr.getVersion());
        return success();
      })
      .Default([&](Attribute attr) {
        // fall through for omp attributes that do not require lowering and/or
        // have no concrete definition and thus no type to define a case on
        return success();
      });

  return failure();
}

/// Given an OpenMP MLIR operation, create the corresponding LLVM IR
/// (including OpenMP runtime calls).
LogicalResult OpenMPDialectLLVMIRTranslationInterface::convertOperation(
    Operation *op, llvm::IRBuilderBase &builder,
    LLVM::ModuleTranslation &moduleTranslation) const {

  llvm::OpenMPIRBuilder *ompBuilder = moduleTranslation.getOpenMPBuilder();

  return llvm::TypeSwitch<Operation *, LogicalResult>(op)
      .Case([&](omp::BarrierOp) {
        ompBuilder->createBarrier(builder.saveIP(), llvm::omp::OMPD_barrier);
        return success();
      })
      .Case([&](omp::TaskwaitOp) {
        ompBuilder->createTaskwait(builder.saveIP());
        return success();
      })
      .Case([&](omp::TaskyieldOp) {
        ompBuilder->createTaskyield(builder.saveIP());
        return success();
      })
      .Case([&](omp::FlushOp) {
        // No support in Openmp runtime function (__kmpc_flush) to accept
        // the argument list.
        // OpenMP standard states the following:
        //  "An implementation may implement a flush with a list by ignoring
        //   the list, and treating it the same as a flush without a list."
        //
        // The argument list is discarded so that, flush with a list is treated
        // same as a flush without a list.
        ompBuilder->createFlush(builder.saveIP());
        return success();
      })
      .Case([&](omp::ParallelOp op) {
        return convertOmpParallel(op, builder, moduleTranslation);
      })
      .Case([&](omp::ReductionOp reductionOp) {
        return convertOmpReductionOp(reductionOp, builder, moduleTranslation);
      })
      .Case([&](omp::MasterOp) {
        return convertOmpMaster(*op, builder, moduleTranslation);
      })
      .Case([&](omp::CriticalOp) {
        return convertOmpCritical(*op, builder, moduleTranslation);
      })
      .Case([&](omp::OrderedRegionOp) {
        return convertOmpOrderedRegion(*op, builder, moduleTranslation);
      })
      .Case([&](omp::OrderedOp) {
        return convertOmpOrdered(*op, builder, moduleTranslation);
      })
      .Case([&](omp::WsLoopOp) {
        return convertOmpWsLoop(*op, builder, moduleTranslation);
      })
      .Case([&](omp::SimdLoopOp) {
        return convertOmpSimdLoop(*op, builder, moduleTranslation);
      })
      .Case([&](omp::AtomicReadOp) {
        return convertOmpAtomicRead(*op, builder, moduleTranslation);
      })
      .Case([&](omp::AtomicWriteOp) {
        return convertOmpAtomicWrite(*op, builder, moduleTranslation);
      })
      .Case([&](omp::AtomicUpdateOp op) {
        return convertOmpAtomicUpdate(op, builder, moduleTranslation);
      })
      .Case([&](omp::AtomicCaptureOp op) {
        return convertOmpAtomicCapture(op, builder, moduleTranslation);
      })
      .Case([&](omp::SectionsOp) {
        return convertOmpSections(*op, builder, moduleTranslation);
      })
      .Case([&](omp::SingleOp op) {
        return convertOmpSingle(op, builder, moduleTranslation);
      })
      .Case([&](omp::TaskOp op) {
        return convertOmpTaskOp(op, builder, moduleTranslation);
      })
      .Case([&](omp::TaskGroupOp op) {
        return convertOmpTaskgroupOp(op, builder, moduleTranslation);
      })
      .Case<omp::YieldOp, omp::TerminatorOp, omp::ReductionDeclareOp,
            omp::CriticalDeclareOp>([](auto op) {
        // `yield` and `terminator` can be just omitted. The block structure
        // was created in the region that handles their parent operation.
        // `reduction.declare` will be used by reductions and is not
        // converted directly, skip it.
        // `critical.declare` is only used to declare names of critical
        // sections which will be used by `critical` ops and hence can be
        // ignored for lowering. The OpenMP IRBuilder will create unique
        // name for critical section names.
        return success();
      })
      .Case([&](omp::ThreadprivateOp) {
        return convertOmpThreadprivate(*op, builder, moduleTranslation);
      })
      .Case<omp::DataOp, omp::EnterDataOp, omp::ExitDataOp>([&](auto op) {
        return convertOmpTargetData(op, builder, moduleTranslation);
      })
      .Case([&](omp::TargetOp) {
        return convertOmpTarget(*op, builder, moduleTranslation);
      })
      .Default([&](Operation *inst) {
        return inst->emitError("unsupported OpenMP operation: ")
               << inst->getName();
      });
}

void mlir::registerOpenMPDialectTranslation(DialectRegistry &registry) {
  registry.insert<omp::OpenMPDialect>();
  registry.addExtension(+[](MLIRContext *ctx, omp::OpenMPDialect *dialect) {
    dialect->addInterfaces<OpenMPDialectLLVMIRTranslationInterface>();
  });
}

void mlir::registerOpenMPDialectTranslation(MLIRContext &context) {
  DialectRegistry registry;
  registerOpenMPDialectTranslation(registry);
  context.appendDialectRegistry(registry);
}
