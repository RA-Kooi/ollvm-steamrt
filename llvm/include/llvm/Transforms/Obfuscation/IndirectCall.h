#ifndef LLVM_INDIRECTCALL_H
#define LLVM_INDIRECTCALL_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class IndirectCallPass : public PassInfoMixin<IndirectCallPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};
} // namespace llvm

#endif // LLVM_INDIRECTCALL_H
