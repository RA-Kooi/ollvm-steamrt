#ifndef LLVM_INDIRECTGLOBALVARIABLE_H
#define LLVM_INDIRECTGLOBALVARIABLE_H

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "CryptoUtils.h"
#include "ObfuscationOptions.h"

#include <map>
#include <vector>

namespace llvm {
class IndirectGlobalVariablePass
    : public PassInfoMixin<IndirectGlobalVariablePass> {
public:
  explicit IndirectGlobalVariablePass(bool Enable)
      : Enabled(Enable), Options(new ObfuscationOptions()), GVNumbering(),
        GlobalVariables(), RandomEngine() {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  void numberGlobalVariable(Function &F);
  GlobalVariable *getIndirectGlobalVariables(Function &F, ConstantInt *EncKey);

private:
  bool Enabled;
  ObfuscationOptions *Options;
  std::map<GlobalVariable *, unsigned> GVNumbering;
  std::vector<GlobalVariable *> GlobalVariables;
  CryptoUtils RandomEngine;
};

IndirectGlobalVariablePass *createIndirectGlobalVariable(bool Enabled);
} // namespace llvm

#endif // LLVM_INDIRECTGLOBALVARIABLE_H
