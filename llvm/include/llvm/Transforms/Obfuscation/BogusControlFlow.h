#ifndef _BOGUSCONTROLFLOW_H_
#define _BOGUSCONTROLFLOW_H_

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"

namespace llvm {
class BogusControlFlowPass : public PassInfoMixin<BogusControlFlowPass> {
public:
  bool flag;
  BogusControlFlowPass(bool flag) { this->flag = flag; }
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  void bogus(Function &F);
  void addBogusFlow(BasicBlock *basicBlock, Function &F);
  bool doF(Module &M, Function &F);
  static bool isRequired() { return true; }
};

BogusControlFlowPass *createBogusControlFlow(bool flag);
} // namespace llvm

#endif
