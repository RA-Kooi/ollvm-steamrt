#include "llvm/Transforms/Obfuscation/IndirectCall.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Obfuscation/IPObfuscationContext.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Obfuscation/compat/CallSite.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <mutex>

using namespace llvm;

static cl::opt<bool> IcallEnabled("icall", cl::init(false),
                                  cl::desc("Indirect Call"));

PreservedAnalyses IndirectCallPass::run(Function &F,
                                        FunctionAnalysisManager &AM) {
  if (shouldObfuscate(IcallEnabled, &F, "icall")) {
    std::lock_guard<std::mutex> Guard(IPO.Lock);

    runOnFunction(F);
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

bool IndirectCallPass::runOnFunction(Function &Fn) {
  LLVMContext &Ctx = Fn.getContext();

  CalleeNumbering.clear();
  Callees.clear();
  CallSites.clear();

  numberCallees(Fn);

  if (Callees.empty())
    return false;

  uint64_t V = RandomEngine.getUint64T();
  IntegerType *IntType = Type::getInt32Ty(Ctx);

  unsigned PointerSize = Fn.getParent()->getDataLayout().getPointerSize();

  if (PointerSize == 8)
    IntType = Type::getInt64Ty(Ctx);

  ConstantInt *EncKey = ConstantInt::get(IntType, V, false);

  const IPObfuscationContext::IPOInfo *SecretInfo = IPO.getIPOInfo(&Fn);

  Value *MySecret = nullptr;
  if (SecretInfo)
    MySecret = SecretInfo->SecretLI;
  else
    MySecret = ConstantInt::get(IntType, 0, true);

  ConstantInt *Zero = ConstantInt::get(IntType, 0);
  GlobalVariable *Targets = getIndirectCallees(Fn, EncKey);

  for (auto *CI : CallSites) {
    SmallVector<Value *, 8> Args;
    SmallVector<AttributeSet, 8> ArgAttrVec;

    CallBase *CB = CI;

    Function *Callee = CB->getCalledFunction();
    FunctionType *FTy = CB->getFunctionType();
    IRBuilder<> IRB(CB);

    Args.clear();
    ArgAttrVec.clear();

    // clang-format off
    Value *Idx = ConstantInt::get(IntType, CalleeNumbering[CB->getCalledFunction()]);
    Value *GEP = IRB.CreateGEP(Targets->getValueType(), Targets, {Zero, Idx});
    LoadInst *EncDestAddr = IRB.CreateLoad(GEP->getType(), GEP, CI->getName());
    // clang-format on

    Constant *X;
    if (SecretInfo)
      X = ConstantExpr::getSub(SecretInfo->SecretCI, EncKey);
    else
      X = ConstantExpr::getSub(Zero, EncKey);

    const AttributeList &CallPAL = CB->getAttributes();
    auto *I = CB->arg_begin();
    unsigned I1 = 0;

    for (unsigned E = FTy->getNumParams(); I1 != E; ++I, ++I1) {
      Args.push_back(*I);
      AttributeSet Attrs = CallPAL.getParamAttrs(I1);
      ArgAttrVec.push_back(Attrs);
    }

    for (auto *E = CB->arg_end(); I != E; ++I, ++I1) {
      Args.push_back(*I);
      ArgAttrVec.push_back(CallPAL.getParamAttrs(I1));
    }

    // clang-format off
    AttributeList NewCallPAL = AttributeList::get(
        IRB.getContext(),
        CallPAL.getFnAttrs(),
        CallPAL.getRetAttrs(),
        ArgAttrVec);

    Value *Secret = IRB.CreateSub(X, MySecret);

    Value *DestAddr = IRB.CreateGEP(
        PointerType::getUnqual(Ctx),
        EncDestAddr,
        Secret);
    // clang-format on

    Value *FnPtr = IRB.CreateBitCast(DestAddr, FTy->getPointerTo());
    FnPtr->setName("Call_" + Callee->getName());
    CallInst *NewCall = IRB.CreateCall(FTy, FnPtr, Args, CB->getName());
    NewCall->setAttributes(NewCallPAL);
    CB->replaceAllUsesWith(NewCall);
    CB->eraseFromParent();
  }

  return true;
}

GlobalVariable *IndirectCallPass::getIndirectCallees(Function &F,
                                                     ConstantInt *EncKey) {
  std::string GVName(F.getName().str() + "_IndirectCallees");
  GlobalVariable *GV = F.getParent()->getNamedGlobal(GVName);
  if (GV)
    return GV;

  // callee's address
  // clang-format off
  std::vector<Constant *> Elements;
  for (Function *Callee : Callees) {
    Constant *CE = ConstantExpr::getBitCast(
        Callee,
        PointerType::getUnqual(F.getContext()));

    CE = ConstantExpr::getGetElementPtr(
        Type::getInt64Ty(F.getContext()),
        CE,
        EncKey);

    Elements.push_back(CE);
  }

  ArrayType *ATy = ArrayType::get(
      PointerType::getUnqual(F.getContext()),
      Elements.size());

  Constant *CA = ConstantArray::get(ATy, ArrayRef<Constant *>(Elements));

  GV = new GlobalVariable(
      *F.getParent(),
      ATy,
      false,
      GlobalValue::LinkageTypes::PrivateLinkage,
      CA,
      GVName);
  // clang-format on

  appendToCompilerUsed(*F.getParent(), {GV});

  return GV;
}

void IndirectCallPass::numberCallees(Function &F) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (!isa<CallInst>(&I))
        continue;

      CallSite CS(&I);
      Function *Callee = CS.getCalledFunction();
      if (Callee == nullptr)
        continue;

      if (Callee->isIntrinsic())
        continue;

      CallSites.push_back((CallInst *)&I);

      if (CalleeNumbering.count(Callee) > 0)
        continue;

      CalleeNumbering[Callee] = Callees.size();
      Callees.push_back(Callee);
    }
  }
}
