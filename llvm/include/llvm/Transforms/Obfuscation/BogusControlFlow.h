#ifndef _BOGUSCONTROLFLOW_H_
#define _BOGUSCONTROLFLOW_H_

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class BogusControlFlowPass : public PassInfoMixin<BogusControlFlowPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  void bogus(Function &F);
  void addBogusFlow(BasicBlock *BasicBlock, Function &F);
  bool doF(Module &M, Function &F);
  static bool isRequired() { return true; }
};
} // namespace llvm

#endif
