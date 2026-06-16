#ifndef LLVM_STRING_ENCRYPTION_H
#define LLVM_STRING_ENCRYPTION_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class StringEncryptionPass : public PassInfoMixin<StringEncryptionPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};
} // namespace llvm

#endif
