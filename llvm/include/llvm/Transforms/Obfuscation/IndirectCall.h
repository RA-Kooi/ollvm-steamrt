#ifndef LLVM_INDIRECTCALL_H
#define LLVM_INDIRECTCALL_H

#include "llvm/Analysis/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "CryptoUtils.h"
#include "IPObfuscationContext.h"
#include "ObfuscationOptions.h"
#include "Utils.h"
#include "compat/CallSite.h"

#include <random>

namespace llvm {
class IndirectCallPass : public PassInfoMixin<IndirectCallPass> {
public:
  bool flag;
  std::vector<CallInst *> CallSites;
  IPObfuscationContext *IPO;
  ObfuscationOptions *Options;
  std::vector<Function *> Callees;
  std::map<Function *, unsigned> CalleeNumbering;
  CryptoUtils RandomEngine;

  IndirectCallPass(bool flag) {
    this->flag = flag;
    this->IPO = new IPObfuscationContext;
    this->Options = new ObfuscationOptions;
  }

  bool doIndirctCall(Function &F);
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  GlobalVariable *getIndirectCallees(Function &F, ConstantInt *EncKey);
  void NumberCallees(Function &F);
  static bool isRequired() { return true; }
};

IndirectCallPass *createIndirectCall(bool flag);
} // namespace llvm

#endif // LLVM_INDIRECTCALL_H
