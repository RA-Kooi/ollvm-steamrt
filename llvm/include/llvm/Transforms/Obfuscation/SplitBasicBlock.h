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
  explicit SplitBasicBlockPass(bool Enable) : Enabled(Enable) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }

private:
  bool Enabled;
};

SplitBasicBlockPass *createSplitBasicBlock(bool Enabled);
} // namespace llvm

#endif // LLVM_SPLIT_BASIC_BLOCK_H
