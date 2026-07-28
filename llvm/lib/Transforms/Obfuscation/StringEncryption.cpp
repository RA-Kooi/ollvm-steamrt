#include "llvm/Transforms/Obfuscation/StringEncryption.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/GlobalStatus.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

#define DEBUG_TYPE "strenc"

using namespace llvm;

static cl::opt<bool> SobfEnabled("sobf", cl::init(false),
                                 cl::desc("String Obfuscation"));

namespace {
struct EncryptedGV {
  GlobalVariable *GV;
  uint64_t Key;
  uint32_t Len;
};

struct CSPEntry {
  CSPEntry()
      : ID(0), Offset(0), DecGV(nullptr), DecStatus(nullptr), DecFunc(nullptr) {
  }

  unsigned ID;
  unsigned Offset;
  GlobalVariable *DecGV;
  GlobalVariable *DecStatus; // is decrypted or not
  std::vector<uint8_t> Data;
  std::vector<uint8_t> EncKey;
  Function *DecFunc;
};

struct CSUser {
  CSUser(Type *ETy, GlobalVariable *User, GlobalVariable *NewGV)
      : Ty(ETy), GV(User), DecGV(NewGV), DecStatus(nullptr), InitFunc(nullptr) {
  }

  Type *Ty;
  GlobalVariable *GV;
  GlobalVariable *DecGV;
  GlobalVariable *DecStatus; // is decrypted or not
  Function *InitFunc;        // InitFunc will use decryted string to
  // initialize DecGV
};

struct PassState {
  PassState() = default;

  std::vector<std::unique_ptr<CSPEntry>> ConstantStringPool;
  std::map<GlobalVariable *, CSPEntry *> CSPEntryMap;
  std::map<GlobalVariable *, std::unique_ptr<CSUser>> CSUserMap;
  GlobalVariable *EncryptedStringTable;
  std::set<GlobalVariable *> MaybeDeadGlobalVars;

  std::map<Function * /*Function*/, GlobalVariable * /*Decryption Status*/>
      Encstatus;

  bool doStrEnc(Module &M, ModuleAnalysisManager &AM, bool Enabled);

  bool processConstantStringUse(Function *F);

  bool processConstantStringBlockPHI(
      Instruction &Inst,
      SmallPtrSet<GlobalVariable *, 16> &DecryptedGV,
      PHINode *PHI);

  bool processConstantStringBlock(
      Instruction &Inst,
      SmallPtrSet<GlobalVariable *, 16> &DecryptedGV);

  void deleteUnusedGlobalVariable();
  void getRandomBytes(std::vector<uint8_t> &Bytes, uint32_t MinSize,
                      uint32_t MaxSize);
};
} // namespace

static void collectConstantStringUser(GlobalVariable *CString,
                                      std::set<GlobalVariable *> &Users);

static bool isValidToEncrypt(GlobalVariable *GV);
static Function *buildDecryptFunction(Module *M, const CSPEntry *Entry);
static Function *buildInitFunction(Module *M, const CSUser *User);

static void lowerGlobalConstant(Constant *CV, IRBuilder<> &IRB, Value *Ptr,
                                Type *Ty);

static void lowerGlobalConstantStruct(ConstantStruct *CS, IRBuilder<> &IRB,
                                      Value *Ptr, Type *Ty);

static void lowerGlobalConstantArray(ConstantArray *CA, IRBuilder<> &IRB,
                                     Value *Ptr, Type *Ty);

bool PassState::doStrEnc(Module &M, ModuleAnalysisManager &AM, bool Enabled) {
  std::set<GlobalVariable *> ConstantStringUsers;

  // collect all c strings

  LLVMContext &Ctx = M.getContext();
  ConstantInt *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);

  for (GlobalVariable &GV : M.globals()) {
    if (!GV.isConstant() || !GV.hasInitializer() ||
        GV.hasDLLExportStorageClass() || GV.isDLLImportDependent()) {
      continue;
    }

    Constant *Init = GV.getInitializer();

    if (Init == nullptr)
      continue;

    auto *CDS = dyn_cast<ConstantDataSequential>(Init);
    if (!CDS)
      continue;

    if (!CDS->isCString())
      continue;

    StringRef Name = GV.getName();

    // NOTE(Rafaël): Skip RTTI type names, as this breaks libunwind assumptions
    // and causes an infinite loop.
    if (Name.starts_with("_ZTS")
        || Name.starts_with("_ZTI")
        || Name.starts_with("_ZTV")) {
      continue;
    }

    auto Entry = std::make_unique<CSPEntry>();
    StringRef Data = CDS->getRawDataValues();
    Entry->Data.reserve(Data.size());

    for (unsigned I = 0; I < Data.size(); ++I)
      Entry->Data.push_back(static_cast<uint8_t>(Data[I]));

    Entry->ID = static_cast<unsigned>(ConstantStringPool.size());

    auto *ZeroInit = ConstantAggregateZero::get(CDS->getType());

    auto *DecGV = new GlobalVariable(
        M,
        CDS->getType(),
        false,
        GlobalValue::ExternalLinkage,
        ZeroInit,
        "dec" + Twine::utohexstr(Entry->ID) + GV.getName());

    auto *DecStatus = new GlobalVariable(
        M,
        Type::getInt32Ty(Ctx),
        false,
        GlobalValue::ExternalLinkage,
        Zero,
        "dec_status_" + Twine::utohexstr(Entry->ID) + GV.getName());

    DecGV->setAlignment(MaybeAlign(GV.getAlignment()));
    Entry->DecGV = DecGV;
    Entry->DecStatus = DecStatus;
    CSPEntryMap[&GV] = Entry.get();
    ConstantStringPool.push_back(std::move(Entry));

    collectConstantStringUser(&GV, ConstantStringUsers);
  }

  // encrypt those strings, build corresponding decrypt function
  for (auto &Entry : ConstantStringPool) {
    getRandomBytes(Entry->EncKey, 16, 32);

    for (unsigned I = 0; I < Entry->Data.size(); ++I)
      Entry->Data[I] ^= Entry->EncKey[I % Entry->EncKey.size()];

    Entry->DecFunc = buildDecryptFunction(&M, Entry.get());
  }

  // build initialization function for supported constant string users
  for (GlobalVariable *GV : ConstantStringUsers) {
    if (!isValidToEncrypt(GV))
      continue;

    Type *EltType = GV->getValueType();
    ConstantAggregateZero *ZeroInit = ConstantAggregateZero::get(EltType);

    auto *DecGV = new GlobalVariable(
        M,
        EltType,
        false,
        GlobalValue::PrivateLinkage,
        ZeroInit,
        "dec_" + GV->getName());

    DecGV->setAlignment(MaybeAlign(GV->getAlignment()));

    auto *DecStatus = new GlobalVariable(
        M,
        Type::getInt32Ty(Ctx),
        false,
        GlobalValue::PrivateLinkage,
        Zero,
        "dec_status_" + GV->getName());

    auto User = std::make_unique<CSUser>(EltType, GV, DecGV);
    User->DecStatus = DecStatus;
    User->InitFunc = buildInitFunction(&M, User.get());

    CSUserMap[GV] = std::move(User);
  }

  // emit the constant string pool
  // | junk bytes | key 1 | encrypted string 1 | junk bytes | key 2 | encrypted
  // string 2 | ...
  std::vector<uint8_t> Data;
  std::vector<uint8_t> JunkBytes;

  JunkBytes.reserve(32);
  for (auto &Entry : ConstantStringPool) {
    JunkBytes.clear();
    getRandomBytes(JunkBytes, 16, 32);
    Data.insert(Data.end(), JunkBytes.begin(), JunkBytes.end());

    Entry->Offset = static_cast<unsigned>(Data.size());
    Data.insert(Data.end(), Entry->EncKey.begin(), Entry->EncKey.end());
    Data.insert(Data.end(), Entry->Data.begin(), Entry->Data.end());
  }

  Constant *CDA = ConstantDataArray::get(M.getContext(), ArrayRef<uint8_t>(Data));

  EncryptedStringTable = new GlobalVariable(
      M,
      CDA->getType(),
      true,
      GlobalValue::PrivateLinkage,
      CDA,
      "EncryptedStringTable");

  // decrypt string back at every use, change the plain string use to the
  // decrypted one
  bool Changed = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    Changed |= processConstantStringUse(&F);
  }

  for (auto &I : CSUserMap) {
    CSUser *User = I.second.get();
    Changed |= processConstantStringUse(User->InitFunc);
  }

  // delete unused global variables
  deleteUnusedGlobalVariable();

  for (auto &Entry : ConstantStringPool) {
    if (Entry->DecFunc->use_empty())
      Entry->DecFunc->eraseFromParent();
  }

  return Changed;
}

PreservedAnalyses StringEncryptionPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
  if (SobfEnabled) {
    PassState State;
    if (State.doStrEnc(M, AM, SobfEnabled))
      return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

void PassState::getRandomBytes(std::vector<uint8_t> &Bytes, uint32_t MinSize,
                               uint32_t MaxSize) {
  uint32_t N = Cryptoutils->getUint32T();
  uint32_t Len;

  assert(MaxSize >= MinSize);

  if (MinSize == MaxSize) {
    Len = MinSize;
  } else {
    Len = MinSize + (N % (MaxSize - MinSize));
  }

  Bytes.resize(Len);
  Cryptoutils->getBytes(reinterpret_cast<char *>(Bytes.data()), Len);
}

/*static void goron_decrypt_string(uint8_t *plain_string, const uint8_t *data)
{
  const uint8_t *key = data;
  uint32_t key_size = 1234;
  uint8_t *es = (uint8_t *) &data[key_size];
  uint32_t i;
  for (i = 0;i < 5678;i ++) {
    plain_string[i] = es[i] ^ key[i % key_size];
  }
}*/

static Function *buildDecryptFunction(Module *M, const CSPEntry *Entry) {
  LLVMContext &Ctx = M->getContext();
  IRBuilder<> IRB(Ctx);

  FunctionType *FuncTy = FunctionType::get(
      Type::getVoidTy(Ctx),
      {IRB.getPtrTy(), IRB.getPtrTy()},
      false);

  Function *DecFunc = Function::Create(
      FuncTy,
      GlobalValue::LinkageTypes::ExternalLinkage,
      "goron_decrypt_string_" + Twine::utohexstr(Entry->ID),
      M);

  DecFunc->addFnAttr(Attribute::NoUnwind);

  auto *ArgIt = DecFunc->arg_begin();
  Argument *PlainString = ArgIt; // output
  ++ArgIt;
  Argument *Data = ArgIt; // input

  Attribute NoCapture = Attribute::getWithCaptureInfo(Ctx, CaptureInfo::none());
  PlainString->setName("plain_string");
  PlainString->addAttr(NoCapture);
  Data->setName("data");
  Data->addAttr(NoCapture);
  Data->addAttr(Attribute::ReadOnly);

  BasicBlock *Enter = BasicBlock::Create(Ctx, "Enter", DecFunc);
  BasicBlock *LoopBody = BasicBlock::Create(Ctx, "LoopBody", DecFunc);

  BasicBlock *UpdateDecStatus = BasicBlock::Create(
      Ctx,
      "UpdateDecStatus",
      DecFunc);

  BasicBlock *Exit = BasicBlock::Create(Ctx, "Exit", DecFunc);

  IRB.SetInsertPoint(Enter);

  ConstantInt *KeySize = ConstantInt::get(
      Type::getInt32Ty(Ctx),
      Entry->EncKey.size());

  Value *EncPtr = IRB.CreateInBoundsGEP(IRB.getInt8Ty(), Data, KeySize);

  Value *DecStatus = IRB.CreateLoad(
      Entry->DecStatus->getValueType(),
      Entry->DecStatus);

  Value *IsDecrypted = IRB.CreateICmpEQ(DecStatus, IRB.getInt32(1));
  IRB.CreateCondBr(IsDecrypted, Exit, LoopBody);

  IRB.SetInsertPoint(LoopBody);
  PHINode *LoopCounter = IRB.CreatePHI(IRB.getInt32Ty(), 2);
  LoopCounter->addIncoming(IRB.getInt32(0), Enter);

  Value *EncCharPtr = IRB.CreateInBoundsGEP(
      IRB.getInt8Ty(),
      EncPtr,
      LoopCounter);

  Value *EncChar = IRB.CreateLoad(IRB.getInt8Ty(), EncCharPtr);
  Value *KeyIdx = IRB.CreateURem(LoopCounter, KeySize);

  Value *KeyCharPtr = IRB.CreateInBoundsGEP(IRB.getInt8Ty(), Data, KeyIdx);
  Value *KeyChar = IRB.CreateLoad(IRB.getInt8Ty(), KeyCharPtr);

  Value *DecChar = IRB.CreateXor(EncChar, KeyChar);

  Value *DecCharPtr = IRB.CreateInBoundsGEP(
      IRB.getInt8Ty(),
      PlainString,
      LoopCounter);

  IRB.CreateStore(DecChar, DecCharPtr);

  Value *NewCounter = IRB.CreateAdd(
      LoopCounter,
      IRB.getInt32(1),
      "",
      true,
      true);

  LoopCounter->addIncoming(NewCounter, LoopBody);

  Value *Cond = IRB.CreateICmpEQ(
      NewCounter,
      IRB.getInt32(static_cast<uint32_t>(Entry->Data.size())));

  IRB.CreateCondBr(Cond, UpdateDecStatus, LoopBody);

  IRB.SetInsertPoint(UpdateDecStatus);
  IRB.CreateStore(IRB.getInt32(1), Entry->DecStatus);
  IRB.CreateBr(Exit);

  IRB.SetInsertPoint(Exit);
  IRB.CreateRetVoid();

  return DecFunc;
}

static Function *buildInitFunction(Module *M, const CSUser *User) {
  LLVMContext &Ctx = M->getContext();
  IRBuilder<> IRB(Ctx);

  FunctionType *FuncTy = FunctionType::get(
      Type::getVoidTy(Ctx),
      {User->DecGV->getType()},
      false);

  Function *InitFunc = Function::Create(
      FuncTy,
      GlobalValue::LinkageTypes::ExternalLinkage,
      "__global_variable_initializer_" + User->GV->getName(), M);

  InitFunc->addFnAttr(Attribute::NoUnwind);

  auto *ArgIt = InitFunc->arg_begin();
  Argument *Thiz = ArgIt;

  Attribute NoCapture = Attribute::getWithCaptureInfo(Ctx, CaptureInfo::none());
  Thiz->setName("this");
  Thiz->addAttr(NoCapture);

  // convert constant initializer into a series of instructions
  BasicBlock *Enter = BasicBlock::Create(Ctx, "Enter", InitFunc);
  BasicBlock *InitBlock = BasicBlock::Create(Ctx, "InitBlock", InitFunc);
  BasicBlock *Exit = BasicBlock::Create(Ctx, "Exit", InitFunc);

  IRB.SetInsertPoint(Enter);

  Value *DecStatus = IRB.CreateLoad(
      User->DecStatus->getValueType(),
      User->DecStatus);

  Value *IsDecrypted = IRB.CreateICmpEQ(DecStatus, IRB.getInt32(1));
  IRB.CreateCondBr(IsDecrypted, Exit, InitBlock);

  IRB.SetInsertPoint(InitBlock);
  Constant *Init = User->GV->getInitializer();
  lowerGlobalConstant(Init, IRB, User->DecGV, User->Ty);
  IRB.CreateStore(IRB.getInt32(1), User->DecStatus);
  IRB.CreateBr(Exit);

  IRB.SetInsertPoint(Exit);
  IRB.CreateRetVoid();

  return InitFunc;
}

static void lowerGlobalConstant(Constant *CV, IRBuilder<> &IRB, Value *Ptr,
                                Type *Ty) {
  if (isa<ConstantAggregateZero>(CV)) {
    IRB.CreateStore(CV, Ptr);
    return;
  }

  if (auto *CA = dyn_cast<ConstantArray>(CV))
    lowerGlobalConstantArray(CA, IRB, Ptr, Ty);
  else if (auto *CS = dyn_cast<ConstantStruct>(CV))
    lowerGlobalConstantStruct(CS, IRB, Ptr, Ty);
  else
    IRB.CreateStore(CV, Ptr);
}

static void lowerGlobalConstantArray(ConstantArray *CA, IRBuilder<> &IRB,
                                     Value *Ptr, Type *Ty) {
  for (unsigned I = 0, E = CA->getNumOperands(); I != E; ++I) {
    Constant *CV = CA->getOperand(I);
    Value *GEP = IRB.CreateGEP(Ty, Ptr, {IRB.getInt32(0), IRB.getInt32(I)});
    lowerGlobalConstant(CV, IRB, GEP, CV->getType());
  }
}

static void lowerGlobalConstantStruct(ConstantStruct *CS, IRBuilder<> &IRB,
                                      Value *Ptr, Type *Ty) {
  for (unsigned I = 0, E = CS->getNumOperands(); I != E; ++I) {
    Constant *CV = CS->getOperand(I);
    Value *GEP = IRB.CreateGEP(Ty, Ptr, {IRB.getInt32(0), IRB.getInt32(I)});
    lowerGlobalConstant(CV, IRB, GEP, CV->getType());
  }
}

bool PassState::processConstantStringUse(Function *F) {
  lowerConstantExpr(*F);
  SmallPtrSet<GlobalVariable *, 16> DecryptedGV;

  // if GV has multiple use in a block, decrypt only at the first use
  bool Changed = false;
  for (BasicBlock &BB : *F) {
    DecryptedGV.clear();

    if (BB.isEHPad())
      continue;

    for (Instruction &Inst : BB) {
      if (Inst.isEHPad())
        continue;

      if (PHINode *PHI = dyn_cast<PHINode>(&Inst))
        Changed = processConstantStringBlockPHI(Inst, DecryptedGV, PHI);
      else
        Changed = processConstantStringBlock(Inst, DecryptedGV);
    }
  }

  return Changed;
}

bool PassState::processConstantStringBlockPHI(
    Instruction &Inst,
    SmallPtrSet<GlobalVariable *, 16> &DecryptedGV,
    PHINode *PHI) {
  bool Changed = false;

  for (unsigned int I = 0; I < PHI->getNumIncomingValues(); ++I) {
    GlobalVariable *GV = dyn_cast<GlobalVariable>(PHI->getIncomingValue(I));
    if (!GV) {
      continue;
    }
    auto Iter1 = CSPEntryMap.find(GV);
    auto Iter2 = CSUserMap.find(GV);

    if (Iter2 != CSUserMap.end()) { // GV is a constant string user
      CSUser *User = Iter2->second.get();

      if (DecryptedGV.count(GV) > 0) {
        Inst.replaceUsesOfWith(GV, User->DecGV);
        continue;
      }

      Instruction *InsertPoint = PHI->getIncomingBlock(I)->getTerminator();
      IRBuilder<> IRB(InsertPoint);
      CallInst *CI = IRB.CreateCall(User->InitFunc, {User->DecGV});
      CI->setTailCall();

      Inst.replaceUsesOfWith(GV, User->DecGV);
      MaybeDeadGlobalVars.insert(GV);
      DecryptedGV.insert(GV);

      Changed = true;
    } else if (Iter1 != CSPEntryMap.end()) { // GV is a constant string
    CSPEntry *Entry = Iter1->second;

    if (DecryptedGV.count(GV) > 0) {
      Inst.replaceUsesOfWith(GV, Entry->DecGV);
      continue;
    }

    Instruction *InsertPoint = PHI->getIncomingBlock(I)->getTerminator();
    IRBuilder<> IRB(InsertPoint);

    Value *OutBuf = IRB.CreateBitCast(Entry->DecGV, IRB.getPtrTy());

    Value *Data = IRB.CreateInBoundsGEP(
        EncryptedStringTable->getValueType(),
        EncryptedStringTable,
        {IRB.getInt32(0), IRB.getInt32(Entry->Offset)});

    CallInst *CI = IRB.CreateCall(Entry->DecFunc, {OutBuf, Data});
    CI->setTailCall();

    Inst.replaceUsesOfWith(GV, Entry->DecGV);
    MaybeDeadGlobalVars.insert(GV);
    DecryptedGV.insert(GV);

    Changed = true;
    }
  }

  return Changed;
}

bool PassState::processConstantStringBlock(
    Instruction &Inst,
    SmallPtrSet<GlobalVariable *, 16> &DecryptedGV) {
  bool Changed = false;

  for (User::op_iterator Op = Inst.op_begin(); Op != Inst.op_end(); ++Op) {
    if (GlobalVariable *GV = dyn_cast<GlobalVariable>(*Op)) {
      auto Iter1 = CSPEntryMap.find(GV);
      auto Iter2 = CSUserMap.find(GV);

      if (Iter2 != CSUserMap.end()) {
        CSUser *User = Iter2->second.get();

        if (DecryptedGV.count(GV) > 0) {
          Inst.replaceUsesOfWith(GV, User->DecGV);
          continue;
        }

        IRBuilder<> IRB(&Inst);
        CallInst *CI = IRB.CreateCall(User->InitFunc, {User->DecGV});
        CI->setTailCall();

        Inst.replaceUsesOfWith(GV, User->DecGV);
        MaybeDeadGlobalVars.insert(GV);
        DecryptedGV.insert(GV);

        Changed = true;
      } else if (Iter1 != CSPEntryMap.end()) {
        CSPEntry *Entry = Iter1->second;
        if (DecryptedGV.count(GV) > 0) {
          Inst.replaceUsesOfWith(GV, Entry->DecGV);
          continue;
        }

        IRBuilder<> IRB(&Inst);
        Value *OutBuf = IRB.CreateBitCast(Entry->DecGV, IRB.getPtrTy());

        Value *Data = IRB.CreateInBoundsGEP(
            EncryptedStringTable->getValueType(),
            EncryptedStringTable,
            {IRB.getInt32(0), IRB.getInt32(Entry->Offset)});

        CallInst *CI = IRB.CreateCall(Entry->DecFunc, {OutBuf, Data});
        CI->setTailCall();

        Inst.replaceUsesOfWith(GV, Entry->DecGV);
        MaybeDeadGlobalVars.insert(GV);
        DecryptedGV.insert(GV);

        Changed = true;
      }
    }
  }

  return Changed;
}

static void collectConstantStringUser(GlobalVariable *CString,
                                      std::set<GlobalVariable *> &Users) {
  SmallPtrSet<Value *, 16> Visited;
  SmallVector<Value *, 16> ToVisit;

  ToVisit.push_back(CString);

  while (!ToVisit.empty()) {
    Value *V = ToVisit.pop_back_val();

    if (Visited.count(V) > 0)
      continue;

    Visited.insert(V);

    for (Value *User : V->users()) {
      if (auto *GV = dyn_cast<GlobalVariable>(User))
        Users.insert(GV);
      else
        ToVisit.push_back(User);
    }
  }
}

static bool isValidToEncrypt(GlobalVariable *GV) {
  if (GV->isConstant() && GV->hasInitializer()) {
    return GV->getInitializer() != nullptr;
  }

  return false;
}

void PassState::deleteUnusedGlobalVariable() {
  // NOTE(Rafaël): I think this addresses the case where global variables are
  // interdependent and become dead after a dependent has been removed.
  bool Changed = true;
  while (Changed) {
    Changed = false;

    for (auto Iter = MaybeDeadGlobalVars.begin();
         Iter != MaybeDeadGlobalVars.end();) {
      GlobalVariable *GV = *Iter;

      if (!GV->hasLocalLinkage()) {
        ++Iter;
        continue;
      }

      GV->removeDeadConstantUsers();
      if (!GV->use_empty()) {
        ++Iter;
        continue;
      }

      if (GV->hasInitializer()) {
        Constant *Init = GV->getInitializer();
        GV->setInitializer(nullptr);

        if (isSafeToDestroyConstant(Init))
          Init->destroyConstant();
      }

      Iter = MaybeDeadGlobalVars.erase(Iter);
      GV->eraseFromParent();
      Changed = true;
    }
  }
}
