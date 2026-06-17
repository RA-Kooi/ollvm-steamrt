#ifndef OBFUSCATION_IPOBFUSCATIONCONTEXT_H
#define OBFUSCATION_IPOBFUSCATIONCONTEXT_H

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

// Namespace
namespace llvm {
struct IPObfuscationContext {
  /* Inter-procedural obfuscation secret info of a function */
  struct IPOInfo {
    IPOInfo(AllocaInst *CallerAI, AllocaInst *CalleeAI, LoadInst *LI,
            ConstantInt *Value)
        : CallerSlot(CallerAI), CalleeSlot(CalleeAI), SecretLI(LI),
          SecretCI(Value) {}
    // Stack slot use to store caller's secret token
    AllocaInst *CallerSlot;
    // Stack slot use to store callee's secret argument
    AllocaInst *CalleeSlot;
    // Load caller secret from caller's slot or the secret argument passed by
    // caller
    LoadInst *SecretLI;
    // A random constant value
    ConstantInt *SecretCI;
  };

  const IPOInfo *getIPOInfo(Function *F);

  void surveyFunction(Function &F);
  Function *insertSecretArgument(Function *F);
  void computeCallSiteSecretArgument(Function *F);
  std::unique_ptr<IPOInfo> allocaSecretSlot(Function &F);

  std::mutex Lock;

  std::set<Function *> LocalFunctions;
  SmallVector<std::unique_ptr<IPOInfo>, 16> IPOInfoList;
  std::map<Function *, IPOInfo *> IPOInfoMap;
  std::vector<AllocaInst *> DeadSlots;
};

static IPObfuscationContext IPO;

struct IPObfuscationContextPass
    : public PassInfoMixin<IPObfuscationContextPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};
} // namespace llvm

#endif
