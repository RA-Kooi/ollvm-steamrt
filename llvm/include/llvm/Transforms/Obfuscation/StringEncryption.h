#ifndef LLVM_STRING_ENCRYPTION_H
#define LLVM_STRING_ENCRYPTION_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"

#include "ObfuscationOptions.h"

namespace llvm {
class StringEncryptionPass : public PassInfoMixin<StringEncryptionPass> {
public:
  StringEncryptionPass() : Options(new ObfuscationOptions()) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  ObfuscationOptions *Options;
};
} // namespace llvm

#endif
