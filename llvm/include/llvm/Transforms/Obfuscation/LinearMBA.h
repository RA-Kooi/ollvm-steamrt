#ifndef OBFUSCATOR_LINEAR_MBA_H
#define OBFUSCATOR_LINEAR_MBA_H

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
struct BitwiseTerm {
  int TruthTable[4];
  Value *(*Builder)(IRBuilder<> &, Value *, Value *);
};

struct LinearMBATerm {
  BitwiseTerm *TermInfo;
  int64_t Coefficient;
};

struct LinearMBA : PassInfoMixin<LinearMBA> {
  PreservedAnalyses run(Function &M, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm
#endif
