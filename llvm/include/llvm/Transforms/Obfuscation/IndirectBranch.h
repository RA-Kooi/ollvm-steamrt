#ifndef LLVM_INDIRECTBRANCH_H
#define LLVM_INDIRECTBRANCH_H

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
class IndirectBranchPass : public PassInfoMixin<IndirectBranchPass> {
public:
  explicit IndirectBranchPass(bool Enable)
      : Enabled(Enable), Options(new ObfuscationOptions()), BBNumbering(),
        BBTargets(), RandomEngine() {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  void numberBasicBlock(Function &F);
  GlobalVariable *getIndirectTargets(Function &F, ConstantInt *EncKey);

private:
  bool Enabled;
  ObfuscationOptions *Options;
  std::map<BasicBlock *, unsigned> BBNumbering;
  std::vector<BasicBlock *> BBTargets; // all conditional branch targets
  CryptoUtils RandomEngine;
};

IndirectBranchPass *createIndirectBranch(bool Enabled);
} // namespace llvm

#endif // LLVM_INDIRECTBRANCH_H
