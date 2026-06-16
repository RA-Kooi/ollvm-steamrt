#include "llvm/Transforms/Obfuscation/Flattening.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Scalar/Reg2Mem.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"

#define DEBUG_TYPE "flattening"

using namespace llvm;

STATISTIC(Flattened, "Functions flattened");

static cl::opt<bool> FlaEnabled("fla", cl::init(false), cl::desc("Flattening"));

static bool flatten(Function *F, FunctionAnalysisManager &AM);

PreservedAnalyses FlatteningPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  if (shouldObfuscate(FlaEnabled, &F, "fla")) {
    INIT_CONTEXT(F);

    if (flatten(&F, AM)) {
      ++Flattened;
    }

    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

static bool flatten(Function *F, FunctionAnalysisManager &AM) {
  SmallVector<BasicBlock *, 8> OrigBb;
  BasicBlock *LoopEntry, *LoopEnd;
  LoadInst *Load;
  SwitchInst *SwitchI;
  AllocaInst *SwitchVar, *SwitchVarAddr;
  const DataLayout &DL = F->getParent()->getDataLayout();

  std::unordered_map<uint32_t, uint32_t> ScramblingKey;

  LowerSwitchPass SwitchPass;
  SwitchPass.run(*F, AM);

  for (BasicBlock &BB : *F) {
    if (BB.isEHPad() || BB.isLandingPad()) {
      errs() << F->getName()
             << " Contains Exception Handing Instructions and is unsupported "
                "for flattening in the open-source version of Hikari.\n";

      return false;
    }
    if (!isa<BranchInst>(BB.getTerminator()) &&
        !isa<ReturnInst>(BB.getTerminator()))
      return false;
    OrigBb.emplace_back(&BB);
  }

  // Nothing to flatten
  if (OrigBb.size() <= 1)
    return false;

  // Remove first BB
  OrigBb.erase(OrigBb.begin());

  // Get a pointer on the first BB
  Function::iterator Tmp = F->begin();
  BasicBlock *Insert = &*Tmp;

  // If main begin with an if
  BranchInst *Br = nullptr;
  if (isa<BranchInst>(Insert->getTerminator()))
    Br = cast<BranchInst>(Insert->getTerminator());

  if ((Br && Br->isConditional()) ||
      Insert->getTerminator()->getNumSuccessors() > 1) {
    BasicBlock::iterator I = Insert->end();
    --I;

    if (Insert->size() > 1) {
      --I;
    }

    BasicBlock *TmpBb = Insert->splitBasicBlock(I, "first");
    OrigBb.insert(OrigBb.begin(), TmpBb);
  }

  // Remove jump
  Instruction *OldTerm = Insert->getTerminator();

  // Create switch variable and set as it
  SwitchVar = new AllocaInst(Type::getInt32Ty(F->getContext()),
                             DL.getAllocaAddrSpace(), "switchVar", OldTerm);
  SwitchVarAddr =
      new AllocaInst(Type::getInt32Ty(F->getContext())->getPointerTo(),
                     DL.getAllocaAddrSpace(), "", OldTerm);

  // Remove jump
  OldTerm->eraseFromParent();

  new StoreInst(ConstantInt::get(Type::getInt32Ty(F->getContext()),
                                 Cryptoutils->scramble32(0, ScramblingKey)),
                SwitchVar, Insert);
  new StoreInst(SwitchVar, SwitchVarAddr, Insert);

  // Create main loop
  LoopEntry = BasicBlock::Create(F->getContext(), "loopEntry", F, Insert);
  LoopEnd = BasicBlock::Create(F->getContext(), "loopEnd", F, Insert);

  Load = new LoadInst(SwitchVar->getAllocatedType(), SwitchVar, "switchVar",
                      LoopEntry);

  // Move first BB on top
  Insert->moveBefore(LoopEntry);
  BranchInst::Create(LoopEntry, Insert);

  // loopEnd jump to loopEntry
  BranchInst::Create(LoopEntry, LoopEnd);

  BasicBlock *SwDefault =
      BasicBlock::Create(F->getContext(), "switchDefault", F, LoopEnd);
  BranchInst::Create(LoopEnd, SwDefault);

  // Create switch instruction itself and set condition
  SwitchI = SwitchInst::Create(&*F->begin(), SwDefault, 0, LoopEntry);
  SwitchI->setCondition(Load);

  // Remove branch jump from 1st BB and make a jump to the while
  F->begin()->getTerminator()->eraseFromParent();

  BranchInst::Create(LoopEntry, &*F->begin());

  // Put BB in the switch
  for (BasicBlock *I : OrigBb) {
    ConstantInt *NumCase = nullptr;

    // Move the BB inside the switch (only visual, no code logic)
    I->moveBefore(LoopEnd);

    // Add case to switch
    NumCase = cast<ConstantInt>(ConstantInt::get(
        SwitchI->getCondition()->getType(),
        Cryptoutils->scramble32(SwitchI->getNumCases(), ScramblingKey)));
    SwitchI->addCase(NumCase, I);
  }

  // Recalculate switchVar
  for (BasicBlock *I : OrigBb) {
    ConstantInt *NumCase = nullptr;

    // If it's a non-conditional jump
    if (I->getTerminator()->getNumSuccessors() == 1) {
      // Get successor and delete terminator
      BasicBlock *Succ = I->getTerminator()->getSuccessor(0);
      I->getTerminator()->eraseFromParent();

      // Get next case
      NumCase = SwitchI->findCaseDest(Succ);

      // If next case == default case (switchDefault)
      if (!NumCase) {
        NumCase = cast<ConstantInt>(
            ConstantInt::get(SwitchI->getCondition()->getType(),
                             Cryptoutils->scramble32(SwitchI->getNumCases() - 1,
                                                     ScramblingKey)));
      }

      // Update switchVar and jump to the end of loop
      new StoreInst(
          NumCase,
          new LoadInst(SwitchVarAddr->getAllocatedType(), SwitchVarAddr, "", I),
          I);
      BranchInst::Create(LoopEnd, I);
      continue;
    }

    // If it's a conditional jump
    if (I->getTerminator()->getNumSuccessors() == 2) {
      // Get next cases
      ConstantInt *NumCaseTrue =
          SwitchI->findCaseDest(I->getTerminator()->getSuccessor(0));
      ConstantInt *NumCaseFalse =
          SwitchI->findCaseDest(I->getTerminator()->getSuccessor(1));

      // Check if next case == default case (switchDefault)
      if (!NumCaseTrue) {
        NumCaseTrue = cast<ConstantInt>(
            ConstantInt::get(SwitchI->getCondition()->getType(),
                             Cryptoutils->scramble32(SwitchI->getNumCases() - 1,
                                                     ScramblingKey)));
      }

      if (!NumCaseFalse) {
        NumCaseFalse = cast<ConstantInt>(
            ConstantInt::get(SwitchI->getCondition()->getType(),
                             Cryptoutils->scramble32(SwitchI->getNumCases() - 1,
                                                     ScramblingKey)));
      }

      // Create a SelectInst
      BranchInst *Br = cast<BranchInst>(I->getTerminator());
      SelectInst *Sel =
          SelectInst::Create(Br->getCondition(), NumCaseTrue, NumCaseFalse, "",
                             I->getTerminator());

      // Erase terminator
      I->getTerminator()->eraseFromParent();
      // Update switchVar and jump to the end of loop
      new StoreInst(
          Sel,
          new LoadInst(SwitchVarAddr->getAllocatedType(), SwitchVarAddr, "", I),
          I);
      BranchInst::Create(LoopEnd, I);
      continue;
    }
  }

  RegToMemPass::runPass(*F);

  return true;
}
