#include "llvm/Transforms/Obfuscation/AliasAccess.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <type_traits>
#include <vector>

#define DEBUG_TYPE "aliaslocal"

using namespace llvm;

constexpr int DefaultBranchNum = 6;

struct ElementPos {
  StructType *Type;
  unsigned Index;
};

struct ReferenceNode {
  AllocaInst *AI;
  bool IsRaw;
  unsigned Id;
  std::map<AllocaInst *, ElementPos> RawInsts;
  std::map<unsigned, ReferenceNode *> Edges;
  std::map<AllocaInst *, std::vector<unsigned>> Path;
};

static cl::opt<bool> LaaEnabled(
    "laa",
    cl::init(false),
    cl::desc("Local alias access: Access local variables through an alias. "
             "See -laa_members."));

static cl::opt<int> LaaBranches(
    "laa_members",
    cl::desc("Choose how many members are at most inserted in the generated structs. "
             "-laa_members=x where x >= 1."),
    cl::init(DefaultBranchNum),
    cl::Optional);

static void process(Function &F);

template<typename T>
static void getRandomNoRepeat(T UpperBound, T Size, std::vector<T> &Result);

PreservedAnalyses AliasAccess::run(Function &F, FunctionAnalysisManager &AM) {
  if (shouldObfuscate(LaaEnabled, &F, "laa")) {
    if (LaaBranches < 1) {
      errs() << "AliasAcces branch count must be greater than 0.\n";
      return PreservedAnalyses::all();
    }

    process(F);

    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

static Function *buildGetterFunction(Module &M, StructType *ST, unsigned Index) {
  std::vector<Type *> Params;
  Params.push_back(PointerType::getUnqual(M.getContext()));

  FunctionType *FT = FunctionType::get(
      PointerType::getUnqual(M.getContext()),
      Params,
      false);

  Function *F = Function::Create(
      FT,
      GlobalValue::PrivateLinkage,
      Twine("__obfu_aliasaccess_getter"),
      M);

  BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);

  Function::arg_iterator Iter = F->arg_begin();
  Value *Ptr = Iter;

  IRBuilder<> IRB(Entry);
  IRB.CreateRet(IRB.CreateGEP(ST, Ptr, {IRB.getInt32(0), IRB.getInt32(Index)}));

  return F;
}

static void process(Function &F) {
  std::vector<AllocaInst *> AIs;
  std::map<unsigned, Function *> Getter;
  std::vector<std::unique_ptr<ReferenceNode>> Graph;
  std::vector<Type *> Slots;

  Type *PtrType = PointerType::getUnqual(F.getContext());
  StructType *TransST = StructType::create(F.getContext());

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (!isa<AllocaInst>(I))
        continue;

      AllocaInst *AI = (AllocaInst *)&I;
      if (AI->getAlign().value() <= 8)
        AIs.push_back((AllocaInst *)&I);
    }
  }

  for (int I = 0; I < LaaBranches; I++)
    Slots.push_back(PtrType);

  TransST->setBody(Slots);

  std::vector<std::vector<AllocaInst *>> Bucket;
  for (unsigned I = 0; I < AIs.size(); I++)
    Bucket.push_back(std::vector<AllocaInst *>());

  for (AllocaInst *AI : AIs) {
    unsigned Index = Cryptoutils->getUint32T() % AIs.size();
    Bucket[Index].push_back(AI);
  }

  unsigned Count = 0;
  IRBuilder<> IRB(&*F.getEntryBlock().getFirstInsertionPt());

  for (auto &Items : Bucket) {
    if (Items.size() == 0)
      continue;

    auto RN = std::make_unique<ReferenceNode>();
    RN->IsRaw = true;
    RN->Id = Count++;

    StructType *ST = StructType::create(F.getContext());

    size_t Num = Items.size() * 2 + 1;

    Slots.clear();

    for (unsigned I = 0; I < Num; I++)
      Slots.push_back(nullptr);

    // uint64_t AlignVal = 1;
    std::vector<size_t> Random;
    getRandomNoRepeat(Num, Items.size(), Random);

    for (unsigned I = 0; I < Items.size(); I++) {
      AllocaInst *AI = Items[I];
      // AlignVal = std::max(AI->getAlignment(), AlignVal);

      unsigned Idx = Random[I];
      Slots[Idx] = AI->getAllocatedType();

      ElementPos EP;
      EP.Type = ST;
      EP.Index = Idx;

      RN->RawInsts[AI] = EP;
    }

    for (unsigned I = 0; I < Num; I++) {
      if (!Slots[I])
        Slots[I] = PtrType;
    }

    ST->setBody(Slots);
    RN->AI = IRB.CreateAlloca(ST);

    // AlignVal = std::max(RN->AI->getAlignment(), AlignVal);
    // RN->AI->setAlignment(Align(AlignVal));

    Graph.push_back(std::move(RN));
  }

  unsigned Num = Graph.size() * 3;
  for (unsigned I = 0; I < Num; I++) {
    // std::shuffle(Graph.begin(), Graph.end(), std::default_random_engine());

    AllocaInst *Cur = IRB.CreateAlloca(TransST);
    auto Parent = std::make_unique<ReferenceNode>();
    Parent->AI = Cur;
    Parent->IsRaw = false;
    Parent->Id = Count++;

    size_t BN = Cryptoutils->getUint64T() % LaaBranches;
    std::vector<size_t> Random;
    getRandomNoRepeat(size_t(LaaBranches), BN, Random);

    for (unsigned J = 0; J < BN; J++) {
      unsigned Idx = Random[J];
      ReferenceNode *RN = Graph[Cryptoutils->getUint64T() % Graph.size()].get();
      Parent->Edges[Idx] = RN;

      IRB.CreateStore(
          RN->AI,
          IRB.CreateGEP(TransST, Cur, {IRB.getInt32(0), IRB.getInt32(Idx)}));

      // printf("s%d -> s%d at %d\n", Parent->Id, RN->Id, Idx);
      if (RN->IsRaw) {
        for (auto Iter = RN->RawInsts.begin(); Iter != RN->RawInsts.end(); Iter++) {
          AllocaInst *AI = Iter->first;
          Parent->Path[AI].push_back(Idx);
        }
      } else {
        for (auto Iter = RN->Path.begin(); Iter != RN->Path.end(); Iter++)
          Parent->Path[Iter->first].push_back(Idx);
      }
    }

    Graph.push_back(std::move(Parent));
  }

  // printf("---------------------------------\n");
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      for (Use &U : I.operands()) {
        Value *Opnd = U.get();
        if (std::find(AIs.begin(), AIs.end(), Opnd) == AIs.end())
          continue;

        AllocaInst *AI = (AllocaInst *)Opnd;
        IRB.SetInsertPoint(&I);

        uint64_t Seed = Cryptoutils->getUint64T();
        std::default_random_engine Rnd(Seed);
        std::shuffle(Graph.begin(), Graph.end(), Rnd);

        ReferenceNode *Ptr = nullptr;
        for (auto &RN : Graph) {
          if (RN->Path.find(AI) != RN->Path.end() ||
              (RN->IsRaw && RN->RawInsts.find(AI) != RN->RawInsts.end())) {
            Ptr = RN.get();
            break;
          }
        }

        assert(Ptr != nullptr);
        Value *VP = Ptr->AI;
        while (!Ptr->IsRaw) {
          std::vector<unsigned> &Idxs = Ptr->Path[AI];
          unsigned Idx = Idxs[Cryptoutils->getUint64T() % Idxs.size()];

          if (Getter.find(Idx) == Getter.end()) {
            Function *G = buildGetterFunction(*F.getParent(), TransST, Idx);
            Getter[Idx] = G;
          }

          VP = IRB.CreateLoad(
              PtrType, IRB.CreateCall(FunctionCallee(Getter[Idx]), {VP}));

          // printf("(s%d, %d) -> ", Ptr->Id, Idx);
          // VP = IRB.CreateLoad(
          //   PtrType,
          //    IRB.CreateGEP(TransST, VP, {IRB.getInt32(0),
          //    IRB.getInt32(Idx)}));

          Ptr = Ptr->Edges[Idx];
        }

        // printf("s%d\n", Ptr->Id);

        assert(Ptr->RawInsts.find(AI) != Ptr->RawInsts.end());

        ElementPos &EP = Ptr->RawInsts[AI];

        VP = IRB.CreateGEP(
            EP.Type,
            VP,
            {IRB.getInt32(0), IRB.getInt32(EP.Index)});

        U.set(VP);
      }
    }
  }

  for (AllocaInst *AI : AIs)
    AI->eraseFromParent();
}

template<typename T>
static void getRandomNoRepeat(T UpperBound, T Size, std::vector<T> &Result) {
  static_assert(std::is_integral_v<T>);
  assert(UpperBound >= Size);

  std::vector<T> List;
  for (T I = 0; I < UpperBound; I++)
    List.push_back(I);

  uint32_t Seed = Cryptoutils->getUint32T();
  std::shuffle(List.begin(), List.end(), std::default_random_engine(Seed));

  for (T I = 0; I < Size; I++)
    Result.push_back(List[I]);
}
