#include "llvm/Transforms/Obfuscation/Flattening.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Scalar/Reg2Mem.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#define DEBUG_TYPE "flattening"

using namespace llvm;

STATISTIC(Flattened, "Functions flattened");

static cl::opt<bool> FlaEnabled("fla", cl::init(false), cl::desc("Flattening"));

static bool flatten(Function &F, FunctionAnalysisManager &AM);

PreservedAnalyses FlatteningPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  if (shouldObfuscate(FlaEnabled, &F, "fla")) {
    if (flatten(F, AM))
      ++Flattened;

    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

static bool flatten(Function &F, FunctionAnalysisManager &AM) {
  LLVMContext &Ctx = F.getContext();

  IntegerType *IntType = Type::getInt32Ty(Ctx);

  // Transforms all switches in the function to a list of branches.
  LowerSwitchPass SwitchPass;
  SwitchPass.run(F, AM);

  // Demote all registers to the stack.
  RegToMemPass R2MPass;
  R2MPass.run(F, AM);

  auto IsExtendedLandingPad = [](BasicBlock &BB) -> bool {
    for (PHINode &Phi : BB.phis()) {
      for (auto &Inc : Phi.incoming_values()) {
        auto *I = dyn_cast_or_null<Instruction>(&Inc);
        if(!I)
          continue;

        if (I->getParent()->isLandingPad())
          return true;
      }
    }

    return false;
  };

  SmallVector<BasicBlock *, 8> OrigBBs;
  for (BasicBlock &BB : F)
  {
    // Landing pads and the resume function cannot be part of the flattening.
    // NOTE(Rafaël): I don't remember the exact reason, but llc was complaining
    // about it when they were. Something about only specific origins being
    // allowed or something.
    if (BB.isLandingPad() || isa<ResumeInst>(BB.getTerminator()))
      continue;

    // It's possible that a landing pad is split and deduplicated giving a tree
    // like this:
    // ┌------┐ ┌-------┐
    // | lpad | | lpad2 |
    // └---┬--┘ └---┬---┘
    //     |        |
    //     └----┬---┘
    //          |
    //  ┌-------┴-----┐
    //  | shared_lpad |
    //  └-------------┘
    //  The shared landing pad also cannot be part of the flattening, so we
    //  skip it.
    if (IsExtendedLandingPad(BB))
      continue;

    OrigBBs.emplace_back(&BB);
  }

  // Nothing to flatten
  if (OrigBBs.size() <= 1)
    return false;

  // Get a pointer to the first BB
  BasicBlock *Insert = &*F.begin();

  // Remove first BB
  OrigBBs.erase(OrigBBs.begin());

  // If main begins with a branch
  Instruction *Term = Insert->getTerminator();

  bool Branches = isa<BranchInst>(Term)
    || isa<InvokeInst>(Term)
    || isa<ResumeInst>(Term)
    || isa<CallBrInst>(Term);

  if (Branches) {
    auto It = Insert->end();
    --It;

    if (Insert->size() > 1)
      --It;

    BasicBlock *TmpBb = Insert->splitBasicBlock(It, "first");
    OrigBBs.insert(OrigBBs.begin(), TmpBb);
  }

  // Remove jump
  Insert->getTerminator()->eraseFromParent();

  IRBuilder<> IRB(Insert);

  // Create switch variable and set as it
  char ScramblingKey[16];
  Cryptoutils->getBytes(ScramblingKey, 16);

  uint32_t Scram = Cryptoutils->scramble32(0, ScramblingKey);

  ConstantInt *ScramInit = IRB.getInt32(Scram);

  // Create main loop
  BasicBlock *LoopEntry = BasicBlock::Create(Ctx, "loopEntry", &F, Insert);

  // Move first BB on top and jump to while loop
  Insert->moveBefore(LoopEntry);
  BranchInst::Create(LoopEntry, Insert);

  // default case jump to LoopEntry
  BasicBlock *SwDefault = BasicBlock::Create(Ctx, "switchDefault", &F, LoopEntry);
  BranchInst::Create(LoopEntry, SwDefault);

  IRB.SetInsertPoint(LoopEntry);
  PHINode *CurSw = IRB.CreatePHI(IntType, OrigBBs.size() + 2);
  CurSw->addIncoming(ScramInit, Insert);
  // NOTE(Rafaël): This looks weird, but if you set the incoming to its initial
  // value it won't change the value. Functionally identical to a = a.
  CurSw->addIncoming(ScramInit, SwDefault);

  // Create switch instruction itself and set condition
  SwitchInst *SwitchI = IRB.CreateSwitch(CurSw, SwDefault, OrigBBs.size());

  // Put all BBs except the first in the switch
  for (BasicBlock *I : OrigBBs) {
    // Move the BB inside the switch (only visual, no code logic)
    I->moveBefore(LoopEntry);

    // Add case to switch
    Scram = Cryptoutils->scramble32(SwitchI->getNumCases(), ScramblingKey);
    ConstantInt *NumCase = ConstantInt::get(IntType, Scram);

    SwitchI->addCase(NumCase, I);
  }

  // Recalculate switchVar
  for (BasicBlock *I : OrigBBs) {
    IRB.SetInsertPoint(I);

    // If it's a non-conditional jump
    if (I->getTerminator()->getNumSuccessors() == 1) {
      // Get successor
      BasicBlock *Succ = I->getTerminator()->getSuccessor(0);

      // If the block jumped to is part of exception handling, don't replace
      // the jump. Since the resume block cannot be part of the while loop.
      if (isa<ResumeInst>(Succ->getTerminator()))
        continue;

      // and delete terminator
      I->getTerminator()->eraseFromParent();

      // Get next case
      ConstantInt *NumCase = SwitchI->findCaseDest(Succ);

      // If next case == default case (switchDefault)
      if (!NumCase) {
        int MyCase = SwitchI->getNumCases() - 1;
        Scram = Cryptoutils->scramble32(MyCase, ScramblingKey);
        NumCase = ConstantInt::get(IntType, Scram);
      }

      // Update switchVar and jump to the end of loop
      CurSw->addIncoming(NumCase, I);
      IRB.CreateBr(LoopEntry);

      continue;
    }

    // If it's a conditional jump
    Instruction *InstTerm = I->getTerminator();
    if (InstTerm->getNumSuccessors() == 2) {
      // Get next cases
      BasicBlock *SuccT = InstTerm->getSuccessor(0);
      BasicBlock *SuccF = InstTerm->getSuccessor(1);

      ConstantInt *NumCaseTrue = SwitchI->findCaseDest(SuccT);
      ConstantInt *NumCaseFalse = SwitchI->findCaseDest(SuccF);

      Scram = Cryptoutils->scramble32(SwitchI->getNumCases() - 1, ScramblingKey);

      // Check if next case == default case (switchDefault)
      if (!NumCaseTrue)
        NumCaseTrue = ConstantInt::get(IntType, Scram);

      if (!NumCaseFalse)
        NumCaseFalse = ConstantInt::get(IntType, Scram);

      if (!isa<BranchInst>(InstTerm) && !isa<InvokeInst>(InstTerm)) {
        errs() << "[Control flow flattening]: Terminator is not a branch "
          "instruction!\n";

        InstTerm->print(errs());
        errs() << "\n";

        continue;
      }

      // If the terminator is an invoke instruction, change the jump to the
      // continue handler to the while loop instead.
      if (isa<InvokeInst>(InstTerm)) {
        ConstantInt *Sel = SuccT->isLandingPad() ? NumCaseFalse : NumCaseTrue;
        CurSw->addIncoming(Sel, I);

        InstTerm->setSuccessor(SuccT->isLandingPad() ? 1 : 0, LoopEntry);

        continue;
      }

      bool ResumeIsTrue = isa<ResumeInst>(SuccT->getTerminator());
      bool ResumeIsFalse = isa<ResumeInst>(SuccF->getTerminator());

      BranchInst *Br = cast<BranchInst>(InstTerm);

      // Create a SelectInst
      Value *Sel = IRB.CreateSelect(Br->getCondition(), NumCaseTrue, NumCaseFalse);

      // Update switchVar
      CurSw->addIncoming(Sel, I);

      if (ResumeIsTrue || ResumeIsFalse) {
        IRB.CreateCondBr(Br->getCondition(),
                         ResumeIsTrue ? SuccT : LoopEntry,
                         ResumeIsFalse ? SuccF : LoopEntry);
      } else
        IRB.CreateBr(LoopEntry);

      // Erase terminator
      InstTerm->eraseFromParent();

      continue;
    }

    if (InstTerm->getNumSuccessors() > 2) {
      errs() << "[Control flow flattening]: Unhandled BasicBlock: "
        "successors > 2\n";
    }
  }

  // Recalculate the dominator tree to promote our demotions back to registers.
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  DT.recalculate(F);

  // Promote what we can to registers.
  PromotePass PP;
  PP.run(F, AM);

  return true;
}
