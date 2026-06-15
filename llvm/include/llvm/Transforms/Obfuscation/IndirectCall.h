#ifndef LLVM_INDIRECTCALL_H
#define LLVM_INDIRECTCALL_H

#include "llvm/Analysis/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "CryptoUtils.h"
#include "IPObfuscationContext.h"
#include "ObfuscationOptions.h"

#include <map>
#include <vector>

namespace llvm {
class IndirectCallPass : public PassInfoMixin<IndirectCallPass> {
public:
  explicit IndirectCallPass(bool Enable)
      : Enabled(Enable), IPO(new IPObfuscationContext()),
        Options(new ObfuscationOptions()), Callees(), CalleeNumbering(),
        RandomEngine() {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  bool doIndirctCall(Function &F);
  GlobalVariable *getIndirectCallees(Function &F, ConstantInt *EncKey);
  void numberCallees(Function &F);

private:
  bool Enabled;
  std::vector<CallInst *> CallSites;
  IPObfuscationContext *IPO;
  ObfuscationOptions *Options;
  std::vector<Function *> Callees;
  std::map<Function *, unsigned> CalleeNumbering;
  CryptoUtils RandomEngine;
};

IndirectCallPass *createIndirectCall(bool Enabled);
} // namespace llvm

#endif // LLVM_INDIRECTCALL_H
