#ifndef LLVM_SPLIT_BASIC_BLOCK_H
#define LLVM_SPLIT_BASIC_BLOCK_H

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"

#include <vector>

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
