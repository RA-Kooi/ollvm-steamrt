#ifndef LLVM_STRING_ENCRYPTION_H
#define LLVM_STRING_ENCRYPTION_H

#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "ObfuscationOptions.h"

namespace llvm {
class StringEncryptionPass : public PassInfoMixin<StringEncryptionPass> {
public:
  explicit StringEncryptionPass(bool Enable)
      : Enabled(Enable), Options(new ObfuscationOptions()) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  bool Enabled;

  ObfuscationOptions *Options;
};

StringEncryptionPass *createStringEncryption(bool Enabled);
} // namespace llvm

#endif
