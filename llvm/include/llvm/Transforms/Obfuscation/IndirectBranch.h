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
  bool flag;
  ObfuscationOptions *Options;
  std::map<BasicBlock *, unsigned> BBNumbering;
  std::vector<BasicBlock *> BBTargets; // all conditional branch targets
  CryptoUtils RandomEngine;

  IndirectBranchPass(bool flag) {
    this->flag = flag;
    this->Options = new ObfuscationOptions;
  }

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  void NumberBasicBlock(Function &F);
  GlobalVariable *getIndirectTargets(Function &F, ConstantInt *EncKey);
  static bool isRequired() { return true; }
};

IndirectBranchPass *createIndirectBranch(bool flag);
} // namespace llvm

#endif // LLVM_INDIRECTBRANCH_H
