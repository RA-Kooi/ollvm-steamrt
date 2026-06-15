#ifndef LLVM_FLATTENING_H
#define LLVM_FLATTENING_H

#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/Local.h"

namespace llvm {
class FlatteningPass : public PassInfoMixin<FlatteningPass> {
public:
  explicit FlatteningPass(bool Enable) : Enabled(Enable) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  bool Enabled;
};

FlatteningPass *createFlattening(bool Enabled);
} // namespace llvm

#endif // LLVM_FLATTENING_H
