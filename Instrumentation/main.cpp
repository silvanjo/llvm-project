#include "Instrumentation.h"

#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::cerr << "Usage: " << argv[0] << " <input-file> <output-file> <function-name> <upper-bound>" << std::endl;
        return 1;
    }

    std::string filename = argv[1];
    std::string outputFilename = argv[2];
    std::string functionName = argv[3];
    int upperBound = std::stoi(argv[4]);

    LLVMContext context;

    SMDiagnostic error;

    std::unique_ptr<llvm::Module> module = parseIRFile(filename, error, context);

    if (!module)
    {
        std::cerr << "Error: " << error.getMessage().str() << std::endl;
        return 2;
    }

    for (Function& F : *module)
    {
        std::cout << "Function: " << F.getName().str() << std::endl;

        if (F.getName().str() == "add")
        {
            CheckBufferBoundary(F);
            CheckFunctionArguments(F, 0, upperBound);
        }
    }

    std::error_code errorCode;

    raw_fd_ostream outputStream(outputFilename, errorCode, sys::fs::OF_None);

    if (errorCode)
    {
        std::cerr << "Error: " << errorCode.message() << std::endl;
        return 3;
    }

    WriteBitcodeToFile(*module, outputStream);

    return 0;
}