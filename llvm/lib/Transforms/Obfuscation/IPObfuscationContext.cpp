#include "llvm/Transforms/Obfuscation/IPObfuscationContext.h"

#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include <mutex>

#if LLVM_VERSION_MAJOR > 10
#include "llvm/Transforms/Obfuscation/compat/CallSite.h"
#else
#include "llvm/IR/CallSite.h"
#endif

#define DEBUG_TYPE "ipobf"

using namespace llvm;

PreservedAnalyses IPObfuscationContextPass::run(Module &M,
                                                ModuleAnalysisManager &AM) {
  std::lock_guard<std::mutex> Guard(IPO.Lock);

  for (Function &F : M) {
    IPO.surveyFunction(F);
  }

  for (Function &F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    auto Info = IPO.allocaSecretSlot(F);

    IPO.IPOInfoList.push_back(std::move(Info));
    IPO.IPOInfoMap[&F] = IPO.IPOInfoList.back().get();
  }

  std::vector<Function *> NewFuncs;
  for (Function *F : IPO.LocalFunctions) {
    Function *NF = IPO.insertSecretArgument(F);
    NewFuncs.push_back(NF);
  }

  for (Function *F : NewFuncs) {
    IPO.computeCallSiteSecretArgument(F);
  }

  for (AllocaInst *Slot : IPO.DeadSlots) {
    for (auto I = Slot->use_begin(), E = Slot->use_end(); I != E; ++I) {
      if (Instruction *Inst = dyn_cast<Instruction>(I->getUser())) {
        Inst->eraseFromParent();
      }
    }
    Slot->eraseFromParent();
  }

  return PreservedAnalyses::none();
}

void IPObfuscationContext::surveyFunction(Function &F) {
  if (!F.hasLocalLinkage() || F.isDeclaration()) {
    return;
  }

  for (const Use &U : F.uses()) {
    ImmutableCallSite CS(U.getUser());
    if (!CS || !CS.isCallee(&U)) {
      return;
    }

    const Instruction *TheCall = CS.getInstruction();
    if (!TheCall) {
      return;
    }
  }

  LLVM_DEBUG(dbgs() << "Enqueue Local Function  " << F.getName() << "\n");
  LocalFunctions.insert(&F);
}

Function *IPObfuscationContext::insertSecretArgument(Function *F) {
  FunctionType *FTy = F->getFunctionType();
  std::vector<Type *> Params;

  SmallVector<AttributeSet, 8> ArgAttrVec;
  const AttributeList &PAL = F->getAttributes();

  Params.push_back(Type::getInt32Ty(F->getContext()));
  ArgAttrVec.push_back(AttributeSet());

  unsigned AttribCount = 0;
  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E;
       ++I, ++AttribCount) {
    Params.push_back(I->getType());
    ArgAttrVec.push_back(PAL.getParamAttrs(AttribCount));
  }

  // Find out the new return value.
  Type *RetTy = FTy->getReturnType();

// The existing function return attributes.
#if LLVM_VERSION_MAJOR >= 13
  AttributeSet RAttrs = PAL.getRetAttrs();
#else
  AttributeSet RAttrs = PAL.getRetAttributes();
#endif

// Reconstruct the AttributesList based on the vector we constructed.
#if LLVM_VERSION_MAJOR >= 13
  AttributeList NewPAL =
      AttributeList::get(F->getContext(), PAL.getFnAttrs(), RAttrs, ArgAttrVec);
#else
  AttributeList NewPAL = AttributeList::get(
      F->getContext(), PAL.getFnAttributes(), RAttrs, ArgAttrVec);
#endif

  // Create the new function type based on the recomputed parameters.
  FunctionType *NFTy = FunctionType::get(RetTy, Params, FTy->isVarArg());

  // Create the new function body and insert it into the module...
  Function *NF = Function::Create(NFTy, F->getLinkage());
  NF->copyAttributesFrom(F);
  NF->setComdat(F->getComdat());
  NF->setAttributes(NewPAL);
  // Insert the new function before the old function, so we won't be processing
  // it again.
  F->getParent()->getFunctionList().insert(F->getIterator(), NF);
  NF->takeName(F);
  NF->setSubprogram(F->getSubprogram());

  SmallVector<Value *, 8> Args;
  while (!F->use_empty()) {
    CallSite CS(F->user_back());
    Instruction *Call = CS.getInstruction();

    ArgAttrVec.clear();
    const AttributeList &CallPAL = CS.getAttributes();

    // Get the Secret Token
    Function *Caller = Call->getParent()->getParent();
    IPOInfo *SecretInfo = IPOInfoMap[Caller];
    Args.push_back(SecretInfo->CalleeSlot);
    ArgAttrVec.push_back(AttributeSet());
    // Declare these outside of the loops, so we can reuse them for the second
    // loop, which loops the varargs.
    CallSite::arg_iterator I = CS.arg_begin();
    unsigned AttrCount = 0;
    // Loop over those operands, corresponding to the normal arguments to the
    // original function, and add those that are still alive.
    for (unsigned E = FTy->getNumParams(); AttrCount != E; ++I, ++AttrCount) {
      Args.push_back(*I);
#if LLVM_VERSION_MAJOR >= 13
      AttributeSet Attrs = CallPAL.getParamAttrs(AttrCount);
#else
      AttributeSet Attrs = CallPAL.getParamAttributes(AttrCount);
#endif
      ArgAttrVec.push_back(Attrs);
    }

    // Push any varargs arguments on the list. Don't forget their attributes.
    for (CallSite::arg_iterator E = CS.arg_end(); I != E; ++I, ++AttrCount) {
      Args.push_back(*I);
#if LLVM_VERSION_MAJOR >= 13
      ArgAttrVec.push_back(CallPAL.getParamAttrs(AttrCount));
#else
      ArgAttrVec.push_back(CallPAL.getParamAttributes(AttrCount));
#endif
    }

// Reconstruct the AttributesList based on the vector we constructed.
#if LLVM_VERSION_MAJOR >= 13
    AttributeList NewCallPAL =
        AttributeList::get(F->getContext(), CallPAL.getFnAttrs(),
                           CallPAL.getRetAttrs(), ArgAttrVec);
#else
    AttributeList NewCallPAL =
        AttributeList::get(F->getContext(), CallPAL.getFnAttributes(),
                           CallPAL.getRetAttributes(), ArgAttrVec);
#endif

    Instruction *New;
    if (InvokeInst *II = dyn_cast<InvokeInst>(Call)) {
      New = InvokeInst::Create(NF, II->getNormalDest(), II->getUnwindDest(),
                               Args, "", Call);
      cast<InvokeInst>(New)->setCallingConv(CS.getCallingConv());
      cast<InvokeInst>(New)->setAttributes(NewCallPAL);
    } else {
      New = CallInst::Create(NF, Args, "", Call);
      cast<CallInst>(New)->setCallingConv(CS.getCallingConv());
      cast<CallInst>(New)->setAttributes(NewCallPAL);
      if (cast<CallInst>(Call)->isTailCall())
        cast<CallInst>(New)->setTailCall();
    }
    New->setDebugLoc(Call->getDebugLoc());

    Args.clear();

    if (!Call->use_empty()) {
      Call->replaceAllUsesWith(New);
      New->takeName(Call);
    }

    // Finally, remove the old call from the program, reducing the use-count of
    // F.
    Call->eraseFromParent();
  }

  NF->splice(NF->begin(), F);

  // Loop over the argument list, transferring uses of the old arguments over to
  // the new arguments, also transferring over the names as well.
  Function::arg_iterator I2 = NF->arg_begin();
  I2->setName("SecretArg");
  ++I2;
  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E;
       ++I) {
    I->replaceAllUsesWith(I2);
    I2->takeName(I);
    ++I2;
  }

  // Load Secret Token from the secret argument
  IntegerType *I32Ty = Type::getInt32Ty(NF->getContext());
  IRBuilder<> IRB(&NF->getEntryBlock().front());
  Value *Ptr = IRB.CreateBitCast(NF->arg_begin(), I32Ty->getPointerTo());
  LoadInst *MySecret = IRB.CreateLoad(Ptr->getType(), Ptr);

  IPOInfo *Info = IPOInfoMap[F];
  Info->SecretLI->eraseFromParent();
  Info->SecretLI = MySecret;
  DeadSlots.push_back(Info->CallerSlot);

  IPOInfoMap[NF] = Info;
  IPOInfoMap.erase(F);

  F->eraseFromParent();

  return NF;
}

// Create StackSlots for Secrets and a LoadInst for caller's secret slot
std::unique_ptr<IPObfuscationContext::IPOInfo>
IPObfuscationContext::allocaSecretSlot(Function &F) {
  IRBuilder<> IRB(&F.getEntryBlock().front());
  IntegerType *I32Ty = Type::getInt32Ty(F.getContext());
  AllocaInst *CallerSlot = IRB.CreateAlloca(I32Ty, nullptr, "CallerSlot");
  CallerSlot->setAlignment(Align(4));
  AllocaInst *CalleeSlot = IRB.CreateAlloca(I32Ty, nullptr, "CalleeSlot");
  CalleeSlot->setAlignment(Align(4));

  uint32_t V = Cryptoutils->getUint32T();
  ConstantInt *SecretCI = ConstantInt::get(I32Ty, V, false);
  IRB.CreateStore(SecretCI, CallerSlot);
  LoadInst *MySecret =
      IRB.CreateLoad(CallerSlot->getType(), CallerSlot, "MySecret");

  std::unique_ptr<IPOInfo> Info(
      new IPOInfo(CallerSlot, CalleeSlot, MySecret, SecretCI));

  return Info;
}

const IPObfuscationContext::IPOInfo *
IPObfuscationContext::getIPOInfo(Function *F) {
  return IPOInfoMap[F];
}

// at each callsite, compute the callee's secret argument using the caller's
void IPObfuscationContext::computeCallSiteSecretArgument(Function *F) {
  IPOInfo *CalleeIPOInfo = IPOInfoMap[F];

  for (const Use &U : F->uses()) {
    CallSite CS(U.getUser());
    Instruction *Call = CS.getInstruction();
    IRBuilder<> IRB(Call);

    Function *Caller = Call->getParent()->getParent();
    IPOInfo *CallerIPOInfo = IPOInfoMap[Caller];

    Value *CallerSecret;
    CallerSecret = CallerIPOInfo->SecretLI;

    Constant *X =
        ConstantExpr::getSub(CallerIPOInfo->SecretCI, CalleeIPOInfo->SecretCI);
    Value *CalleeSecret = IRB.CreateSub(CallerSecret, X);
    IRB.CreateStore(CalleeSecret, CallerIPOInfo->CalleeSlot);
  }
}
