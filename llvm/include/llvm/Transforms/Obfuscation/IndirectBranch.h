#ifndef LLVM_INDIRECTBRANCH_H
#define LLVM_INDIRECTBRANCH_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include <map>
#include <vector>

namespace llvm {
class IndirectBranchPass : public PassInfoMixin<IndirectBranchPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  void numberBasicBlock(Function &F);
  GlobalVariable *getIndirectTargets(Function &F, ConstantInt *EncKey);

private:
  std::map<BasicBlock *, unsigned> BBNumbering;
  std::vector<BasicBlock *> BBTargets; // all conditional branch targets
};
} // namespace llvm

#endif // LLVM_INDIRECTBRANCH_H
