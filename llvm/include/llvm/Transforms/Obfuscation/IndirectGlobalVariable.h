#ifndef LLVM_INDIRECTGLOBALVARIABLE_H
#define LLVM_INDIRECTGLOBALVARIABLE_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include "CryptoUtils.h"
#include "ObfuscationOptions.h"

#include <map>
#include <vector>

namespace llvm {
class IndirectGlobalVariablePass
    : public PassInfoMixin<IndirectGlobalVariablePass> {
public:
  IndirectGlobalVariablePass()
      : Options(new ObfuscationOptions()), GVNumbering(), GlobalVariables(),
        RandomEngine() {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  void numberGlobalVariable(Function &F);
  GlobalVariable *getIndirectGlobalVariables(Function &F, ConstantInt *EncKey);

private:
  ObfuscationOptions *Options;
  std::map<GlobalVariable *, unsigned> GVNumbering;
  std::vector<GlobalVariable *> GlobalVariables;
  CryptoUtils RandomEngine;
};
} // namespace llvm

#endif // LLVM_INDIRECTGLOBALVARIABLE_H
