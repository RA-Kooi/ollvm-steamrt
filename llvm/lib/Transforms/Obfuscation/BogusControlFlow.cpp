//===- BogusControlFlow.h - BogusControlFlow Obfuscation
// pass-------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===--------------------------------------------------------------------------------===//
//
// This file contains includes and defines for the bogusControlFlow pass
//
//===--------------------------------------------------------------------------------===//
/*
    LLVM BogusControlFlow Pass
    The main modification is the branching condition is calculated on-the-fly
    Instead of hard-code the always true condition. Relicensed from NCSA license
    to AGPL Copyright (C) 2017 Zhang(https://github.com/Naville/)
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

//===- BogusControlFlow.cpp - BogusControlFlow Obfuscation
// pass-------------------------===//
//
// This file implements BogusControlFlow's pass, inserting bogus control flow.
// It adds bogus flow to a given basic block this way:
//
// Before :
//          entry
//            |
//      ______v______
//     |   Original  |
//     |_____________|
//            |
//            v
//         return
//
// After :
//          entry
//            |
//        ____v_____
//       |condition*| (false)
//       |__________|----+
//      (true)|          |
//            |          |
//      ______v______    |
// +-->|   Original* |   |
// |   |_____________| (true)
// |   (false)|    !-----------> return
// |    ______v______    |
// |   |   Altered   |<--!
// |   |_____________|
// |__________|
//
//  * The results of these terminator's branch's conditions are always true, but
//  these predicates are
//    opacificated. For this, we declare two global values: x and y, and replace
//    the FCMP_TRUE predicate with (y < 10 || x * (x + 1) % 2 == 0) (this could
//    be improved, as the global values give a hint on where are the opaque
//    predicates)
//
//  The altered bloc is a copy of the original's one with junk instructions
//  added accordingly to the type of instructions we found in the bloc
//
//  Each basic block of the function is choosen if a random number in the range
//  [0,100] is smaller than the choosen probability rate. The default value
//  is 30. This value can be modify using the option -boguscf-prob=[value].
//  Value must be an integer in the range [0, 100], otherwise the default value
//  is taken. Exemple: -boguscf -boguscf-prob=60
//
//  The pass can also be loop many times on a function, including on the basic
//  blocks added in a previous loop. Be careful if you use a big probability
//  number and choose to run the loop many times wich may cause the pass to run
//  for a very long time. The default value is one loop, but you can change it
//  with -boguscf-loop=[value]. Value must be an integer greater than 1,
//  otherwise the default value is taken. Exemple: -boguscf -boguscf-loop=2
//
//
//  Defined debug types:
//  - "gen" : general informations
//  - "opt" : concerning the given options (parameter)
//  - "cfg" : printing the various function's cfg before transformation
//            and after transformation if it has been modified, and all
//            the functions at end of the pass, after doFinalization.
//
//  To use them all, simply use the -debug option.
//  To use only one of them, follow the pass' command by -debug-only=name.
//  Exemple, -boguscf -debug-only=cfg
//
//
//  Stats:
//  The following statistics will be printed if you use
//  the -stats command:
//
// a. Number of functions in this module
// b. Number of times we run on each function
// c. Initial number of basic blocks in this module
// d. Number of modified basic blocks
// e. Number of added basic blocks in this module
// f. Final number of basic blocks in this module
//
// file   : lib/Transforms/Obfuscation/BogusControlFlow.cpp
// date   : june 2012
// version: 1.0
// author : julie.michielin@gmail.com
// modifications: pjunod, Rinaldini Julien
// project: Obfuscator
// option : -boguscf
//
//===----------------------------------------------------------------------------------===//
#include "llvm/Transforms/Obfuscation/BogusControlFlow.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <deque>
#include <vector>

#define DEBUG_TYPE "BogusControlFlow"

using namespace llvm;

STATISTIC(NumFunction, "a. Number of functions in this module");
STATISTIC(NumTimesOnFunctions, "b. Number of times we run on each function");

STATISTIC(InitNumBasicBlocks,
          "c. Initial number of basic blocks in this module");

STATISTIC(NumModifiedBasicBlocks, "d. Number of modified basic blocks");

STATISTIC(NumAddedBasicBlocks,
          "e. Number of added basic blocks in this module");

STATISTIC(FinalNumBasicBlocks,
          "f. Final number of basic blocks in this module");

// Options for the pass
constexpr int DefaultObfRate = 70;
constexpr int DefaultObfTime = 2;

static cl::opt<bool> BcfEnabled("bcf", cl::init(false),
                                cl::desc("BogusControlFlow: application number "
                                         "-bcf_loop=x must be x > 0"));

static cl::opt<int>
    ObfProbRate("bcf_prob",
                cl::desc("Choose the probability [%] each basic blocks will be "
                         "obfuscated by the -bcf pass"),
                cl::value_desc("probability rate"), cl::init(DefaultObfRate),
                cl::Optional);

static cl::opt<int>
    ObfTimes("bcf_loop",
             cl::desc("Choose how many time the -bcf pass loop on a function"),
             cl::value_desc("number of times"), cl::init(DefaultObfTime),
             cl::Optional);

static BasicBlock *createAlteredBasicBlock(BasicBlock *BasicBlock,
                                           const Twine &Name = "gen",
                                           Function *F = 0);

static void bogus(Function &F);
static void addBogusFlow(BasicBlock *Basic, Function &F);
static bool doF(Module &M, Function &F);

PreservedAnalyses BogusControlFlowPass::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  // Check if the percentage is correct
  if (ObfTimes <= 0) {
    errs() << "BogusControlFlow application number -bcf_loop=x must be x > 0";

    return PreservedAnalyses::all();
  }

  // Check if the number of applications is correct
  if (!((ObfProbRate > 0) && (ObfProbRate <= 100))) {
    errs() << "BogusControlFlow application basic blocks percentage "
              "-bcf_prob=x must be 0 < x <= 100";

    return PreservedAnalyses::all();
  }

  // If bcf annotations
  if (shouldObfuscate(BcfEnabled, &F, "bcf")) {
    bogus(F);
    doF(*F.getParent(), F);

    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

static void bogus(Function &F) {
  // For statistics and debug
  ++NumFunction;

#ifndef NDEBUG
  int NumBasicBlocks = 0;
#endif
  bool FirstTime = true; // First time we do the loop in this function
  bool HasBeenModified = false;

  DEBUG_WITH_TYPE("opt",
                  errs() << "bcf: Started on function " << F.getName() << "\n");

  DEBUG_WITH_TYPE("opt",
                  errs() << "bcf: Probability rate: " << ObfProbRate << "\n");

  if (ObfProbRate < 0 || ObfProbRate > 100) {
    DEBUG_WITH_TYPE("opt", errs() << "bcf: Incorrect value,"
                                  << " probability rate set to default value: "
                                  << DefaultObfRate << " \n");

    ObfProbRate = DefaultObfRate;
  }

  DEBUG_WITH_TYPE("opt", errs() << "bcf: How many times: " << ObfTimes << "\n");

  if (ObfTimes <= 0) {
    DEBUG_WITH_TYPE("opt", errs() << "bcf: Incorrect value,"
                                  << " must be greater than 1. Set to default: "
                                  << DefaultObfTime << " \n");

    ObfTimes = DefaultObfTime;
  }

  NumTimesOnFunctions = ObfTimes;
  int NumObfTimes = ObfTimes;

  // Real begining of the pass
  // Loop for the number of time we run the pass on the function
  do {
    DEBUG_WITH_TYPE("cfg", errs() << "bcf: Function " << F.getName()
                                  << ", before the pass:\n");

    DEBUG_WITH_TYPE("cfg", F.viewCFG());

    std::deque<BasicBlock *> BasicBlocks;
    for (auto I = F.begin(); I != F.end(); ++I)
      BasicBlocks.push_back(&*I);

    DEBUG_WITH_TYPE("gen", errs() << "bcf: Iterating on the Function's Basic Blocks\n");

    while (!BasicBlocks.empty()) {
#ifndef NDEBUG
      NumBasicBlocks++;
#endif

      // Basic Blocks' selection
      if ((int)Cryptoutils->getRange(100) <= ObfProbRate) {
        DEBUG_WITH_TYPE("opt", errs() << "bcf: Block " << NumBasicBlocks
                                      << " selected. \n");

        HasBeenModified = true;

        ++NumModifiedBasicBlocks;
        NumAddedBasicBlocks += 3;
        FinalNumBasicBlocks += 3;

        // Add bogus flow to the given Basic Block (see description)
        BasicBlock *BasicBlock = BasicBlocks.front();
        addBogusFlow(BasicBlock, F);
      } else {
        DEBUG_WITH_TYPE("opt", errs() << "bcf: Block " << NumBasicBlocks
                                      << " not selected.\n");
      }

      // remove the block from the list
      BasicBlocks.pop_front();

      if (FirstTime) { // first time we iterate on this function
        ++InitNumBasicBlocks;
        ++FinalNumBasicBlocks;
      }
    } // end of while(!basicBlocks.empty())

    DEBUG_WITH_TYPE("gen",
                    errs() << "bcf: End of function " << F.getName() << "\n");

    if (HasBeenModified) { // if the function has been modified
      DEBUG_WITH_TYPE("cfg", errs() << "bcf: Function " << F.getName()
                                    << ", after the pass: \n");

      DEBUG_WITH_TYPE("cfg", F.viewCFG());
    } else {
      DEBUG_WITH_TYPE("cfg", errs() << "bcf: Function's not been modified \n");
    }

    FirstTime = false;
  } while (--NumObfTimes > 0);
}

/* addBogusFlow
 *
 * Add bogus flow to a given basic block, according to the header's
 * description
 */
static void addBogusFlow(BasicBlock *Basic, Function &F) {
  // Split the block: first part with only the phi nodes and debug info and
  // terminator
  //                  created by splitBasicBlock. (-> No instruction)
  //                  Second part with every instructions from the original
  //                  block
  // We do this way, so we don't have to adjust all the phi nodes, metadatas
  // and so on for the first block. We have to let the phi nodes in the first
  // part, because they actually are updated in the second part according to
  // them.
  auto It = Basic->getFirstNonPHIOrDbgOrLifetime()->getIterator();
  auto IsValidInst = [](decltype(It) &It) {
    return It->isEHPad()
      || isa<AllocaInst>(*It)
      || isa<DbgInfoIntrinsic>(*It);
  };

  while (It != Basic->end() && IsValidInst(It))
    ++It;

  if (It == Basic->end() || It == Basic->getTerminator()->getIterator())
    return;

  // Fix Verifier.cpp: "CatchPadInst not the first non-PHI instruction in the
  // block.", "The unwind destination does not have an exception handling
  // instruction!"
  BasicBlock *SplitBB = Basic->splitBasicBlock(It, "splitBB");

  DEBUG_WITH_TYPE("gen", errs() << "bcf: First and original basic blocks: ok\n");

  // Creating the altered basic block on which the first Basic block will jump
  BasicBlock *AlteredBb = createAlteredBasicBlock(SplitBB, "alteredBB", &F);

  DEBUG_WITH_TYPE("gen", errs() << "bcf: Altered basic block: ok\n");

  // Now that all the blocks are created,
  // we modify the terminators to adjust the control flow.
  AlteredBb->getTerminator()->eraseFromParent();
  Basic->getTerminator()->eraseFromParent();

  IRBuilder<NoFolder> IRB(Basic);

  DEBUG_WITH_TYPE("gen", errs() << "bcf: Terminator removed from the altered"
                                << " and first basic blocks\n");

  // Preparing a condition..
  // For now, the condition is an always true comparaison between 2 float
  // This will be complicated after the pass (in doFinalization())
  Type *FloatTy = Type::getFloatTy(F.getContext());
  Value *LHS = ConstantFP::get(FloatTy, 1.0);
  Value *RHS = ConstantFP::get(FloatTy, 1.0);

  DEBUG_WITH_TYPE("gen", errs() << "bcf: Value LHS and RHS created\n");

  // The always true condition. End of the first block
  Value *Condition = IRB.CreateFCmp(FCmpInst::FCMP_TRUE, LHS, RHS);

  DEBUG_WITH_TYPE("gen", errs() << "bcf: Always true condition created\n");

  // Jump to the original basic block if the condition is true or
  // to the altered block if false.
  IRB.CreateCondBr(Condition, SplitBB, AlteredBb);

  DEBUG_WITH_TYPE("gen", errs() << "bcf: Terminator instruction in first basic block: ok\n");

  // The altered block loop back on the original one.
  IRB.SetInsertPoint(AlteredBb);
  IRB.CreateBr(SplitBB);

  DEBUG_WITH_TYPE("gen", errs() << "bcf: Terminator instruction in altered block: ok\n");

  // The end of the originalBB is modified to give the impression that
  // sometimes it continues in the loop, and sometimes it return the desired
  // value (of course it's always true, so it always use the original
  // terminator.. but this will be obfuscated too ;) )

  // iterate on instruction just before the terminator of the splitBB
  auto I = SplitBB->end();

  // Split at this point (we only want the terminator in the second part)
  BasicBlock *SplitBBpart2 = SplitBB->splitBasicBlock(--I, "splitBBpart2");

  DEBUG_WITH_TYPE("gen",
                  errs() << "bcf: Terminator part of the original basic block"
                         << " is isolated\n");

  // the first part go either on the return statement or on the begining
  // of the altered block.. So we erase the terminator created when splitting.
  SplitBB->getTerminator()->eraseFromParent();

  // We add at the end a new always true condition
  IRB.SetInsertPoint(SplitBB);
  Value *Condition2 = IRB.CreateFCmp(CmpInst::FCMP_TRUE, LHS, RHS);
  IRB.CreateCondBr(Condition2, SplitBBpart2, AlteredBb);

  DEBUG_WITH_TYPE("gen", errs() << "bcf: Terminator original basic block: ok\n");

  DEBUG_WITH_TYPE("gen", errs() << "bcf: End of addBogusFlow().\n");

} // end of addBogusFlow()

/* createAlteredBasicBlock
 *
 * This function return a basic block similar to a given one.
 * It's inserted just after the given basic block.
 * The instructions are similar but junk instructions are added between
 * the cloned one. The cloned instructions' phi nodes, metadatas, uses and
 * debug locations are adjusted to fit in the cloned basic block and
 * behave nicely.
 */
BasicBlock *createAlteredBasicBlock(BasicBlock *Basic, const Twine &Name,
                                    Function *F) {
  // Useful to remap the informations concerning instructions.
  ValueToValueMapTy VMap;

  // Basic->dump();
  BasicBlock *AlteredBb = CloneBasicBlock(Basic, VMap, Name, F);

  DEBUG_WITH_TYPE("gen", errs() << "bcf: Original basic block cloned\n");

  // Remap operands.
  auto Ji = Basic->begin();
  for (auto I = AlteredBb->begin(), E = AlteredBb->end(); I != E; ++I) {
    // Loop over the operands of the instruction
    for (auto Opi = I->op_begin(), Ope = I->op_end(); Opi != Ope; ++Opi) {
      // get the value for the operand
      Value *V = MapValue(*Opi, VMap, RF_NoModuleLevelChanges, 0);

      if (V) {
        *Opi = V;
        DEBUG_WITH_TYPE("gen", errs() << "bcf: Value's operand has been setted\n");
      }
    }

    DEBUG_WITH_TYPE("gen", errs() << "bcf: Operands remapped\n");

    // Remap phi nodes' incoming blocks.
    if (PHINode *Pn = dyn_cast<PHINode>(I)) {
      for (unsigned J = 0, E = Pn->getNumIncomingValues(); J != E; ++J) {
        Value *V = MapValue(Pn->getIncomingBlock(J), VMap, RF_None, 0);

        if (V)
          Pn->setIncomingBlock(J, cast<BasicBlock>(V));
      }
    }

    DEBUG_WITH_TYPE("gen", errs() << "bcf: PHINodes remapped\n");

    // Remap attached metadata.
    SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
    I->getAllMetadata(MDs);

    DEBUG_WITH_TYPE("gen", errs() << "bcf: Metadatas remapped\n");

    // important for compiling with DWARF, using option -g.
    I->setDebugLoc(Ji->getDebugLoc());
    Ji++;

    DEBUG_WITH_TYPE("gen", errs() << "bcf: Debug information location setted\n");
  } // The instructions' informations are now all correct

  for (auto I = AlteredBb->begin(), E = AlteredBb->end(); I != E;) {
    Instruction *Instr = &*I++;

    if (isa<DbgInfoIntrinsic>(Instr))
      Instr->eraseFromParent();
  }

  // Fix Verifier.cpp: "mismatched subprogram between llvm.dbg.value label and
  // !dbg attachment"
  DEBUG_WITH_TYPE("gen", errs() << "bcf: The cloned basic block is now correct\n");
  DEBUG_WITH_TYPE("gen", errs() << "bcf: Starting to add junk code in the cloned block...\n");

  // add random instruction in the middle of the bloc. This part can be
  // improve
  for (auto I = AlteredBb->begin(), E = AlteredBb->end(); I != E; ++I) {
    // in the case we find binary operator, we modify this part slightly by
    // randomly inserting some instructions
    if (!I->isBinaryOp())
      continue;

    // binary instructions
    unsigned Opcode = I->getOpcode();
    IRBuilder<> IRB(&*I);

    // treat differently float or int
    // Binary int
    if (Opcode == Instruction::Add || Opcode == Instruction::Sub ||
        Opcode == Instruction::Mul || Opcode == Instruction::UDiv ||
        Opcode == Instruction::SDiv || Opcode == Instruction::URem ||
        Opcode == Instruction::SRem || Opcode == Instruction::Shl ||
        Opcode == Instruction::LShr || Opcode == Instruction::AShr ||
        Opcode == Instruction::And || Opcode == Instruction::Or ||
        Opcode == Instruction::Xor) {
      for (int Random = (int)Cryptoutils->getRange(10); Random < 10; ++Random) {
        switch (Cryptoutils->getRange(4)) { // to improve
          case 0:                           // do nothing
          break;
          case 1: {
            Value *Op = IRB.CreateNeg(I->getOperand(0));
            IRB.CreateAdd(Op, I->getOperand(1));
          } break;
          case 2: {
            Value *Op = IRB.CreateSub(I->getOperand(0), I->getOperand(1));
            IRB.CreateMul(Op, I->getOperand(1));
          } break;
          case 3:
            IRB.CreateShl(I->getOperand(0), I->getOperand(1));
         break;
        }
      }
    }

    // Binary float
    if (Opcode == Instruction::FAdd || Opcode == Instruction::FSub ||
        Opcode == Instruction::FMul || Opcode == Instruction::FDiv ||
        Opcode == Instruction::FRem) {
      for (int Random = (int)Cryptoutils->getRange(10); Random < 10; ++Random) {
        switch (Cryptoutils->getRange(3)) { // can be improved
          case 0:                           // do nothing
          break;
          case 1: {
            Value *Op = IRB.CreateFNeg(I->getOperand(0));
            IRB.CreateFAdd(Op, I->getOperand(1));
          } break;
          case 2: {
            Value *Op = IRB.CreateFSub(I->getOperand(0), I->getOperand(1));
            IRB.CreateFMul(Op, I->getOperand(1));
          } break;
        }
      }
    }

    if (Opcode == Instruction::ICmp) { // Condition (with int)
      ICmpInst *CurrentI = (ICmpInst *)(&I);
      switch (Cryptoutils->getRange(3)) { // must be improved
        case 0:                           // do nothing
        break;
        case 1:
          CurrentI->swapOperands();
        break;
        case 2: // randomly change the predicate
        switch (Cryptoutils->getRange(10)) {
          case 0:
            CurrentI->setPredicate(ICmpInst::ICMP_EQ);
          break; // equal
          case 1:
            CurrentI->setPredicate(ICmpInst::ICMP_NE);
          break; // not equal
          case 2:
            CurrentI->setPredicate(ICmpInst::ICMP_UGT);
          break; // unsigned greater than
          case 3:
            CurrentI->setPredicate(ICmpInst::ICMP_UGE);
          break; // unsigned greater or equal
          case 4:
            CurrentI->setPredicate(ICmpInst::ICMP_ULT);
          break; // unsigned less than
          case 5:
            CurrentI->setPredicate(ICmpInst::ICMP_ULE);
          break; // unsigned less or equal
          case 6:
            CurrentI->setPredicate(ICmpInst::ICMP_SGT);
          break; // signed greater than
          case 7:
            CurrentI->setPredicate(ICmpInst::ICMP_SGE);
          break; // signed greater or equal
          case 8:
            CurrentI->setPredicate(ICmpInst::ICMP_SLT);
          break; // signed less than
          case 9:
            CurrentI->setPredicate(ICmpInst::ICMP_SLE);
          break; // signed less or equal
        }
        break;
      }
    }

    if (Opcode == Instruction::FCmp) { // Conditions (with float)
      FCmpInst *CurrentI = (FCmpInst *)(&I);
      switch (Cryptoutils->getRange(3)) { // must be improved
        case 0:                           // do nothing
        break;
        case 1:
          CurrentI->swapOperands();
        break;
        case 2: // randomly change the predicate
        switch (Cryptoutils->getRange(10)) {
          case 0:
            CurrentI->setPredicate(FCmpInst::FCMP_OEQ);
          break; // ordered and equal
          case 1:
            CurrentI->setPredicate(FCmpInst::FCMP_ONE);
          break; // ordered and operands are unequal
          case 2:
            CurrentI->setPredicate(FCmpInst::FCMP_UGT);
          break; // unordered or greater than
          case 3:
            CurrentI->setPredicate(FCmpInst::FCMP_UGE);
          break; // unordered, or greater than, or equal
          case 4:
            CurrentI->setPredicate(FCmpInst::FCMP_ULT);
          break; // unordered or less than
          case 5:
            CurrentI->setPredicate(FCmpInst::FCMP_ULE);
          break; // unordered, or less than, or equal
          case 6:
            CurrentI->setPredicate(FCmpInst::FCMP_OGT);
          break; // ordered and greater than
          case 7:
            CurrentI->setPredicate(FCmpInst::FCMP_OGE);
          break; // ordered and greater than or equal
          case 8:
            CurrentI->setPredicate(FCmpInst::FCMP_OLT);
          break; // ordered and less than
          case 9:
            CurrentI->setPredicate(FCmpInst::FCMP_OLE);
          break; // ordered or less than, or equal
        }
        break;
      }
    }
  }

  return AlteredBb;
} // end of createAlteredBasicBlock()

/* doFinalization
 *
 * Overwrite FunctionPass method to apply the transformations to the whole
 * module. This part obfuscate all the always true predicates of the module.
 * More precisely, the condition which predicate is FCMP_TRUE.
 * It also remove all the functions' basic blocks' and instructions' names.
 */
static bool doF(Module &M, Function &F) {
  // In this part we extract all always-true predicate and replace them with
  // opaque predicate: For this, we declare two global values: x and y, and
  // replace the FCMP_TRUE predicate with (y < 10 || x * (x + 1) % 2 == 0) A
  // better way to obfuscate the predicates would be welcome. In the meantime
  // we will erase the name of the basic blocks, the instructions and the
  // functions.
  DEBUG_WITH_TYPE("gen", errs() << "bcf: Starting doFinalization...\n");

  Type *IntType = Type::getInt32Ty(M.getContext());

  //  The global values
  Constant *X1 = ConstantInt::get(IntType, 0, false);
  Constant *Y1 = ConstantInt::get(IntType, 0, false);

  GlobalVariable *X = new GlobalVariable(M, IntType, false, GlobalValue::CommonLinkage, X1, "x");
  GlobalVariable *Y = new GlobalVariable(M, IntType, false, GlobalValue::CommonLinkage, Y1, "y");

  std::vector<Instruction *> ToEdit, ToDelete;

  // Looking for the conditions and branches to transform
  for (auto Fi = F.begin(), Fe = F.end(); Fi != Fe; ++Fi) {
    Instruction *Tbb = Fi->getTerminator();

    if (Tbb->getOpcode() != Instruction::Br)
      continue;

    BranchInst *Br = (BranchInst *)(Tbb);
    if (!Br->isConditional())
      continue;

    FCmpInst *Cond = (FCmpInst *)Br->getCondition();
    unsigned Opcode = Cond->getOpcode();
    if (Opcode != Instruction::FCmp)
      continue;

    if (Cond->getPredicate() != FCmpInst::FCMP_TRUE)
      continue;

    DEBUG_WITH_TYPE("gen", errs() << "bcf: an always true predicate !\n");

    ToDelete.push_back(Cond); // The condition
    ToEdit.push_back(Tbb);    // The branch using the condition
  }

  // Replacing all the branches we found
  for (auto I = ToEdit.begin(); I != ToEdit.end(); ++I) {
    // if y < 10 || x * (x - 1) % 2 == 0
    IRBuilder<NoFolder> IRB(*I);

    Value *OpX = IRB.CreateLoad(IntType, X);
    Value *OpY = IRB.CreateLoad(IntType, Y);

    // x - 1
    Value *Op = IRB.CreateSub(OpX, ConstantInt::get(IntType, 1));

    // x * (x - 1)
    Value *Op1 = IRB.CreateMul(OpX, Op);

    // x * (x - 1) % 2
    Op = IRB.CreateURem(Op1, ConstantInt::get(IntType, 2));

    // x * (x - 1) % 2 == 0
    Value *Condition = IRB.CreateICmpEQ(Op, ConstantInt::get(IntType, 0));

    // y < 10
    Value *Condition2 = IRB.CreateICmpSLT(OpY, ConstantInt::get(IntType, 10));

    // if y < 10 || x * (x - 1) % 2 == 0
    Op1 = IRB.CreateOr(Condition, Condition2);

    BasicBlock *LHS = ((BranchInst *)*I)->getSuccessor(0);
    BasicBlock *RHS = ((BranchInst *)*I)->getSuccessor(1);
    IRB.CreateCondBr(Op1, LHS, RHS);

    DEBUG_WITH_TYPE("gen", errs() << "bcf: Erase branch instruction:"
                                  << *((BranchInst *)*I) << "\n");

    (*I)->eraseFromParent(); // erase the branch
  }

  // Erase all the associated conditions we found
  for (auto I = ToDelete.begin(); I != ToDelete.end(); ++I) {
    DEBUG_WITH_TYPE("gen", errs() << "bcf: Erase condition instruction:"
                                  << *((Instruction *)*I) << "\n");

    (*I)->eraseFromParent();
  }

  // Only for debug
  DEBUG_WITH_TYPE("cfg", errs() << "bcf: End of the pass, here are the "
                                   "graphs after doFinalization\n");

  // for (Module::iterator mi = M.begin(), me = M.end(); mi != me; ++mi) {
  //   DEBUG_WITH_TYPE("cfg", errs() << "bcf: Function " << mi->getName() <<
  //   "\n"); DEBUG_WITH_TYPE("cfg", mi->viewCFG());
  // }

  return true;
}
