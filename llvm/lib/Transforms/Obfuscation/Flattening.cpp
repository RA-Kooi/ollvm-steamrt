#include "llvm/Transforms/Obfuscation/Flattening.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
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
    if (flatten(&F, AM)) {
      ++Flattened;
    }

    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

static bool flatten(Function *F, FunctionAnalysisManager &AM) {
  const DataLayout &DL = F->getParent()->getDataLayout();
  LLVMContext &Ctx = F->getContext();

  IntegerType *IntType = Type::getInt32Ty(Ctx);

  // Transforms all switches in the function to a list of branches.
  LowerSwitchPass SwitchPass;
  SwitchPass.run(*F, AM);

  SmallVector<BasicBlock *, 8> OrigBBs;
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

    OrigBBs.emplace_back(&BB);
  }

  // Nothing to flatten
  if (OrigBBs.size() <= 1)
    return false;

  // Remove first BB
  OrigBBs.erase(OrigBBs.begin());

  // Get a pointer on the first BB
  BasicBlock *Insert = &*F->begin();

  // If main begin with an if
  if (isa<BranchInst>(Insert->getTerminator())) {
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

  // TODO(Rafaël): Use PHI nodes instead
  // Create switch variable and set as it
  AllocaInst *SwitchVar = IRB.CreateAlloca(IntType, DL.getAllocaAddrSpace());

  AllocaInst *SwitchVarAddr = IRB.CreateAlloca(PointerType::getUnqual(Ctx),
                                               DL.getAllocaAddrSpace());

  Type *SwitchVarAddrTy = SwitchVarAddr->getAllocatedType();

  char ScramblingKey[16];
  Cryptoutils->getBytes(ScramblingKey, 16);

  uint32_t Scram = Cryptoutils->scramble32(0, ScramblingKey);

  IRB.CreateStore(ConstantInt::get(IntType, Scram), SwitchVar);
  IRB.CreateStore(SwitchVar, SwitchVarAddr);

  // Create main loop
  BasicBlock *LoopEntry = BasicBlock::Create(Ctx, "loopEntry", F, Insert);
  BasicBlock *LoopEnd = BasicBlock::Create(Ctx, "loopEnd", F, Insert);

  // Move first BB on top and jump to while loop
  Insert->moveBefore(LoopEntry);
  BranchInst::Create(LoopEntry, Insert);

  // loopEnd jump to loopEntry
  BranchInst::Create(LoopEntry, LoopEnd);

  BasicBlock *SwDefault = BasicBlock::Create(Ctx, "switchDefault", F, LoopEnd);
  BranchInst::Create(LoopEnd, SwDefault);

  IRB.SetInsertPoint(LoopEntry);
  Value *Load = IRB.CreateLoad(IntType, SwitchVar);

  // Create switch instruction itself and set condition
  SwitchInst *SwitchI = IRB.CreateSwitch(Load, SwDefault, OrigBBs.size());

  // Put all BBs except the first in the switch
  for (BasicBlock *I : OrigBBs) {
    // Move the BB inside the switch (only visual, no code logic)
    I->moveBefore(LoopEnd);

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
      // Get successor and delete terminator
      BasicBlock *Succ = I->getTerminator()->getSuccessor(0);
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
      IRB.CreateStore(NumCase, IRB.CreateLoad(SwitchVarAddrTy, SwitchVarAddr));
      IRB.CreateBr(LoopEnd);

      continue;
    }

    // If it's a conditional jump
    Instruction *InstTerm = I->getTerminator();
    if (InstTerm->getNumSuccessors() == 2) {
      // Get next cases
      ConstantInt *NumCaseTrue = SwitchI->findCaseDest(InstTerm->getSuccessor(0));
      ConstantInt *NumCaseFalse = SwitchI->findCaseDest(InstTerm->getSuccessor(1));

      Scram = Cryptoutils->scramble32(SwitchI->getNumCases() - 1, ScramblingKey);

      // Check if next case == default case (switchDefault)
      if (!NumCaseTrue)
        NumCaseTrue = ConstantInt::get(IntType, Scram);

      if (!NumCaseFalse)
        NumCaseFalse = ConstantInt::get(IntType, Scram);

      // Create a SelectInst
      if (!isa<BranchInst>(InstTerm)) {
        errs() << "[Control flow flattening]: Terminator is not a branch "
          "instruction!\n";

        std::abort();
      }

      BranchInst *Br = cast<BranchInst>(InstTerm);
      Value *Sel = IRB.CreateSelect(Br->getCondition(), NumCaseTrue, NumCaseFalse);

      // Erase terminator
      InstTerm->eraseFromParent();

      // Update switchVar and jump to the end of loop
      IRB.CreateStore(Sel, IRB.CreateLoad(SwitchVarAddrTy, SwitchVarAddr));
      IRB.CreateBr(LoopEnd);

      continue;
    }

    if (InstTerm->getNumSuccessors() > 2)
      errs() << "Unhandled BasicBlock: successors > 2\n";
  }

  RegToMemPass::runPass(*F);

  return true;
}
