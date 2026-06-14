#ifndef LLVM_SPLIT_BASIC_BLOCK_H
#define LLVM_SPLIT_BASIC_BLOCK_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"

namespace llvm {
class SplitBasicBlockPass : public PassInfoMixin<SplitBasicBlockPass> {
public:
  bool flag;
  SplitBasicBlockPass(bool flag) { this->flag = flag; }
  PreservedAnalyses run(Function &F,
                        FunctionAnalysisManager &AM);

  void split(Function *f);
  bool containsPHI(BasicBlock *BB);
  void shuffle(std::vector<int> &vec);
  static bool isRequired() { return true; }
};

SplitBasicBlockPass *createSplitBasicBlock(bool flag);
} // namespace llvm

#endif // LLVM_SPLIT_BASIC_BLOCK_H
