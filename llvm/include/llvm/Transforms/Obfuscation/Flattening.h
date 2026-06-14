#ifndef LLVM_FLATTENING_H
#define LLVM_FLATTENING_H

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <cstdlib>
#include <ctime>
#include <vector>

namespace llvm {
class FlatteningPass : public PassInfoMixin<FlatteningPass> {
public:
  bool flag;
  FlatteningPass(bool flag) { this->flag = flag; }
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  bool flatten(Function *f);
  static bool isRequired() { return true; }
};
FlatteningPass *createFlattening(bool flag);
} // namespace llvm

#endif // LLVM_FLATTENING_H
