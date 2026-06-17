#ifndef OBFUSCATION_ALIAS_ACCESS
#define OBFUSCATION_ALIAS_ACCESS

#include "llvm/IR/PassManager.h"

namespace llvm {
struct AliasAccess : PassInfoMixin<AliasAccess> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm
#endif
