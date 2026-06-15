#ifndef LLVM_INDIRECTBRANCH_H
#define LLVM_INDIRECTBRANCH_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include "CryptoUtils.h"
#include "ObfuscationOptions.h"

#include <map>
#include <vector>

namespace llvm {
class IndirectBranchPass : public PassInfoMixin<IndirectBranchPass> {
public:
  IndirectBranchPass()
      : Options(new ObfuscationOptions()), BBNumbering(), BBTargets(),
        RandomEngine() {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  void numberBasicBlock(Function &F);
  GlobalVariable *getIndirectTargets(Function &F, ConstantInt *EncKey);

private:
  ObfuscationOptions *Options;
  std::map<BasicBlock *, unsigned> BBNumbering;
  std::vector<BasicBlock *> BBTargets; // all conditional branch targets
  CryptoUtils RandomEngine;
};
} // namespace llvm

#endif // LLVM_INDIRECTBRANCH_H
