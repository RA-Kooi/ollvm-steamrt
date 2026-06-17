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
// 	         		     entry
//      			       |
//  	    	  	 ______v______
//   	    		|   Original  |
//   	    		|_____________|
//             		       |
// 		        	       v
//		        	     return
//
// After :
//           		     entry
//             		       |
//            		   ____v_____
//      			  |condition*| (false)
//           		  |__________|----+
//           		 (true)|          |
//             		       |          |
//           		 ______v______    |
// 		        +-->|   Original* |   |
// 		        |   |_____________| (true)
// 		        |   (false)|    !-----------> return
// 		        |    ______v______    |
// 		        |   |   Altered   |<--!
// 		        |   |_____________|
// 		        |__________|
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
//	      and after transformation if it has been modified, and all
//	      the functions at end of the pass, after doFinalization.
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
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include <list>

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
  // If fla annotations
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
    // Put all the function's block in a list
    std::list<BasicBlock *> BasicBlocks;
    for (Function::iterator I = F.begin(); I != F.end(); ++I) {
      BasicBlocks.push_back(&*I);
    }
    DEBUG_WITH_TYPE(
        "gen", errs() << "bcf: Iterating on the Function's Basic Blocks\n");

    while (!BasicBlocks.empty()) {
#ifndef NDEBUG
      NumBasicBlocks++;
#endif
      // Basic Blocks' selection
      if ((int)llvm::Cryptoutils->getRange(100) <= ObfProbRate) {
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
  BasicBlock::iterator I1 = Basic->begin();
  if (Basic->getFirstNonPHIOrDbgOrLifetime())
    I1 = (BasicBlock::iterator)Basic->getFirstNonPHIOrDbgOrLifetime();
  if (Basic->getFirstNonPHI()->isEHPad())
    return;
  // Fix Verifier.cpp: "CatchPadInst not the first non-PHI instruction in the
  // block.", "The unwind destination does not have an exception handling
  // instruction!"
  Twine *Var;
  Var = new Twine("originalBB");
  BasicBlock *OriginalBb = Basic->splitBasicBlock(I1, *Var);
  DEBUG_WITH_TYPE("gen", errs()
                             << "bcf: First and original basic blocks: ok\n");

  // Creating the altered basic block on which the first Basic block will jump
  Twine *Var3 = new Twine("alteredBB");
  BasicBlock *AlteredBb = createAlteredBasicBlock(OriginalBb, *Var3, &F);
  DEBUG_WITH_TYPE("gen", errs() << "bcf: Altered basic block: ok\n");

  // Now that all the blocks are created,
  // we modify the terminators to adjust the control flow.

  AlteredBb->getTerminator()->eraseFromParent();
  Basic->getTerminator()->eraseFromParent();
  DEBUG_WITH_TYPE("gen", errs() << "bcf: Terminator removed from the altered"
                                << " and first basic blocks\n");

  // Preparing a condition..
  // For now, the condition is an always true comparaison between 2 float
  // This will be complicated after the pass (in doFinalization())
  Value *LHS = ConstantFP::get(Type::getFloatTy(F.getContext()), 1.0);
  Value *RHS = ConstantFP::get(Type::getFloatTy(F.getContext()), 1.0);
  DEBUG_WITH_TYPE("gen", errs() << "bcf: Value LHS and RHS created\n");

  // The always true condition. End of the first block
  Twine *Var4 = new Twine("condition");
  FCmpInst *Condition =
      new FCmpInst(InsertPosition(Basic), FCmpInst::FCMP_TRUE, LHS, RHS, *Var4);
  DEBUG_WITH_TYPE("gen", errs() << "bcf: Always true condition created\n");

  // Jump to the original basic block if the condition is true or
  // to the altered block if false.
  BranchInst::Create(OriginalBb, AlteredBb, (Value *)Condition, Basic);
  DEBUG_WITH_TYPE(
      "gen",
      errs() << "bcf: Terminator instruction in first basic block: ok\n");

  // The altered block loop back on the original one.
  BranchInst::Create(OriginalBb, AlteredBb);
  DEBUG_WITH_TYPE(
      "gen", errs() << "bcf: Terminator instruction in altered block: ok\n");

  // The end of the originalBB is modified to give the impression that
  // sometimes it continues in the loop, and sometimes it return the desired
  // value (of course it's always true, so it always use the original
  // terminator..
  //  but this will be obfuscated too;) )

  // iterate on instruction just before the terminator of the originalBB
  BasicBlock::iterator I = OriginalBb->end();

  // Split at this point (we only want the terminator in the second part)
  Twine *Var5 = new Twine("originalBBpart2");
  BasicBlock *OriginalBBpart2 = OriginalBb->splitBasicBlock(--I, *Var5);
  DEBUG_WITH_TYPE("gen",
                  errs() << "bcf: Terminator part of the original basic block"
                         << " is isolated\n");
  // the first part go either on the return statement or on the begining
  // of the altered block.. So we erase the terminator created when splitting.
  OriginalBb->getTerminator()->eraseFromParent();
  // We add at the end a new always true condition
  Twine *Var6 = new Twine("condition2");
  FCmpInst *Condition2 = new FCmpInst(InsertPosition(OriginalBb),
                                      CmpInst::FCMP_TRUE, LHS, RHS, *Var6);
  BranchInst::Create(OriginalBBpart2, AlteredBb, (Value *)Condition2,
                     OriginalBb);
  DEBUG_WITH_TYPE("gen", errs()
                             << "bcf: Terminator original basic block: ok\n");
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
  BasicBlock *AlteredBb = llvm::CloneBasicBlock(Basic, VMap, Name, F);
  DEBUG_WITH_TYPE("gen", errs() << "bcf: Original basic block cloned\n");
  // Remap operands.
  BasicBlock::iterator Ji = Basic->begin();
  for (BasicBlock::iterator I = AlteredBb->begin(), E = AlteredBb->end();
       I != E; ++I) {
    // Loop over the operands of the instruction
    for (User::op_iterator Opi = I->op_begin(), Ope = I->op_end(); Opi != Ope;
         ++Opi) {
      // get the value for the operand
      Value *V = MapValue(*Opi, VMap, RF_None, 0);
      if (V != 0) {
        *Opi = V;
        DEBUG_WITH_TYPE("gen", errs()
                                   << "bcf: Value's operand has been setted\n");
      }
    }
    DEBUG_WITH_TYPE("gen", errs() << "bcf: Operands remapped\n");
    // Remap phi nodes' incoming blocks.
    if (PHINode *Pn = dyn_cast<PHINode>(I)) {
      for (unsigned J = 0, E = Pn->getNumIncomingValues(); J != E; ++J) {
        Value *V = MapValue(Pn->getIncomingBlock(J), VMap, RF_None, 0);
        if (V != 0) {
          Pn->setIncomingBlock(J, cast<BasicBlock>(V));
        }
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
    DEBUG_WITH_TYPE("gen", errs()
                               << "bcf: Debug information location setted\n");

  } // The instructions' informations are now all correct

  for (auto I = AlteredBb->begin(), E = AlteredBb->end(); I != E;) {
    Instruction *Instr = &*I++;
    if (isa<DbgInfoIntrinsic>(Instr))
      Instr->eraseFromParent();
  }
  // Fix Verifier.cpp: "mismatched subprogram between llvm.dbg.value label and
  // !dbg attachment"

  DEBUG_WITH_TYPE("gen", errs()
                             << "bcf: The cloned basic block is now correct\n");
  DEBUG_WITH_TYPE(
      "gen",
      errs() << "bcf: Starting to add junk code in the cloned bloc...\n");

  // add random instruction in the middle of the bloc. This part can be
  // improve
  for (BasicBlock::iterator I = AlteredBb->begin(), E = AlteredBb->end();
       I != E; ++I) {
    // in the case we find binary operator, we modify slightly this part by
    // randomly insert some instructions
    if (I->isBinaryOp()) { // binary instructions
      unsigned Opcode = I->getOpcode();
      Instruction *Op, *Op1 = NULL;
      Twine *Var = new Twine("_");
      // treat differently float or int
      // Binary int
      if (Opcode == Instruction::Add || Opcode == Instruction::Sub ||
          Opcode == Instruction::Mul || Opcode == Instruction::UDiv ||
          Opcode == Instruction::SDiv || Opcode == Instruction::URem ||
          Opcode == Instruction::SRem || Opcode == Instruction::Shl ||
          Opcode == Instruction::LShr || Opcode == Instruction::AShr ||
          Opcode == Instruction::And || Opcode == Instruction::Or ||
          Opcode == Instruction::Xor) {
        for (int Random = (int)llvm::Cryptoutils->getRange(10); Random < 10;
             ++Random) {
          switch (llvm::Cryptoutils->getRange(4)) { // to improve
          case 0:                                   // do nothing
            break;
          case 1:
            Op = BinaryOperator::CreateNeg(I->getOperand(0), *Var, &*I);
            Op1 = BinaryOperator::Create(Instruction::Add, Op, I->getOperand(1),
                                         "gen", &*I);
            break;
          case 2:
            Op1 = BinaryOperator::Create(Instruction::Sub, I->getOperand(0),
                                         I->getOperand(1), *Var, &*I);
            Op = BinaryOperator::Create(Instruction::Mul, Op1, I->getOperand(1),
                                        "gen", &*I);
            break;
          case 3:
            Op = BinaryOperator::Create(Instruction::Shl, I->getOperand(0),
                                        I->getOperand(1), *Var, &*I);
            break;
          }
        }
      }
      // Binary float
      if (Opcode == Instruction::FAdd || Opcode == Instruction::FSub ||
          Opcode == Instruction::FMul || Opcode == Instruction::FDiv ||
          Opcode == Instruction::FRem) {
        for (int Random = (int)llvm::Cryptoutils->getRange(10); Random < 10;
             ++Random) {
          switch (llvm::Cryptoutils->getRange(3)) { // can be improved
          case 0:                                   // do nothing
            break;
          case 1:
            Op = UnaryOperator::CreateFNeg(I->getOperand(0), *Var, &*I);
            Op1 = BinaryOperator::Create(Instruction::FAdd, Op,
                                         I->getOperand(1), "gen", &*I);
            break;
          case 2:
            Op = BinaryOperator::Create(Instruction::FSub, I->getOperand(0),
                                        I->getOperand(1), *Var, &*I);
            Op1 = BinaryOperator::Create(Instruction::FMul, Op,
                                         I->getOperand(1), "gen", &*I);
            break;
          }
        }
      }
      if (Opcode == Instruction::ICmp) { // Condition (with int)
        ICmpInst *CurrentI = (ICmpInst *)(&I);
        switch (llvm::Cryptoutils->getRange(3)) { // must be improved
        case 0:                                   // do nothing
          break;
        case 1:
          CurrentI->swapOperands();
          break;
        case 2: // randomly change the predicate
          switch (llvm::Cryptoutils->getRange(10)) {
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
        switch (llvm::Cryptoutils->getRange(3)) { // must be improved
        case 0:                                   // do nothing
          break;
        case 1:
          CurrentI->swapOperands();
          break;
        case 2: // randomly change the predicate
          switch (llvm::Cryptoutils->getRange(10)) {
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

  //  The global values
  Twine *VarX = new Twine("x");
  Twine *VarY = new Twine("y");
  Value *X1 = ConstantInt::get(Type::getInt32Ty(M.getContext()), 0, false);
  Value *Y1 = ConstantInt::get(Type::getInt32Ty(M.getContext()), 0, false);

  GlobalVariable *X =
      new GlobalVariable(M, Type::getInt32Ty(M.getContext()), false,
                         GlobalValue::CommonLinkage, (Constant *)X1, *VarX);
  GlobalVariable *Y =
      new GlobalVariable(M, Type::getInt32Ty(M.getContext()), false,
                         GlobalValue::CommonLinkage, (Constant *)Y1, *VarY);

  std::vector<Instruction *> ToEdit, ToDelete;
  BinaryOperator *Op, *Op1 = NULL;
  LoadInst *OpX, *OpY;
  ICmpInst *Condition, *Condition2;
  // Looking for the conditions and branches to transform

  for (Function::iterator Fi = F.begin(), Fe = F.end(); Fi != Fe; ++Fi) {
    // fi->setName("");
    Instruction *Tbb = Fi->getTerminator();
    if (Tbb->getOpcode() == Instruction::Br) {
      BranchInst *Br = (BranchInst *)(Tbb);
      if (Br->isConditional()) {
        FCmpInst *Cond = (FCmpInst *)Br->getCondition();
        unsigned Opcode = Cond->getOpcode();
        if (Opcode == Instruction::FCmp) {
          if (Cond->getPredicate() == FCmpInst::FCMP_TRUE) {
            DEBUG_WITH_TYPE("gen", errs()
                                       << "bcf: an always true predicate !\n");
            ToDelete.push_back(Cond); // The condition
            ToEdit.push_back(Tbb);    // The branch using the condition
          }
        }
      }
    }
    /*
    for (BasicBlock::iterator bi = fi->begin(), be = fi->end() ; bi != be;
    ++bi){ bi->setName(""); // setting the basic blocks' names
    }
    */
  }

  // Replacing all the branches we found
  for (std::vector<Instruction *>::iterator I = ToEdit.begin();
       I != ToEdit.end(); ++I) {
    // if y < 10 || x*(x+1) % 2 == 0
    OpX = new LoadInst(Type::getInt32Ty(M.getContext()), (Value *)X, "", (*I));
    OpY = new LoadInst(Type::getInt32Ty(M.getContext()), (Value *)Y, "", (*I));

    Op = BinaryOperator::Create(
        Instruction::Sub, (Value *)OpX,
        ConstantInt::get(Type::getInt32Ty(M.getContext()), 1, false), "", (*I));
    Op1 = BinaryOperator::Create(Instruction::Mul, (Value *)OpX, Op, "", (*I));
    Op = BinaryOperator::Create(
        Instruction::URem, Op1,
        ConstantInt::get(Type::getInt32Ty(M.getContext()), 2, false), "", (*I));
    Condition = new ICmpInst(
        (*I), ICmpInst::ICMP_EQ, Op,
        ConstantInt::get(Type::getInt32Ty(M.getContext()), 0, false));
    Condition2 = new ICmpInst(
        (*I), ICmpInst::ICMP_SLT, OpY,
        ConstantInt::get(Type::getInt32Ty(M.getContext()), 10, false));
    Op1 = BinaryOperator::Create(Instruction::Or, (Value *)Condition,
                                 (Value *)Condition2, "", (*I));

    BranchInst::Create(((BranchInst *)*I)->getSuccessor(0),
                       ((BranchInst *)*I)->getSuccessor(1), (Value *)Op1,
                       ((BranchInst *)*I)->getParent());
    DEBUG_WITH_TYPE("gen", errs() << "bcf: Erase branch instruction:"
                                  << *((BranchInst *)*I) << "\n");
    (*I)->eraseFromParent(); // erase the branch
  }
  // Erase all the associated conditions we found
  for (std::vector<Instruction *>::iterator I = ToDelete.begin();
       I != ToDelete.end(); ++I) {
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
