#ifndef OBFUSCATION_IPOBFUSCATIONCONTEXT_H
#define OBFUSCATION_IPOBFUSCATIONCONTEXT_H

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include <map>
#include <set>
#include <vector>

// Namespace
namespace llvm {
class ModulePass;
class FunctionPass;
class PassRegistry;

struct IPObfuscationContext : public ModulePass {
  static char ID;

  IPObfuscationContext()
      : ModulePass(ID), Enabled(false), LocalFunctions(), IPOInfoList(),
        IPOInfoMap(), DeadSlots() {}

  explicit IPObfuscationContext(bool Enable)
      : ModulePass(ID), Enabled(Enable), LocalFunctions(), IPOInfoList(),
        IPOInfoMap(), DeadSlots() {}

  bool runOnModule(Module &M) override;
  bool doFinalization(Module &) override;

private:
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

private:
  void surveyFunction(Function &F);
  Function *insertSecretArgument(Function *F);
  void computeCallSiteSecretArgument(Function *F);
  IPOInfo *allocaSecretSlot(Function &F);
  const IPOInfo *getIPOInfo(Function *F);

private:
  bool Enabled;

  std::set<Function *> LocalFunctions;
  SmallVector<IPOInfo *, 16> IPOInfoList;
  std::map<Function *, IPOInfo *> IPOInfoMap;
  std::vector<AllocaInst *> DeadSlots;
};

IPObfuscationContext *createIPObfuscationContextPass(bool Enabled);
void initializeIPObfuscationContextPass(PassRegistry &Registry);
} // namespace llvm

#endif
