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

    // map of buffer names to buffer sizes

    std::map<std::string, unsigned> bufferMap;

    // Get context, module and create IRBuilder for instrumentations

    LLVMContext &context = F.getContext();
    auto module = F.getParent();
    IRBuilder<> builder(context);
    
    // Get printf function

    std::vector<Type*> printfArgsTypes;
    printfArgsTypes.push_back(Type::getInt8PtrTy(context));
    FunctionType* printfFunctionType = FunctionType::get(builder.getInt32Ty(), printfArgsTypes, true);
    Function* printfFunction = Function::Create(printfFunctionType, Function::ExternalLinkage, "printf", module);

    // Iterate over all instructions in the function

    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) 
    {

        if (AllocaInst* allocaInst = dyn_cast<AllocaInst>(&*I))
        {

            // This is an alloca instruction, a buffer is being allocated here

            std::string bufferName = allocaInst->getName().str();   // get buffer name

            if (ArrayType* arrayType = dyn_cast<ArrayType>(allocaInst->getAllocatedType()))
            {
                // This is an array allocation
                unsigned bufferSizeValue = arrayType->getNumElements(); // get buffer size as number of elements in the array

                bufferMap[bufferName] = bufferSizeValue;               // add buffer to buffer map

                std::cout << "Found Buffer [Name= " << bufferName << "] of size: " << bufferSizeValue << std::endl;
            }
            
        } else if (GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(&*I)) 
        {
            // This is a getelementptr instruction, a buffer is being accessed here

            std::cout << "GetElementPtr detected" << std::endl;

            Value* basePtr = gepInst->getPointerOperand();         // get base pointer

            if (AllocaInst* allocaInst = dyn_cast<AllocaInst>(basePtr))
            {
                // This is a getelementptr instruction on an alloca instruction

                std::string bufferName = allocaInst->getName().str();   // get buffer name

                if (bufferMap.find(bufferName) != bufferMap.end())
                {
                    // This buffer has been allocated

                    std::cout << "Found Buffer [Name= " << bufferName << "] of size: " << bufferMap[bufferName] << std::endl;

                    // Determine the index of the buffer being accessed

                    Value* indexValue = gepInst->getOperand(1);        // get index value

                    if (ConstantInt* constInt = dyn_cast<ConstantInt>(indexValue))
                    {
                        // This is a constant index
                            
                        unsigned index = constInt->getZExtValue();     // get index as an 64-bit usigned integer

                        std::cout << "Index beeing accessed: " << index << std::endl;

                        if (index >= bufferMap[bufferName])
                        {
                            // Access is out-of-bounds

                            std::cout << "Out-of-bounds access detected" << std::endl;
                        }
                    }
                    else 
                    {
                        // This is a non-constant index
                        
                        if (indexValue->getType()->isIntegerTy())
                        {
                            std::string printLog = "Index being accessed: %d\n";
                            Value* printLogValue = builder.CreateGlobalStringPtr(printLog);

                            builder.SetInsertPoint(&*I);

                            builder.CreateCall(printfFunction, { printLogValue, indexValue });
                        }
                    }

                }
            }

        }

    }

    // Print each detected buffer and its size

    for (const auto& buffer : bufferMap)
    {
        std::cout << "Buffer: " << buffer.first << ", Size: " << buffer.second << std::endl;
    }

    return PreservedAnalyses::none();
}
