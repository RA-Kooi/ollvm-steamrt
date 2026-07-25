#include "llvm/Transforms/Obfuscation/IndirectCall.h"

#include "llvm/IR/AbstractCallSite.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <algorithm>
#include <iterator>
#include <random>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

#define DEBUG_TYPE "icall"

static cl::opt<bool> IcallEnabled("icall", cl::init(false),
                                  cl::desc("Indirect Call"));

static cl::opt<unsigned> IcallMinCallees(
    "icall-min-callees",
    cl::init(20),
    cl::desc(
        "The minimum size of the indirect call tables. "
        "Bigger size means more binary bloat, but more noise."));

struct Mat2x2 {
  static Mat2x2 genInvertible();
  Mat2x2 inverse() const;

  uint64_t A, B, C, D;
};

static void runOnFunction(
    Function &Fn,
    FunctionAnalysisManager &FAM,
    IntegerType *IntType,
    std::vector<CallInst *> const &CallSites,
    std::unordered_map<Constant *, size_t> const &Callee2Idx,
    GlobalVariable *Callees,
    GlobalVariable *Keys,
    GlobalVariable *SubKeys,
    GlobalVariable *Dummies,
    bool Bits64);

static std::unordered_map<Constant *, size_t> createCalleeTables(
    Module &M,
    IntegerType *IntType,
    std::vector<CallInst *> const &CallSites,
    bool Bits64);

static std::vector<CallInst *> findCallSites(Module &M);
static std::vector<Function *> genDummyFuncs(Module &M, size_t CallSiteCount);

static void createIcallTable(
    Module &M,
    GlobalVariable *Callees,
    GlobalVariable *Keys,
    GlobalVariable *DummyCallees,
    GlobalVariable *EncryptedCallees,
    GlobalVariable *EncryptedDummies);

static Value *emitPolynomials(IRBuilder<> &IRB, Value *Input, bool AlwaysTrue);
static Value *emitInvMatrix(IRBuilder<> &IRB, Value *X, Value *Y, bool AlwaysTrue);

PreservedAnalyses IndirectCallPass::run(Module &M,
                                        ModuleAnalysisManager &AM) {
  std::vector<CallInst *> CallSites = findCallSites(M);

  if (CallSites.empty())
    return PreservedAnalyses::all();

  // NOTE(Rafaël): Do a double check, since everything below modifies the
  // module.
  bool ShouldObfuscate = false;
  for (Function &F: M) {
    if (shouldObfuscate(IcallEnabled, &F, "icall")) {
      ShouldObfuscate = true;
      break;
    }
  }

  if (!ShouldObfuscate)
    return PreservedAnalyses::all();

  LLVMContext &Ctx = M.getContext();
  unsigned PointerSize = M.getDataLayout().getPointerSize();
  IntegerType *IntType = Type::getInt32Ty(Ctx);
  if (PointerSize == 8)
    IntType = Type::getInt64Ty(Ctx);

  auto Callee2Idx = createCalleeTables(
      M,
      IntType,
      CallSites,
      PointerSize == 8);

  auto *Callees = M.getNamedGlobal("ollvm_icall_callees");
  auto *Keys = M.getNamedGlobal("ollvm_icall_keys");
  auto *SubKeys = M.getNamedGlobal("ollvm_icall_subkeys");
  auto *DummyCallees = M.getNamedGlobal("ollvm_icall_dummies");
  auto *EncryptedCallees = M.getNamedGlobal("ollvm_icall_data");
  auto *EncryptedDummies = M.getNamedGlobal("ollvm_icall_data_dummy");

  createIcallTable(
      M,
      Callees,
      Keys,
      DummyCallees,
      EncryptedCallees,
      EncryptedDummies);

  auto &FAMProxy = AM.getResult<FunctionAnalysisManagerModuleProxy>(M);
  FunctionAnalysisManager &FAM = FAMProxy.getManager();

  for (Function &F: M) {
    if (shouldObfuscate(IcallEnabled, &F, "icall")) {
      runOnFunction(
          F,
          FAM,
          IntType,
          CallSites,
          Callee2Idx,
          EncryptedCallees,
          Keys,
          SubKeys,
          EncryptedDummies,
          PointerSize == 8);
    }
  }

  return PreservedAnalyses::none();
}

static void runOnFunction(
    Function &Fn,
    FunctionAnalysisManager &FAM,
    IntegerType *IntType,
    std::vector<CallInst *> const &CallSites,
    std::unordered_map<Constant *, size_t> const &Callee2Idx,
    GlobalVariable *Callees,
    GlobalVariable *Keys,
    GlobalVariable *SubKeys,
    GlobalVariable *Dummies,
    bool Bits64) {
  LLVMContext &Ctx = Fn.getContext();
  Module &M = *Fn.getParent();

  PointerType *OpaquePtrTy = PointerType::getUnqual(M.getContext());
  auto *ATy = dyn_cast<ArrayType>(Keys->getValueType());

  ConstantInt *Zero = ConstantInt::get(IntType, 0);

  auto RandomRegister = [IntType](IRBuilder<> &IRB) -> Value* {
    auto *AsmType = FunctionType::get(IntType, {}, false);
    auto *Asm = InlineAsm::get(AsmType, "", "=r", true);
    return IRB.CreateCall(Asm);
  };

  for (CallInst *CI : CallSites) {
    SmallVector<Value *, 8> Args;
    SmallVector<AttributeSet, 8> ArgAttrVec;

    CallBase *CB = CI;

    // NOTE(Rafaël): CB->getParent() returns null when the instruction was
    // replaced by a previous pass. Check if the parent function is the same
    // as the function this is being run on. We want to be able to have function
    // level control on whether this pass runs on it or not.
    if (!CB->getParent() || CB->getParent()->getParent() != &Fn)
      continue;

    AbstractCallSite CS(&CI->getCalledOperandUse());
    CB = CS.getInstruction();

    Function *Callee = CS.getCalledFunction();

    IRBuilder<> IRB(CB);

    Constant *OpaqueCallee = ConstantExpr::getBitCast(Callee, OpaquePtrTy);

    auto It = Callee2Idx.find(OpaqueCallee);
    assert(It != Callee2Idx.end() && "Unable to find Callee");

    size_t Idx = It->second;
    Constant *TargetIdx = ConstantInt::get(IntType, Idx);;

    bool RealIsTrue = Cryptoutils->getUint8T() & 1;
    bool DoSplit = Cryptoutils->getUint8T() & 1;

    bool DoMatrix = Cryptoutils->getUint8T() & 1;

    Value *X = RandomRegister(IRB);

    Value *Cond;
    if (!DoMatrix)
      Cond = emitPolynomials(IRB, X, RealIsTrue);
    else {
      Value *Y = RandomRegister(IRB);

      Cond = emitInvMatrix(IRB, X, Y, RealIsTrue, Bits64);
    }

    auto EmitDestPtr = [&IRB, Zero, TargetIdx, IntType, ATy, Keys, SubKeys](
        GlobalVariable *Table) {
      Value *EncDestAddr = IRB.CreateGEP(ATy, Table, {Zero, TargetIdx});
      EncDestAddr = IRB.CreateLoad(IntType, EncDestAddr);

      Value *Key = IRB.CreateGEP(ATy, Keys, {Zero, TargetIdx});
      Key = IRB.CreateLoad(IntType, Key);

      Value *SubKey = IRB.CreateGEP(ATy, SubKeys, {Zero, TargetIdx});
      SubKey = IRB.CreateLoad(IntType, SubKey);
      SubKey = IRB.CreateNeg(SubKey);

      Value *DestAddr = IRB.CreateXor(Key, EncDestAddr);
      DestAddr = IRB.CreateIntToPtr(DestAddr, IRB.getPtrTy());
      DestAddr = IRB.CreateGEP(IRB.getPtrTy(), DestAddr, SubKey);

      return DestAddr;
    };

    if (DoSplit) {
      // Split the current block at the call instruction into 4 basic blocks.
      // Essentially creating if(condition) { encPtr ^= encKey; } else {
      // encPtr ^= fakeKey; }, with the condition randomly being inverted so
      // it's not always the same path being taken.
      BasicBlock *ParentBB = CB->getParent();
      BasicBlock *Split = ParentBB->splitBasicBlock(CB);
      ParentBB->getTerminator()->eraseFromParent();

      BasicBlock *TrueBB = BasicBlock::Create(Ctx, "True", &Fn, Split);
      BasicBlock *FalseBB = BasicBlock::Create(Ctx, "False", &Fn, Split);

      IRB.SetInsertPoint(ParentBB);
      IRB.CreateCondBr(Cond, TrueBB, FalseBB);

      IRB.SetInsertPoint(TrueBB);

      Value *DestPtrTrue = EmitDestPtr(RealIsTrue ? Callees : Dummies);

      IRB.CreateBr(Split);

      IRB.SetInsertPoint(FalseBB);

      Value *DestPtrFalse = EmitDestPtr(!RealIsTrue ? Callees : Dummies);

      IRB.CreateBr(Split);

      IRB.SetInsertPoint(&Split->front());

      PHINode *Phi = IRB.CreatePHI(IRB.getPtrTy(), 2);
      Phi->addIncoming(DestPtrTrue, TrueBB);
      Phi->addIncoming(DestPtrFalse, FalseBB);

      CB->setCalledOperand(Phi);

      continue;
    }

    Value *DestPtrTrue = EmitDestPtr(RealIsTrue ? Callees : Dummies);
    Value *DestPtrFalse = EmitDestPtr(!RealIsTrue ? Callees : Dummies);
    Value *Sel = IRB.CreateSelect(Cond, DestPtrTrue, DestPtrFalse);

    CB->setCalledOperand(Sel);
  }
}

// Create 5 tables. The first table holds the raw function pointers to all the
// callees. The second table holds their encryption keys. The third table holds
// a shuffled copy of the first table. The fourth table holds a table in which
// we'll place the encrypted functions. The fifth table is a shuffled copy of
// the fourth table. See createIcallTable.
static std::unordered_map<Constant *, size_t> createCalleeTables(
    Module &M,
    IntegerType *IntType,
    std::vector<CallInst *> const &CallSites,
    bool Bits64) {
  PointerType *Opaque = PointerType::getUnqual(M.getContext());
  ConstantInt *Zero = ConstantInt::get(IntType, 0);

  std::default_random_engine Engine(Cryptoutils->getUint64T());

  std::unordered_set<Function *> Callees;
  std::transform(
      CallSites.cbegin(),
      CallSites.cend(),
      std::inserter(Callees, Callees.begin()),
      [](CallInst *CI)
      {
        AbstractCallSite CS(&CI->getCalledOperandUse());
        return CS.getCalledFunction();
      });

  std::vector<Function *> Dummies = genDummyFuncs(M, Callees.size());

  Callees.insert(Dummies.cbegin(), Dummies.cend());

  //                     Normal,    Encrypted, SubKey
  std::vector<std::tuple<Constant*, Constant*, Constant*>> CalleeFuncs;
  CalleeFuncs.reserve(Callees.size());

  std::vector<Constant*> CalleeKeys;
  CalleeKeys.reserve(Callees.size());

  for (auto &Callee : Callees) {
    uint64_t Key = Bits64
      ? Cryptoutils->getUint64T()
      : Cryptoutils->getUint32T();

    ConstantInt *SubKey = ConstantInt::get(IntType, Key);

    Constant *CE = ConstantExpr::getBitCast(Callee, Opaque);
    Constant *CEnc = ConstantExpr::getGetElementPtr(IntType, CE, SubKey);

    CalleeFuncs.push_back({CE, CEnc, SubKey});

    Key = Bits64
      ? Cryptoutils->getUint64T()
      : Cryptoutils->getUint32T();

    Constant *K = ConstantInt::get(IntType, Key);

    CalleeKeys.push_back(K);
  }

  std::shuffle(CalleeFuncs.begin(), CalleeFuncs.end(), Engine);

  std::unordered_map<Constant *, size_t> Callee2Idx;
  Callee2Idx.reserve(CalleeFuncs.size());

  for (size_t I = 0; I < CalleeFuncs.size(); ++I)
    Callee2Idx[std::get<0>(CalleeFuncs[I])] = I;

  ArrayType *ATy = ArrayType::get(Opaque, CalleeFuncs.size());

  std::vector<Constant*> Tmp;
  Tmp.reserve(CalleeFuncs.size());
  for (auto &P : CalleeFuncs)
    Tmp.push_back(std::get<1>(P));

  auto *CA = ConstantArray::get(ATy, Tmp);

  auto *GV = new GlobalVariable(
      M,
      ATy,
      true,
      GlobalValue::LinkageTypes::PrivateLinkage,
      CA,
      "ollvm_icall_callees");

  appendToCompilerUsed(M, {GV});

  std::shuffle(Tmp.begin(), Tmp.end(), Engine);

  ATy = ArrayType::get(Opaque, CalleeFuncs.size());
  CA = ConstantArray::get(ATy, Tmp);

  GV = new GlobalVariable(
      M,
      ATy,
      true,
      GlobalValue::LinkageTypes::PrivateLinkage,
      CA,
      "ollvm_icall_dummies");

  appendToCompilerUsed(M, {GV});

  ATy = ArrayType::get(IntType, CalleeFuncs.size());
  CA = ConstantArray::get(ATy, CalleeKeys);

  GV = new GlobalVariable(
      M,
      ATy,
      true,
      GlobalValue::LinkageTypes::PrivateLinkage,
      CA,
      "ollvm_icall_keys");

  appendToCompilerUsed(M, {GV});

  Tmp.clear();
  for (auto &P : CalleeFuncs)
    Tmp.push_back(std::get<2>(P));

  CA = ConstantArray::get(ATy, Tmp);

  GV = new GlobalVariable(
      M,
      ATy,
      true,
      GlobalValue::LinkageTypes::PrivateLinkage,
      CA,
      "ollvm_icall_subkeys");

  appendToCompilerUsed(M, {GV});

  std::vector<Constant *> Zeros(CalleeFuncs.size(), Zero);

  ATy = ArrayType::get(IntType, CalleeFuncs.size());
  CA = ConstantArray::get(ATy, Zeros);

  GV = new GlobalVariable(
      M,
      ATy,
      false,
      GlobalValue::LinkageTypes::PrivateLinkage,
      CA,
      "ollvm_icall_data");

  appendToCompilerUsed(M, {GV});

  GV = new GlobalVariable(
      M,
      ATy,
      false,
      GlobalValue::LinkageTypes::PrivateLinkage,
      CA,
      "ollvm_icall_data_dummy");

  appendToCompilerUsed(M, {GV});

  return Callee2Idx;
}

static std::vector<CallInst *> findCallSites(Module &M) {
  std::vector<CallInst *> CallSites;

  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (!isa<CallInst>(&I))
          continue;

        CallInst *CI = cast<CallInst>(&I);

        Function *Callee = CI->getCalledFunction();
        if (!Callee || Callee->isIntrinsic())
          continue;

        CallSites.push_back(CI);
      }
    }
  }

  return CallSites;
}

static std::vector<Function *> genDummyFuncs(Module &M, size_t CalleeCount) {
  if (CalleeCount >= IcallMinCallees)
    return {};

  size_t Diff = IcallMinCallees - CalleeCount;

  std::vector<Function *> DummyFuncs;
  DummyFuncs.reserve(Diff);

  LLVMContext &Ctx = M.getContext();
  FunctionType *FuncTy = FunctionType::get(Type::getVoidTy(Ctx), {}, false);

  for (size_t I = 0; I < Diff; ++I) {
    Function *Dummy = Function::Create(
        FuncTy,
        GlobalValue::LinkageTypes::ExternalLinkage,
        "ollvm_icall_dummy_"
          + std::to_string(Cryptoutils->getUint32T())
          + "_"
          + std::to_string(I),
        M);

    appendToUsed(M, {Dummy});

    IRBuilder<> IRB(Ctx);

    BasicBlock *BB = BasicBlock::Create(Ctx, "", Dummy);
    IRB.SetInsertPoint(BB);
    IRB.CreateRetVoid();

    DummyFuncs.push_back(Dummy);
  }

  return DummyFuncs;
}

static void createIcallTable(
    Module &M,
    GlobalVariable *Callees,
    GlobalVariable *Keys,
    GlobalVariable *Dummies,
    GlobalVariable *EncryptedCallees,
    GlobalVariable *EncryptedDummies) {
  /*
   * void ollvm_icall_encrypt()
   * {
   *     int idx = 0;
   *
   *     do
   *     {
   *         uintptr_t *curCall  = ollvm_icall_callees[idx];
   *         uintptr_t *curKey   = ollvm_icall_keys[idx];
   *         uintptr_t *curDummy = ollvm_icall_dummies[idx];
   *
   *         ollvm_icall_data[idx]       = *curCall  ^ *curKey;
   *         ollvm_icall_data_dummy[idx] = *curDummy ^ *curKey;
   *     } while(idx <= sizeof(ollvm_icall_callees));
   * }
   * */
  LLVMContext &Ctx = M.getContext();

  unsigned PointerSize = M.getDataLayout().getPointerSize();
  IntegerType *IntType = Type::getInt32Ty(Ctx);
  if (PointerSize == 8)
    IntType = Type::getInt64Ty(Ctx);

  ConstantInt *Zero = ConstantInt::get(IntType, 0);
  ConstantInt *One = ConstantInt::get(IntType, 1);

  IRBuilder<> IRB(Ctx);

  FunctionType *FuncTy = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
  PointerType *OpaquePtrTy = IRB.getPtrTy();

  // NOTE(Rafaël): Create the function with ExternalLinkage, since the linker
  // only checks which functions are being called from main. This function runs
  // as a static constructor before C(++) constructors to make sure they're able
  // to run if they've been obfuscated by this pass.
  Function *EncryptFunc = Function::Create(
      FuncTy,
      GlobalValue::LinkageTypes::ExternalLinkage,
      "ollvm_icall_encrypt_" + std::to_string(Cryptoutils->getUint32T()),
      M);

  appendToUsed(M, {EncryptFunc});

  // NOTE(Rafaël): Make sure our constructor runs before any C(++) constructor
  // by setting the priority to the lowest we can. Technically we're invading in
  // libc++'s space here, but as people can use attribute init_priority to set
  // the priority as low as 101, we need to make sure we run before those. As
  // we don't touch anything that libc++ touches this should be safe.
  appendToGlobalCtors(M, EncryptFunc, 100, nullptr);

  BasicBlock *Base = BasicBlock::Create(Ctx, "Base", EncryptFunc);
  BasicBlock *LoopLocals = BasicBlock::Create(Ctx, "LoopLocals", EncryptFunc);
  BasicBlock *LoopBody = BasicBlock::Create(Ctx, "LoopBody", EncryptFunc);
  BasicBlock *End = BasicBlock::Create(Ctx, "End", EncryptFunc);

  IRB.SetInsertPoint(Base);

  IRB.CreateBr(LoopLocals);

  IRB.SetInsertPoint(LoopLocals);

  auto *CalleeATy = dyn_cast<ArrayType>(Callees->getValueType());
  auto *KeyATy = dyn_cast<ArrayType>(Keys->getValueType());

  uint64_t LastIdx = CalleeATy->getNumElements() - 1;
  ConstantInt *EndIdx = ConstantInt::get(IntType, LastIdx);

  Value *CurIdx = IRB.getIntN(IntType->getBitWidth(), 0);

  IRB.CreateBr(LoopBody);

  IRB.SetInsertPoint(LoopBody);

  PHINode *CurIdxPhi = IRB.CreatePHI(IntType, 2);
  CurIdxPhi->addIncoming(CurIdx, LoopLocals);

  Value *CurCall = IRB.CreateGEP(CalleeATy, Callees, {Zero, CurIdxPhi});
  Value *CurKey = IRB.CreateGEP(KeyATy, Keys, {Zero, CurIdxPhi});
  Value *CurDummy = IRB.CreateGEP(CalleeATy, Dummies, {Zero, CurIdxPhi});

  Value *CurCallVal = IRB.CreateLoad(IntType, CurCall);
  Value *CurKeyVal = IRB.CreateLoad(IntType, CurKey);
  Value *CurDummyVal = IRB.CreateLoad(IntType, CurDummy);

  auto EmitXorStore = [
    &IRB,
    OpaquePtrTy,
    KeyATy,
    Zero,
    CurIdxPhi,
    CurKeyVal
  ](Value *CurVal, GlobalVariable *DataTable) {
    Value *CurEnc = IRB.CreateGEP(KeyATy, DataTable, {Zero, CurIdxPhi});

    Value *Res = IRB.CreateXor(CurVal, CurKeyVal);
    Res = IRB.CreateIntToPtr(Res, OpaquePtrTy);
    IRB.CreateStore(Res, CurEnc);
  };

  // Randomize the order of the loads to harden the constructor functions.
  bool CallFirst = Cryptoutils->getUint8T() & 1;
  if (CallFirst) {
    EmitXorStore(CurCallVal, EncryptedCallees);
    EmitXorStore(CurDummyVal, EncryptedDummies);
  }
  else {
    EmitXorStore(CurDummyVal, EncryptedDummies);
    EmitXorStore(CurCallVal, EncryptedCallees);
  }

  Value *NextIdx = IRB.CreateAdd(CurIdxPhi, One);
  CurIdxPhi->addIncoming(NextIdx, LoopBody);

  Value *Cond = IRB.CreateICmpULE(CurIdxPhi, EndIdx);
  IRB.CreateCondBr(Cond, LoopBody, End);

  IRB.SetInsertPoint(End);
  IRB.CreateRetVoid();
}

static Value *emitPolynomials(IRBuilder<> &IRB, Value *Input, bool AlwaysTrue) {
  Type *IntType = Input->getType();

  auto RandCoeff = [](bool Force, bool ForceOdd) {
    uint64_t Val = Cryptoutils->getUint64T();
    constexpr uint64_t One = 1;
    if (Force) {
      if (ForceOdd)
        return Val | 1;

      return Val | ~One;
    }

    return Val;
  };

  // Based on Rivest's theorem we generate a permutation polynomial of:
  // P(x) = a_0 + a_1x + a_2x^2 + a_3x^3 for which we enforce:
  // 1. a_1 is odd.
  // 2. a_2 is even.
  // 3. a_3 is even.
  Value *A0 = ConstantInt::get(IntType, RandCoeff(false, false));
  Value *A1 = ConstantInt::get(IntType, RandCoeff(true, true));
  Value *A2 = ConstantInt::get(IntType, RandCoeff(true, false));
  Value *A3 = ConstantInt::get(IntType, RandCoeff(true, false));

  auto EmitPx = [&IRB, A0, A1, A2, A3](Value *X) {
    Value *X2 = IRB.CreateMul(X, X);
    Value *X3 = IRB.CreateMul(X2, X);

    Value *A1X = IRB.CreateMul(A1, X);
    Value *A2X2 = IRB.CreateMul(A2, X2);
    Value *A3X3 = IRB.CreateMul(A3, X3);

    Value *Px = IRB.CreateAdd(A0, A1X);
    Px = IRB.CreateAdd(Px, A2X2);
    Px = IRB.CreateAdd(Px, A3X3);

    return Px;
  };

  Value *Px = EmitPx(Input);

  Constant *One = ConstantInt::get(IntType, 1);
  Value *X1 = IRB.CreateAdd(Input, One);

  Value *Px1 = EmitPx(X1);

  // Since P(x) is a bijection, P(x) != P(x + 1) is always true.
  if (AlwaysTrue)
    return IRB.CreateICmpNE(Px, Px1);

  return IRB.CreateICmpEQ(Px, Px1);
}

static Value *emitInvMatrix(IRBuilder<> &IRB, Value *X, Value *Y, bool AlwaysTrue) {
  Type *XType = X->getType();
  Type *IntType = XType;

  Mat2x2 Mat = Mat2x2::genInvertible();
  Mat2x2 InvMat = Mat.inverse();

  Constant *MA = ConstantInt::get(IntType, Mat.A);
  Constant *MB = ConstantInt::get(IntType, Mat.B);
  Constant *MC = ConstantInt::get(IntType, Mat.C);
  Constant *MD = ConstantInt::get(IntType, Mat.D);

  Constant *IMA = ConstantInt::get(IntType, InvMat.A);
  Constant *IMB = ConstantInt::get(IntType, InvMat.B);
  Constant *IMC = ConstantInt::get(IntType, InvMat.C);
  Constant *IMD = ConstantInt::get(IntType, InvMat.D);

  // Construct V'
  Value *MAX = IRB.CreateMul(X, MA);
  Value *MBY = IRB.CreateMul(Y, MB);
  Value *VPX = IRB.CreateAdd(MAX, MBY);

  Value *MCX = IRB.CreateMul(X, MC);
  Value *MDY = IRB.CreateMul(Y, MD);
  Value *VPY = IRB.CreateAdd(MCX, MDY);

  // Construct V''
  Value *IMAX = IRB.CreateMul(VPX, IMA);
  Value *IMBY = IRB.CreateMul(VPY, IMB);
  Value *VPPX = IRB.CreateAdd(IMAX, IMBY);

  Value *IMCX = IRB.CreateMul(VPX, IMC);
  Value *IMDY = IRB.CreateMul(VPY, IMD);
  Value *VPPY = IRB.CreateAdd(IMCX, IMDY);

  // V'' == x, where x = [X, Y]
  Value *CondX = IRB.CreateICmpEQ(VPPX, X);
  Value *CondY = IRB.CreateICmpEQ(VPPY, Y);

  Value *Cond = IRB.CreateAnd(CondX, CondY);

  if (AlwaysTrue)
    return Cond;

  return IRB.CreateNot(Cond);
}

Mat2x2 Mat2x2::genInvertible() {
  uint64_t A = Cryptoutils->getUint64T() | 1;
  uint64_t B = Cryptoutils->getUint64T() & ~1uLL;
  uint64_t C = Cryptoutils->getUint64T();
  uint64_t D = Cryptoutils->getUint64T() | 1;

  return Mat2x2{A, B, C, D};
}

Mat2x2 Mat2x2::inverse() const {
  uint64_t Det = (A * D) - (B * C);
  if (!(Det & 1))
    std::abort();

  uint64_t DetInv = Det;
  for (int I = 0; I < 5; ++I)
    DetInv = DetInv * (2 - Det * DetInv);

  uint64_t InvA =  D * DetInv;
  uint64_t InvB = -B * DetInv;
  uint64_t InvC = -C * DetInv;
  uint64_t InvD =  A * DetInv;

  return Mat2x2{InvA, InvB, InvC, InvD};
}
