#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <string>
#include <vector>
#include <iostream>

using namespace llvm;

void CheckFunctionArguments(Function& F, int lowerBound, int upperBound) {
    LLVMContext &context = F.getContext();
    auto module = F.getParent();

    BasicBlock* FirstBB = &F.front();
    Instruction* firstInstruction = &F.front().front();
    IRBuilder<> builder(firstInstruction);

    // builder.getInt32(lowerBound);
    // Value* lowerBoundConstant = ConstantInt::get(context, APInt(32, lowerBound));
    Value* upperBoundConstant = ConstantInt::get(context, APInt(32, upperBound));

    std::vector<Type*> printfArgsTypes;
    printfArgsTypes.push_back(Type::getInt8PtrTy(context));
    FunctionType* printfFunctionType = FunctionType::get(builder.getInt32Ty(), printfArgsTypes, true);
    Function* printfFunction = Function::Create(printfFunctionType, Function::ExternalLinkage, "printf", module);


    BasicBlock* CheckBoundsBB = nullptr;
    for (Function::arg_iterator iter = F.arg_begin(), iter_end = F.arg_end(); iter != iter_end; iter++)
    {

        std::cout << "Argument: " << iter->getName().str() << ", Type: " << iter->getType() << std::endl;
        
        if (iter->getType()->getTypeID() != Type::IntegerTyID)
            continue;

        if (!CheckBoundsBB)
            CheckBoundsBB = BasicBlock::Create(context, "check_argument_bound", &F, FirstBB);
        
        BasicBlock* ThenBB = BasicBlock::Create(context, "then", &F, FirstBB);

        builder.SetInsertPoint(ThenBB);

        std::string printLog = "Out of Bounds: %d\n";
        Value* printLogValue = builder.CreateGlobalStringPtr(printLog);
        
        builder.CreateCall(printfFunction, { printLogValue, iter });
        Value* returnValue = ConstantInt::get(context, APInt(32, -1));
        builder.CreateRet(returnValue);

        builder.SetInsertPoint(CheckBoundsBB);

        Value* upperBoundCondition = builder.CreateICmpSGE(iter, upperBoundConstant);

        BasicBlock* NextArgumentTestBlockBB = BasicBlock::Create(context, "check_argument_bound", &F, FirstBB);
        builder.CreateCondBr(upperBoundCondition, ThenBB, NextArgumentTestBlockBB);
        
        CheckBoundsBB = NextArgumentTestBlockBB;

        if ((iter + 1) == iter_end)
        {
            builder.SetInsertPoint(CheckBoundsBB);
            builder.CreateBr(FirstBB);
        }

    }
}

void CheckBufferBoundary(Function& F)
{
    LLVMContext &context = F.getContext();

    for (inst_iterator instruction = inst_begin(F), last_instruction = inst_end(F); instruction != last_instruction; ++instruction)
    {
        // std::cout << "Instruction: " << I->getName().str() << std::endl;
        errs() << *instruction << "\n";

        for (Use& U : instruction->operands()) {
          Type* T = U->getType();
          if (T->isArrayTy()) {
            ArrayType* AT = dyn_cast<ArrayType>(T);
            AT->print(errs());
            errs() << "\n";
          }
        }
    }
}