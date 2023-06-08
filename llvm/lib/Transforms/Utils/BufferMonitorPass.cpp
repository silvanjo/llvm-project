#include "llvm/Transforms/Utils/BufferMonitorPass.h"

#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <string>
#include <vector>
#include <iostream>

using namespace llvm;

PreservedAnalyses BufferMonitorPass::run(Module& module, ModuleAnalysisManager& MAM) 
{
    

    return PreservedAnalyses::none();
}