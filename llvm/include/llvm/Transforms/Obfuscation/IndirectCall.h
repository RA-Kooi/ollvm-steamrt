#ifndef LLVM_INDIRECTCALL_H
#define LLVM_INDIRECTCALL_H

#include "llvm/IR/PassManager.h"

#include "CryptoUtils.h"
#include "IPObfuscationContext.h"
#include "ObfuscationOptions.h"

#include <map>
#include <vector>

namespace llvm {
class IndirectCallPass : public PassInfoMixin<IndirectCallPass> {
public:
  IndirectCallPass()
      : IPO(new IPObfuscationContext()), Options(new ObfuscationOptions()),
        Callees(), CalleeNumbering(), RandomEngine() {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  bool doIndirctCall(Function &F);
  GlobalVariable *getIndirectCallees(Function &F, ConstantInt *EncKey);
  void numberCallees(Function &F);

private:
  std::vector<CallInst *> CallSites;
  IPObfuscationContext *IPO;
  ObfuscationOptions *Options;
  std::vector<Function *> Callees;
  std::map<Function *, unsigned> CalleeNumbering;
  CryptoUtils RandomEngine;
};
} // namespace llvm

#endif // LLVM_INDIRECTCALL_H
