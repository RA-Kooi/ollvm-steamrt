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
#include "llvm/Transforms/Obfuscation/IndirectGlobalVariable.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

static cl::opt<bool> IgvEnabled("igv", cl::init(false),
                                cl::desc("Indirect Global Variable"));

PreservedAnalyses IndirectGlobalVariablePass::run(Function &F,
                                                  FunctionAnalysisManager &AM) {
  if (!shouldObfuscate(IgvEnabled, &F, "igv")) {
    return PreservedAnalyses::all();
  }

  LLVMContext &Ctx = F.getContext();

  GVNumbering.clear();
  GlobalVariables.clear();

  lowerConstantExpr(F);
  numberGlobalVariable(F);

  if (GlobalVariables.empty())
    return PreservedAnalyses::all();

  uint64_t V = Cryptoutils->getUint64T();
  IntegerType *IntType = Type::getInt32Ty(Ctx);

  unsigned PointerSize = F.getParent()->getDataLayout().getPointerSize();

  if (PointerSize == 8)
    IntType = Type::getInt64Ty(Ctx);

  ConstantInt *EncKey = ConstantInt::get(IntType, V, false);

  Value *MySecret = ConstantInt::get(IntType, 0, true);

  ConstantInt *Zero = ConstantInt::get(IntType, 0);
  GlobalVariable *GVars = getIndirectGlobalVariables(F, EncKey);

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    Instruction *Inst = &*I;
    if (isa<LandingPadInst>(Inst) || isa<CleanupPadInst>(Inst) ||
        isa<CatchPadInst>(Inst) || isa<CatchReturnInst>(Inst) ||
        isa<CatchSwitchInst>(Inst) || isa<ResumeInst>(Inst) ||
        isa<CallInst>(Inst)) {
      continue;
    }

    // clang-format off
    auto BuildSharedIR =
    [
      this,
      &IntType,
      &Zero,
      &GVars,
      &EncKey,
      &MySecret,
      &Ctx
    ](GlobalVariable *GV, IRBuilder<> &IRB) -> Value * {
      // clang-format on
      Value *Idx = ConstantInt::get(IntType, GVNumbering[GV]);
      Value *GEP = IRB.CreateGEP(GVars->getValueType(), GVars, {Zero, Idx});
      LoadInst *EncGVAddr = IRB.CreateLoad(GEP->getType(), GEP, GV->getName());

      Constant *X = ConstantExpr::getSub(Zero, EncKey);
      Value *Secret = IRB.CreateAdd(X, MySecret);

      return IRB.CreateGEP(PointerType::getUnqual(Ctx), EncGVAddr, Secret);
    };

    if (PHINode *PHI = dyn_cast<PHINode>(Inst)) {
      for (unsigned int I = 0; I < PHI->getNumIncomingValues(); ++I) {
        Value *Val = PHI->getIncomingValue(I);
        GlobalVariable *GV = dyn_cast<GlobalVariable>(Val);
        if (!GV)
          continue;

        if (GVNumbering.count(GV) == 0)
          continue;

        Instruction *IP = PHI->getIncomingBlock(I)->getTerminator();
        IRBuilder<> IRB(IP);

        Value *GVAddr = BuildSharedIR(GV, IRB);
        GVAddr = IRB.CreateBitCast(GVAddr, GV->getType());
        GVAddr->setName("IndGV0_");
        PHI->setIncomingValue(I, GVAddr);
      }

      return PreservedAnalyses::none();
    }

    for (User::op_iterator Op = Inst->op_begin(); Op != Inst->op_end(); ++Op) {
      GlobalVariable *GV = dyn_cast<GlobalVariable>(*Op);
      if (!GV)
        continue;

      if (GVNumbering.count(GV) == 0)
        continue;

      IRBuilder<> IRB(Inst);

      Value *GVAddr = BuildSharedIR(GV, IRB);
      GVAddr = IRB.CreateBitCast(GVAddr, GV->getType());
      GVAddr->setName("IndGV1_");
      Inst->replaceUsesOfWith(GV, GVAddr);
    }
  }

  return PreservedAnalyses::none();
}

void IndirectGlobalVariablePass::numberGlobalVariable(Function &F) {
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    for (User::op_iterator Op = (*I).op_begin(); Op != (*I).op_end(); ++Op) {
      Value *Val = *Op;
      GlobalVariable *GV = dyn_cast<GlobalVariable>(Val);

      if (!GV)
        continue;

      if (GV->isThreadLocal() || GVNumbering.count(GV) > 0 ||
          GV->isDLLImportDependent())
        continue;

      GVNumbering[GV] = GlobalVariables.size();
      GlobalVariables.push_back((GlobalVariable *)Val);
    }
  }
}

GlobalVariable *
IndirectGlobalVariablePass::getIndirectGlobalVariables(Function &F,
                                                       ConstantInt *EncKey) {
  std::string GVName(F.getName().str() + "_IndirectGVars");
  GlobalVariable *GV = F.getParent()->getNamedGlobal(GVName);
  if (GV)
    return GV;

  // clang-format off
  std::vector<Constant *> Elements;
  for (GlobalVariable *GVar : GlobalVariables) {
    Constant *CE = ConstantExpr::getBitCast(
        GVar,
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
