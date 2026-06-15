#include "llvm/Transforms/Obfuscation/IndirectCall.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Obfuscation/compat/CallSite.h"

using namespace llvm;

PreservedAnalyses IndirectCallPass::run(Function &F,
                                        FunctionAnalysisManager &AM) {
  if (toObfuscate(Enabled, &F, "icall")) {
    doIndirctCall(F);
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

bool IndirectCallPass::doIndirctCall(Function &Fn) {
  if (Options && Options->skipFunction(Fn.getName())) {
    return false;
  }

  LLVMContext &Ctx = Fn.getContext();

  CalleeNumbering.clear();
  Callees.clear();
  CallSites.clear();

  numberCallees(Fn);

  if (Callees.empty()) {
    return false;
  }

  uint64_t V = RandomEngine.getUint64T();
  IntegerType *IntType = Type::getInt32Ty(Ctx);

  unsigned PointerSize =
      Fn.getEntryBlock().getModule()->getDataLayout().getTypeAllocSize(
          PointerType::getUnqual(Fn.getContext()));
  if (PointerSize == 8) {
    IntType = Type::getInt64Ty(Ctx);
  }
  ConstantInt *EncKey = ConstantInt::get(IntType, V, false);
  ConstantInt *EncKey1 = ConstantInt::get(IntType, -V, false);

  Value *MySecret = ConstantInt::get(IntType, 0, true);

  ConstantInt *Zero = ConstantInt::get(IntType, 0);
  GlobalVariable *Targets = getIndirectCallees(Fn, EncKey1);

  for (auto *CI : CallSites) {
    SmallVector<Value *, 8> Args;
    SmallVector<AttributeSet, 8> ArgAttrVec;

    CallBase *CB = CI;

    Function *Callee = CB->getCalledFunction();
    FunctionType *FTy = CB->getFunctionType();
    IRBuilder<> IRB(CB);

    Args.clear();
    ArgAttrVec.clear();

    Value *Idx =
        ConstantInt::get(IntType, CalleeNumbering[CB->getCalledFunction()]);
    Value *GEP = IRB.CreateGEP(Targets->getValueType(), Targets, {Zero, Idx});
    LoadInst *EncDestAddr = IRB.CreateLoad(GEP->getType(), GEP, CI->getName());

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

    Value *Secret = IRB.CreateAdd(EncKey, MySecret);
    Value *DestAddr = IRB.CreateGEP(Type::getInt8Ty(Ctx), EncDestAddr, Secret);

    Value *FnPtr = IRB.CreateBitCast(DestAddr, FTy->getPointerTo());
    FnPtr->setName("Call_" + Callee->getName());
    CB->setCalledOperand(FnPtr);
  }

  return true;
}

GlobalVariable *IndirectCallPass::getIndirectCallees(Function &F,
                                                     ConstantInt *EncKey) {
  std::string GVName(F.getName().str() + "_IndirectCallees");
  GlobalVariable *GV = F.getParent()->getNamedGlobal(GVName);
  if (GV) {
    return GV;
  }
  // callee's address
  std::vector<Constant *> Elements;
  for (Function *Callee : Callees) {
    Constant *CE = ConstantExpr::getBitCast(
        Callee, llvm::PointerType::get(Type::getInt8Ty(F.getContext()), 0));
    CE = ConstantExpr::getGetElementPtr(Type::getInt8Ty(F.getContext()), CE,
                                        EncKey);
    Elements.push_back(CE);
  }
  ArrayType *ATy =
      ArrayType::get(Type::getInt8Ty(F.getContext()), Elements.size());
  Constant *CA = ConstantArray::get(ATy, ArrayRef<Constant *>(Elements));
  GV =
      new GlobalVariable(*F.getParent(), ATy, false,
                         GlobalValue::LinkageTypes::PrivateLinkage, CA, GVName);
  appendToCompilerUsed(*F.getParent(), {GV});
  return GV;
}

void IndirectCallPass::numberCallees(Function &F) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (isa<CallInst>(&I)) {
        CallSite CS(&I);
        Function *Callee = CS.getCalledFunction();
        if (Callee == nullptr) {
          continue;
        }
        if (Callee->isIntrinsic()) {
          continue;
        }
        CallSites.push_back((CallInst *)&I);
        if (CalleeNumbering.count(Callee) == 0) {
          CalleeNumbering[Callee] = Callees.size();
          Callees.push_back(Callee);
        }
      }
    }
  }
}

IndirectCallPass *llvm::createIndirectCall(bool Enabled) {
  return new IndirectCallPass(Enabled);
}
