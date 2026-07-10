#ifndef OLLVM_ANTI_ANALYSIS_H
#define OLLVM_ANTI_ANALYSIS_H

#include "llvm/IR/PassManager.h"

namespace llvm {
struct AntiAnalysisJumpPass : public PassInfoMixin<AntiAnalysisJumpPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};
} // namespace llvm

#endif // OLLVM_ANTI_ANALYSIS_H
