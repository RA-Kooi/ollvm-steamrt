#ifndef LLVM_INDIRECTCALL_H
#define LLVM_INDIRECTCALL_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include "CryptoUtils.h"

#include <map>
#include <vector>

namespace llvm {
class IndirectCallPass : public PassInfoMixin<IndirectCallPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  bool runOnFunction(Function &F);
  GlobalVariable *getIndirectCallees(Function &F, ConstantInt *EncKey);
  void numberCallees(Function &F);

private:
  std::vector<CallInst *> CallSites;
  std::vector<Function *> Callees;
  std::map<Function *, unsigned> CalleeNumbering;
  CryptoUtils RandomEngine;
};
} // namespace llvm

#endif // LLVM_INDIRECTCALL_H
