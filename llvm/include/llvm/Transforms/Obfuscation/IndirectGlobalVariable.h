#ifndef LLVM_INDIRECTGLOBALVARIABLE_H
#define LLVM_INDIRECTGLOBALVARIABLE_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include <map>
#include <vector>

namespace llvm {
class IndirectGlobalVariablePass
    : public PassInfoMixin<IndirectGlobalVariablePass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  void numberGlobalVariable(Function &F);
  GlobalVariable *getIndirectGlobalVariables(Function &F, ConstantInt *EncKey);

private:
  std::map<GlobalVariable *, unsigned> GVNumbering;
  std::vector<GlobalVariable *> GlobalVariables;
};
} // namespace llvm

#endif // LLVM_INDIRECTGLOBALVARIABLE_H
