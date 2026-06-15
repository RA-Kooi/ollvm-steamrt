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
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

static cl::opt<bool> IgvEnabled("igv", cl::init(false),
                                cl::desc("Indirect Global Variable"));

PreservedAnalyses IndirectGlobalVariablePass::run(Module &M,
                                                  ModuleAnalysisManager &AM) {

  for (Function &Fn : M) {
    if (!shouldObfuscate(IgvEnabled, &Fn, "igv")) {
      continue;
    }

    if (Options && Options->skipFunction(Fn.getName())) {
      continue;
    }

    LLVMContext &Ctx = Fn.getContext();

    GVNumbering.clear();
    GlobalVariables.clear();

    lowerConstantExpr(Fn);
    numberGlobalVariable(Fn);

    if (GlobalVariables.empty()) {
      continue;
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
    GlobalVariable *GVars = getIndirectGlobalVariables(Fn, EncKey1);

    for (inst_iterator I = inst_begin(Fn), E = inst_end(Fn); I != E; ++I) {
      Instruction *Inst = &*I;
      if (isa<LandingPadInst>(Inst) || isa<CleanupPadInst>(Inst) ||
          isa<CatchPadInst>(Inst) || isa<CatchReturnInst>(Inst) ||
          isa<CatchSwitchInst>(Inst) || isa<ResumeInst>(Inst) ||
          isa<CallInst>(Inst)) {
        continue;
      }
      if (PHINode *PHI = dyn_cast<PHINode>(Inst)) {
        for (unsigned int I = 0; I < PHI->getNumIncomingValues(); ++I) {
          Value *Val = PHI->getIncomingValue(I);
          if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Val)) {
            if (GVNumbering.count(GV) == 0) {
              continue;
            }

            Instruction *IP = PHI->getIncomingBlock(I)->getTerminator();
            IRBuilder<> IRB(IP);

            Value *Idx = ConstantInt::get(IntType, GVNumbering[GV]);
            Value *GEP =
                IRB.CreateGEP(GVars->getValueType(), GVars, {Zero, Idx});
            LoadInst *EncGVAddr =
                IRB.CreateLoad(GEP->getType(), GEP, GV->getName());

            Value *Secret = IRB.CreateAdd(EncKey, MySecret);
            Value *GVAddr =
                IRB.CreateGEP(Type::getInt8Ty(Ctx), EncGVAddr, Secret);
            GVAddr = IRB.CreateBitCast(GVAddr, GV->getType());
            GVAddr->setName("IndGV0_");
            PHI->setIncomingValue(I, GVAddr);
          }
        }
      } else {
        for (User::op_iterator Op = Inst->op_begin(); Op != Inst->op_end();
             ++Op) {
          if (GlobalVariable *GV = dyn_cast<GlobalVariable>(*Op)) {
            if (GVNumbering.count(GV) == 0) {
              continue;
            }

            IRBuilder<> IRB(Inst);
            Value *Idx = ConstantInt::get(IntType, GVNumbering[GV]);
            Value *GEP =
                IRB.CreateGEP(GVars->getValueType(), GVars, {Zero, Idx});
            LoadInst *EncGVAddr =
                IRB.CreateLoad(GEP->getType(), GEP, GV->getName());

            Value *Secret = IRB.CreateAdd(EncKey, MySecret);
            Value *GVAddr =
                IRB.CreateGEP(Type::getInt8Ty(Ctx), EncGVAddr, Secret);
            GVAddr = IRB.CreateBitCast(GVAddr, GV->getType());
            GVAddr->setName("IndGV1_");
            Inst->replaceUsesOfWith(GV, GVAddr);
          }
        }
      }
    }
  }
  return PreservedAnalyses::none();
}

void IndirectGlobalVariablePass::numberGlobalVariable(Function &F) {
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    for (User::op_iterator Op = (*I).op_begin(); Op != (*I).op_end(); ++Op) {
      Value *Val = *Op;
      if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Val)) {
        if (!GV->isThreadLocal() && GVNumbering.count(GV) == 0 &&
            !GV->isDLLImportDependent()) {
          GVNumbering[GV] = GlobalVariables.size();
          GlobalVariables.push_back((GlobalVariable *)Val);
        }
      }
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

  std::vector<Constant *> Elements;
  for (GlobalVariable *GVar : GlobalVariables) {
    Constant *CE = ConstantExpr::getBitCast(
        GVar, llvm::PointerType::get(Type::getInt8Ty(F.getContext()), 0));
    CE = ConstantExpr::getGetElementPtr(Type::getInt8Ty(F.getContext()), CE,
                                        EncKey);
    Elements.push_back(CE);
  }

  ArrayType *ATy =
      ArrayType::get(llvm::PointerType::get(Type::getInt8Ty(F.getContext()), 0),
                     Elements.size());
  Constant *CA = ConstantArray::get(ATy, ArrayRef<Constant *>(Elements));
  GV =
      new GlobalVariable(*F.getParent(), ATy, false,
                         GlobalValue::LinkageTypes::PrivateLinkage, CA, GVName);
  appendToCompilerUsed(*F.getParent(), {GV});
  return GV;
}
