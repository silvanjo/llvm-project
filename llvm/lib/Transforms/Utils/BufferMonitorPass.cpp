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

#define WRITE_TO_FILE

using namespace llvm;

PreservedAnalyses BufferMonitorPass::run(Function &F, FunctionAnalysisManager &AM) 
{
    std::cout << "Pass on " << F.getName().str() << std::endl;

    // map of buffer names to buffer sizes
    std::map<std::string, int> bufferMap;

    // Get context, module and create IRBuilder for instrumentations
    LLVMContext &context = F.getContext();
    auto module = F.getParent();
    IRBuilder<> builder(context);

    // Get main function
    Function* mainFunction = module->getFunction("main");
    if (!mainFunction) 
    {
        std::cout << "No main function found" << std::endl;
        return PreservedAnalyses::none();
    }

    // Open the file for writing in the beginning of the main function
    builder.SetInsertPoint(&mainFunction->getEntryBlock().front());
    
    // Get printf function
    std::vector<Type*> printfArgsTypes;
    printfArgsTypes.push_back(Type::getInt8PtrTy(context));
    FunctionType* printfFunctionType = FunctionType::get(builder.getInt32Ty(), printfArgsTypes, true);
    
    Function* printfFunction = module->getFunction("printf");
    if (!printfFunction) 
    {
        printfFunction = Function::Create(printfFunctionType, Function::ExternalLinkage, "printf", module);
    }

    // Get or insert fopen function for file IO
    FunctionType* FT = FunctionType::get(Type::getInt8PtrTy(context), { Type::getInt8PtrTy(context), Type::getInt8PtrTy(context) }, false);
    FunctionCallee fopenFuncCallee = module->getOrInsertFunction("fopen", FT);
    Function* fopenFunc = cast<Function>(fopenFuncCallee.getCallee());  

    // Get or insert the fprint function for file IO
    FunctionType* fprintfType = FunctionType::get(Type::getInt32Ty(context), { Type::getInt8PtrTy(context), Type::getInt8PtrTy(context) }, true);
    FunctionCallee fprintfFuncCallee = module->getOrInsertFunction("fprintf", fprintfType);
    Function* fprintfFunc = cast<Function>(fprintfFuncCallee.getCallee());

    // Open the file for writing in the beginning of the main function
    Value* filename = builder.CreateGlobalStringPtr("output.txt");
    Value* mode = builder.CreateGlobalStringPtr("w");
    Value* file_ptr = builder.CreateCall(fopenFunc, {filename, mode});

    // Write a test string to the file
    // Value* testString = builder.CreateGlobalStringPtr("Test string\n");
    // builder.CreateCall(fprintfFunc, {file_ptr, testString});

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
            
        }
        
        if (auto callInst = dyn_cast<CallInst>(&*I)) 
        {
            // This is an call instruction

            std::cout << "Call instruction detected" << std::endl;

            Function *calledFunc = callInst->getCalledFunction();

            if (calledFunc) 
            { 
                StringRef funcName = calledFunc->getName();
                if (funcName == "malloc" || funcName.startswith("_Znwm") || funcName.startswith("_Znam"))
                {
                    // This is a heap allocation
                    
                    std::cout << "Found a heap allocation" << std::endl;

                    // get buffer name
                    std::string dynBufferName = callInst->getName().str();   

                    // add buffer to buffer map
                    bufferMap[dynBufferName] = -1;               

                    // get size of allocation
                    Value* mallocSize = callInst->getArgOperand(0);    

                    // Create output string for the file
                    std::string outputString = "Heap allocation of size: %d\n";

                    // Write size of dynamicallly allocated array to file
                    Value* formatString = builder.CreateGlobalStringPtr(outputString, "fileFormatString", 0, module);
                    
                    // Call fprintf to write the size of the dynamically allocated array to the file
                    builder.CreateCall(fprintfFunc, { file_ptr, formatString, mallocSize });
                }
            } else 
            {
                // This is an indirect function call. Handling this requires a more complex analysis.
           
                std::cout << "Found an indirect function call" << std::endl;           
            }
        }
        
        if (GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(&*I)) 
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
                    Value* indexValue = gepInst->getOperand(2);

#ifdef WRITE_TO_FILE

                    if (indexValue->getType()->isIntegerTy())
                    {
                        // Create output string for the file
                        std::string outputString = "Buffer access: %d (static)\n";
                        // Write the accessed index to the file
                        Value* formatString = builder.CreateGlobalStringPtr("Buffer access: %d\n");
                        // Value* formatString = builder.CreateGlobalStringPtr("Index access\n");

                        builder.SetInsertPoint(&*I);
                        builder.CreateCall(fprintfFunc, { file_ptr, formatString, indexValue });
                    } else 
                    {
                        std::cout << "Cannot read getelementptr instruction. Index wrong format." << std::endl;
                    }
#else
                    
                    if (ConstantInt* constInt = dyn_cast<ConstantInt>(indexValue))
                    {
                        // This is a constant index

                        unsigned index = constInt->getZExtValue();     // get index as an 64-bit usigned integer

                        // Create output string for the file
                        std::string outputString = "Constant index access: %d\n";
                        // Write the accessed index to the file
                        Value* formatString = builder.CreateGlobalStringPtr(outputString, "fileFormatString", 0, module);
                        // Value* formatString = builder.CreateGlobalStringPtr("Index access\n");
                        builder.CreateCall(fprintfFunc, {file_ptr, formatString });

                        std::cout << "Constant index beeing accessed: " << index << std::endl;

                        if (index >= bufferMap[bufferName])
                        {
                            // Access is out-of-bounds

                            std::cout << "Out-of-bounds access detected" << std::endl;
                        }
                    }
                    else
                    {
                        // This is a non-constant index

                        std::cout << "Non-constant index beeing accessed" << std::endl;
                        
                        // Write the accessed index to the file

                        if (indexValue->getType()->isIntegerTy())
                        {
                            std::string printLog = "Index being accessed: %d\n";
                            Value* printLogValue = builder.CreateGlobalStringPtr(printLog, "printLog", 0, module);

                            builder.SetInsertPoint(&*I);

                            builder.CreateCall(printfFunction, { printLogValue, indexValue });
                        }
                    }
#endif

                }
            }

        }

    }

    // Close file using fclose function
    /*std::vector<Type*> fcloseArgs;
    fcloseArgs.push_back(PointerType::getUnqual(Type::getInt8Ty(context))); // FILE* argument
    FunctionType* fcloseType = FunctionType::get(Type::getInt32Ty(context), fcloseArgs, false); 
    FunctionCallee fcloseFunc = module->getOrInsertFunction("fclose", fcloseType);
    builder.CreateCall(fcloseFunc, {file_ptr});*/

    // Print each detected buffer and its size

    for (const auto& buffer : bufferMap)
    {
        std::cout << "Buffer: " << buffer.first << ", Size: " << buffer.second << std::endl;
    }

    return PreservedAnalyses::none();
}
