#ifndef LLVM_BUFFER_MONITOR_PASS_H
#define LLVM_BUFFER_MONTIOR_PASS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

    class BufferMonitorPass : public PassInfoMixin<BufferMonitorPass> 
    {
    public:
        PreservedAnalyses run(Module& module, ModuleAnalysisManager& MAM);
    };

}; 

#endif // LLVM_CHECK_BUFFER_BOUNDS_PASS_H