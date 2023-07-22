#include "llvm/Transforms/Utils/BufferMonitorPass.h"

#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <map>
#include <string>
#include <iostream>

using namespace llvm;

PreservedAnalyses BufferMonitorPass::run(Function &F, FunctionAnalysisManager &AM) 
{
    std::cout << "Pass on " << F.getName().str() << std::endl;

    std::map<std::string, unsigned> bufferMap;

    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) 
    {

        if (AllocaInst* allocaInst = dyn_cast<AllocaInst>(&*I))
        {
            // This is an alloca instruction, a buffer is being allocated here


            std::string bufferName = allocaInst->getName().str();   // get buffer name

            Value* bufferSizeValue = allocaInst->getArraySize();    // get buffer size

            if (ConstantInt* constInt = dyn_cast<ConstantInt>(bufferSizeValue))
            {

                unsigned bufferSize = constInt->getZExtValue();     // get buffer size as an 64-bit usigned integer

                bufferMap[bufferName] = bufferSize;

                std::cout << "Found Buffer [Name= " << bufferName << "] of size: " << bufferSize << std::endl;
                std::cout << "Stored: " << bufferMap[bufferName] << std::endl;

            } else {
                // TODO: Handle non-constant buffer sizes

                std::cout << "Buffer size is not a constant" << std::endl;
            }
            

        } else if (GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(&*I)) 
        {
            // This is a getelementptr instruction, a buffer is being accessed here

            std::cout << "GetElementPtr detected" << std::endl;
        }

    }

    return PreservedAnalyses::none();
}
