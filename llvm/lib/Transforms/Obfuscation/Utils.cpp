// Based on the original OLLVM code:
// https://github.com/obfuscator-llvm/obfuscator

#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/NoFolder.h"

using namespace llvm;

LLVMContext *CONTEXT = nullptr;

std::string llvm::readAnnotate(Function *F) {
  std::string Annotation = "";

  /* Get annotation variable */
  GlobalVariable *Glob =
      F->getParent()->getGlobalVariable("llvm.global.annotations");

  if (Glob == NULL)
    return "";

  /* Get the array */
  if (ConstantArray *Ca = dyn_cast<ConstantArray>(Glob->getInitializer())) {
    for (unsigned I = 0; I < Ca->getNumOperands(); ++I) {
      /* Get the struct */
      ConstantStruct *StructAn = dyn_cast<ConstantStruct>(Ca->getOperand(I));
      if (!StructAn)
        continue;

      ConstantExpr *Expr = dyn_cast<ConstantExpr>(StructAn->getOperand(0));
      if (!Expr)
        continue;

      /*
       * If it's a bitcast we can check if the annotation is concerning
       * the current function
       */
      bool SelfBitCast =
          Expr->getOpcode() == Instruction::BitCast && Expr->getOperand(0) == F;
      if (!SelfBitCast)
        continue;

      ConstantExpr *Note = cast<ConstantExpr>(StructAn->getOperand(1));

      /*
       * If it's a GetElementPtr, that means we found the variable
       * containing the annotations
       */
      if (Note->getOpcode() != Instruction::GetElementPtr)
        continue;

      GlobalVariable *AnnoteStr = dyn_cast<GlobalVariable>(Note->getOperand(0));
      if (!AnnoteStr)
        continue;

      ConstantDataSequential *Data =
          dyn_cast<ConstantDataSequential>(AnnoteStr->getInitializer());
      if (!Data)
        continue;

      if (Data->isString())
        Annotation += Data->getAsString().lower() + " ";
    }
  }

  return (Annotation);
}

static std::string getFunctionAnnotation(Function *F) {
  Module *M = F->getParent();

  GlobalVariable *GA = M->getNamedGlobal("llvm.global.annotations");
  if (!GA)
    return "";

  ConstantArray *CA = dyn_cast<ConstantArray>(GA->getInitializer());
  if (!CA)
    return "";

  for (unsigned I = 0; I < CA->getNumOperands(); ++I) {
    ConstantStruct *CS = dyn_cast<ConstantStruct>(CA->getOperand(I));
    if (!CS)
      continue;

    Function *AnnotatedFunction =
        dyn_cast<Function>(CS->getOperand(0)->stripPointerCasts());
    if (!AnnotatedFunction)
      continue;

    if (AnnotatedFunction != F)
      continue;

    // The second element is a global variable for the annotation
    // string. NOTE(Dragoon): Whatever the fuck that may mean. The
    // original chinese comments are beyond useless. Wondering if this
    // is LLM slop.
    GlobalVariable *GV =
        dyn_cast<GlobalVariable>(CS->getOperand(1)->stripPointerCasts());
    if (!GV)
      continue;

    if (ConstantDataArray *Anno =
            dyn_cast<ConstantDataArray>(GV->getInitializer()))
      return Anno->getAsCString().str();
  }

  return "";
}

bool llvm::shouldObfuscate(bool Flag, Function *F,
                           std::string const &Attribute) {
  std::string Attr = Attribute;
  std::string AttrNo = "no" + Attr;

  if (F->isDeclaration()) {
    return false;
  }

  if (F->hasAvailableExternallyLinkage() != 0) {
    return false;
  }

  //  We have to check the nofla flag first
  //  Because .find("fla") is true for a string like "fla" or
  //  "nofla"
  if (getFunctionAnnotation(F).find(AttrNo) != std::string::npos) {
    return false;
  }

  // If fla annotations
  if (getFunctionAnnotation(F).find(Attr) != std::string::npos) {
    return true;
  }

  return Flag;
}

void llvm::fixFunctionConstantExpr(Function *Func) {
  // Replace ConstantExpr with equal instructions
  // Otherwise replacing on Constant will crash the compiler
  for (BasicBlock &BB : *Func) {
    fixBasicBlockConstantExpr(&BB);
  }
}

void llvm::fixBasicBlockConstantExpr(BasicBlock *BB) {
  // Replace ConstantExpr with equal instructions
  // Otherwise replacing on Constant will crash the compiler
  // Things to note:
  // - Phis must be placed at BB start so CEs must be placed prior to current BB
  assert(!BB->empty() && "BasicBlock is empty!");
  assert((BB->getParent() != NULL) && "BasicBlock must be in a Function!");
  Instruction *FunctionInsertPt =
      &*(BB->getParent()->getEntryBlock().getFirstInsertionPt());
  // Instruction* LocalBBInsertPt=&*(BB.getFirstInsertionPt());
  for (Instruction &I : *BB) {
    if (isa<LandingPadInst>(I) || isa<FuncletPadInst>(I)) {
      continue;
    }
    for (unsigned J = 0; J < I.getNumOperands(); J++) {
      if (ConstantExpr *C = dyn_cast<ConstantExpr>(I.getOperand(J))) {
        Instruction *InsertPt = &I;
        IRBuilder<NoFolder> IRB(InsertPt);
        if (isa<PHINode>(I)) {
          IRB.SetInsertPoint(FunctionInsertPt);
        }
        Instruction *Inst = IRB.Insert(C->getAsInstruction());
        I.setOperand(J, Inst);
      }
    }
  }
}

// LLVM-MSVC has this function, but the official LLVM version does not
// (LLVM: 17.0.6 | LLVM-MSVC: 3.2.6).
void llvm::lowerConstantExpr(Function &F) {
  SmallPtrSet<Instruction *, 8> WorkList;

  for (inst_iterator It = inst_begin(F), E = inst_end(F); It != E; ++It) {
    Instruction *I = &*It;

    if (isa<LandingPadInst>(I) || isa<CatchPadInst>(I) ||
        isa<CatchSwitchInst>(I) || isa<CatchReturnInst>(I))
      continue;

    if (auto *II = dyn_cast<IntrinsicInst>(I)) {
      if (II->getIntrinsicID() == Intrinsic::eh_typeid_for)
        continue;
    }

    for (unsigned int J = 0; J < I->getNumOperands(); ++J) {
      if (isa<ConstantExpr>(I->getOperand(J)))
        WorkList.insert(I);
    }
  }

  while (!WorkList.empty()) {
    auto It = WorkList.begin();
    Instruction *Instr = *It;
    WorkList.erase(*It);

    if (PHINode *PHI = dyn_cast<PHINode>(Instr)) {
      for (unsigned int I = 0; I < PHI->getNumIncomingValues(); ++I) {
        Instruction *TI = PHI->getIncomingBlock(I)->getTerminator();

        ConstantExpr *CE = dyn_cast<ConstantExpr>(PHI->getIncomingValue(I));
        if (!CE)
          continue;

        Instruction *NewInst = CE->getAsInstruction();
        NewInst->insertBefore(TI);
        PHI->setIncomingValue(I, NewInst);
        WorkList.insert(NewInst);
      }

      continue;
    }

    for (unsigned int I = 0; I < Instr->getNumOperands(); ++I) {
      ConstantExpr *CE = dyn_cast<ConstantExpr>(Instr->getOperand(I));
      if (!CE)
        continue;

      Instruction *NewInst = CE->getAsInstruction();
      NewInst->insertBefore(Instr);
      Instr->replaceUsesOfWith(CE, NewInst);
      WorkList.insert(NewInst);
    }
  }
}
