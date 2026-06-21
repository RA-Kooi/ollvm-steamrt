/*
LLVM Indirect Branching Pass
Copyright (C) 2017 Zhang(https://github.com/Naville/)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "llvm/Transforms/Obfuscation/IndirectBranch.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <random>

using namespace llvm;

static cl::opt<bool> IbrEnabled("ibr", cl::init(false),
                                cl::desc("Indirect Branch"));

PreservedAnalyses IndirectBranchPass::run(Function &F,
                                          FunctionAnalysisManager &AM) {
  if (!shouldObfuscate(IbrEnabled, &F, "ibr"))
    return PreservedAnalyses::all();

  if (F.empty() || F.hasLinkOnceLinkage() ||
      F.getSection() == ".text.startup") {
    return PreservedAnalyses::all();
  }

  LLVMContext &Ctx = F.getContext();

  // Init member fields
  BBNumbering.clear();
  BBTargets.clear();

  // llvm cannot split critical edge from IndirectBrInst
  SplitAllCriticalEdges(F, CriticalEdgeSplittingOptions(nullptr, nullptr));
  numberBasicBlock(F);

  if (BBNumbering.empty())
    return PreservedAnalyses::all();

  uint64_t V = Cryptoutils->getUint64T();
  IntegerType *IntType = Type::getInt32Ty(Ctx);

  unsigned PointerSize = F.getParent()->getDataLayout().getPointerSize();

  if (PointerSize == 8)
    IntType = Type::getInt64Ty(Ctx);

  ConstantInt *EncKey = ConstantInt::get(IntType, V, false);

  Value *MySecret = ConstantInt::get(IntType, 0, true);

  ConstantInt *Zero = ConstantInt::get(IntType, 0);
  GlobalVariable *DestBBs = getIndirectTargets(F, EncKey);

  for (BasicBlock &BB : F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (!(BI && BI->isConditional()))
      continue;

    IRBuilder<> IRB(BI);

    Value *Cond = BI->getCondition();
    Value *Idx;
    Value *TIdx, *FIdx;

    TIdx = ConstantInt::get(IntType, BBNumbering[BI->getSuccessor(0)]);
    FIdx = ConstantInt::get(IntType, BBNumbering[BI->getSuccessor(1)]);
    Idx = IRB.CreateSelect(Cond, TIdx, FIdx);

    Value *GEP = IRB.CreateGEP(DestBBs->getValueType(), DestBBs, {Zero, Idx});
    Value *EncDestAddr = IRB.CreateLoad(GEP->getType(), GEP, "EncDestAddr");

    // Use IPO context to compute the encryption key
    // X = FuncSecret - EncKey
    Constant *X = ConstantExpr::getSub(Zero, EncKey);

    // -EncKey = X - FuncSecret
    Value *DecKey = IRB.CreateAdd(X, MySecret);
    Value *DestAddr =
        IRB.CreateGEP(PointerType::getUnqual(Ctx), EncDestAddr, DecKey);

    IndirectBrInst *IBI = IndirectBrInst::Create(DestAddr, 2);
    IBI->addDestination(BI->getSuccessor(0));
    IBI->addDestination(BI->getSuccessor(1));
    ReplaceInstWithInst(BI, IBI);
  }

  return PreservedAnalyses::none();
}

void IndirectBranchPass::numberBasicBlock(Function &F) {
  for (BasicBlock &BB : F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (!BI)
      continue;

    if (!BI->isConditional())
      continue;

    unsigned N = BI->getNumSuccessors();
    for (unsigned I = 0; I < N; I++) {
      BasicBlock *Succ = BI->getSuccessor(I);
      if (BBNumbering.count(Succ) != 0)
        continue;

      BBTargets.push_back(Succ);
      BBNumbering[Succ] = 0;
    }
  }

  long Seed = Cryptoutils->getUint32T();
  std::default_random_engine E(Seed);
  std::shuffle(BBTargets.begin(), BBTargets.end(), E);

  unsigned N = 0;
  for (auto *BB : BBTargets)
    BBNumbering[BB] = N++;
}

GlobalVariable *IndirectBranchPass::getIndirectTargets(Function &F,
                                                       ConstantInt *EncKey) {
  std::string GVName(F.getName().str() + "_IndirectBrTargets");
  GlobalVariable *GV = F.getParent()->getNamedGlobal(GVName);
  if (GV)
    return GV;

  // encrypt branch targets
  // clang-format off
  std::vector<Constant *> Elements;
  for (auto *const BB : BBTargets) {
    Constant *CE = ConstantExpr::getBitCast(
        BlockAddress::get(BB),
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
