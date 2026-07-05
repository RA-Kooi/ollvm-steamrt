//===- Substitution.cpp - Substitution Obfuscation
// pass-------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements operators substitution's pass
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/Substitution.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"

#define DEBUG_TYPE "substitution"

using namespace llvm;

static cl::opt<bool> SubEnabled("sub", cl::init(false),
                                cl::desc("Substitution: sub_loop"));

static cl::opt<int>
    ObfTimes("sub_loop",
             cl::desc("Choose how many time the -sub pass loops on a function"),
             cl::value_desc("number of times"), cl::init(1), cl::Optional);

STATISTIC(Add, "Add substitued");
STATISTIC(Sub, "Sub substitued");
// STATISTIC(Mul,  "Mul substitued");
// STATISTIC(Div,  "Div substitued");
// STATISTIC(Rem,  "Rem substitued");
// STATISTIC(Shi,  "Shift substitued");
STATISTIC(And, "And substitued");
STATISTIC(Or, "Or substitued");
STATISTIC(Xor, "Xor substitued");

PreservedAnalyses SubstitutionPass::run(Function &F,
                                        FunctionAnalysisManager &AM) {
  // Check if the percentage is correct
  if (ObfTimes <= 0) {
    errs() << "Substitution application number -sub_loop=x must be x > 0\n";
    return PreservedAnalyses::all();
  }

  // Do we obfuscate
  if (shouldObfuscate(SubEnabled, &F, "sub")) {
    substitute(&F);
    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

void SubstitutionPass::substitute(Function *F) {
  // Loop for the number of time we run the pass on the function
  int Times = ObfTimes;
  do {
    for (Function::iterator Bb = F->begin(); Bb != F->end(); ++Bb) {
      for (BasicBlock::iterator Inst = Bb->begin(); Inst != Bb->end(); ++Inst) {
        if (Inst->isBinaryOp()) {
          switch (Inst->getOpcode()) {
          case BinaryOperator::Add:
            // case BinaryOperator::FAdd:
            // Substitute with random add operation
            (*FuncAdd[Cryptoutils->getRange(NumberAddSubst)])(
                cast<BinaryOperator>(Inst));
            ++Add;
            break;
          case BinaryOperator::Sub:
            // case BinaryOperator::FSub:
            // Substitute with random sub operation
            (*FuncSub[Cryptoutils->getRange(NumberSubSubst)])(
                cast<BinaryOperator>(Inst));
            ++Sub;
            break;
          case BinaryOperator::Mul:
          case BinaryOperator::FMul:
            //++Mul;
            break;
          case BinaryOperator::UDiv:
          case BinaryOperator::SDiv:
          case BinaryOperator::FDiv:
            //++Div;
            break;
          case BinaryOperator::URem:
          case BinaryOperator::SRem:
          case BinaryOperator::FRem:
            //++Rem;
            break;
          case Instruction::Shl:
            //++Shi;
            break;
          case Instruction::LShr:
            //++Shi;
            break;
          case Instruction::AShr:
            //++Shi;
            break;
          case Instruction::And:
            (*FuncAnd[Cryptoutils->getRange(NumberAndSubst)])(
                cast<BinaryOperator>(Inst));
            ++And;
            break;
          case Instruction::Or:
            (*FuncOr[Cryptoutils->getRange(NumberOrSubst)])(
                cast<BinaryOperator>(Inst));
            ++Or;
            break;
          case Instruction::Xor:
            (*FuncXor[Cryptoutils->getRange(NumberXorSubst)])(
                cast<BinaryOperator>(Inst));
            ++Xor;
            break;
          default:
            break;
          } // End switch
        } // End isBinaryOp
      } // End for basickblock
    } // End for Function
  } while (--Times > 0); // for times
}

// Implementation of a = b - (-c)
void SubstitutionPass::addNeg(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  // Create sub
  if (Bo->getOpcode() == Instruction::Add) {
    Value *Op = IRB.CreateNeg(Bo->getOperand(1));
    Op = IRB.CreateSub(Bo->getOperand(0), Op);

    // Check signed wrap
    // op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    // op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

    Bo->replaceAllUsesWith(Op);
  } /* else {
     op = BinaryOperator::CreateFNeg(bo->getOperand(1), "", bo);
     op = BinaryOperator::Create(Instruction::FSub, bo->getOperand(0), op, "",
                                 bo);
   }*/
}

// Implementation of a = -(-b + (-c))
void SubstitutionPass::addDoubleNeg(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  if (Bo->getOpcode() == Instruction::Add) {
    Value *Op = IRB.CreateNeg(Bo->getOperand(0));
    Value *Op2 = IRB.CreateNeg(Bo->getOperand(1));
    Op = IRB.CreateAdd(Op, Op2);
    Op = IRB.CreateNeg(Op);

    Bo->replaceAllUsesWith(Op);

    // Check signed wrap
    // op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    // op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());
  } else {
    Value *Op = IRB.CreateFNeg(Bo->getOperand(0));
    Value *Op2 = IRB.CreateFNeg(Bo->getOperand(1));
    Op = IRB.CreateFAdd(Op, Op2);
    Op = IRB.CreateFNeg(Op);

    Bo->replaceAllUsesWith(Op);
  }
}

// Implementation of  r = rand (); a = b + r; a = a + c; a = a - r
void SubstitutionPass::addRand(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  if (Bo->getOpcode() == Instruction::Add) {
    Constant *Co = ConstantInt::get(Bo->getType(), Cryptoutils->getUint64T());
    Value *Op = IRB.CreateAdd(Bo->getOperand(0), Co);
    Op = IRB.CreateAdd(Op, Bo->getOperand(1));
    Op = IRB.CreateSub(Op, Co);

    // Check signed wrap
    // op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    // op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

    Bo->replaceAllUsesWith(Op);
  }
  /* else {
      Type *ty = bo->getType();
      ConstantFP *co =
  (ConstantFP*)ConstantFP::get(ty,(float)llvm::cryptoutils->get_uint64_t());
      op = BinaryOperator::Create(Instruction::FAdd,bo->getOperand(0),co,"",bo);
      op = BinaryOperator::Create(Instruction::FAdd,op,bo->getOperand(1),"",bo);
      op = BinaryOperator::Create(Instruction::FSub,op,co,"",bo);
  } */
}

// Implementation of r = rand (); a = b - r; a = a + b; a = a + r
void SubstitutionPass::addRand2(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  if (Bo->getOpcode() == Instruction::Add) {
    Constant *Co = ConstantInt::get(Bo->getType(), Cryptoutils->getUint64T());
    Value *Op = IRB.CreateSub(Bo->getOperand(0), Co);
    Op = IRB.CreateAdd(Op, Bo->getOperand(1));
    Op = IRB.CreateAdd(Op, Co);

    // Check signed wrap
    // op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    // op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

    Bo->replaceAllUsesWith(Op);
  }
  /* else {
      Type *ty = bo->getType();
      ConstantFP *co =
  (ConstantFP*)ConstantFP::get(ty,(float)llvm::cryptoutils->get_uint64_t());
      op = BinaryOperator::Create(Instruction::FAdd,bo->getOperand(0),co,"",bo);
      op = BinaryOperator::Create(Instruction::FAdd,op,bo->getOperand(1),"",bo);
      op = BinaryOperator::Create(Instruction::FSub,op,co,"",bo);
  } */
}

// Implementation of a = b + (-c)
void SubstitutionPass::subNeg(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);
  Value *Op;

  if (Bo->getOpcode() == Instruction::Sub) {
    Op = IRB.CreateNeg(Bo->getOperand(1));
    Op = IRB.CreateAdd(Bo->getOperand(0), Op);

    // Check signed wrap
    // op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    // op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());
  } else {
    Op = IRB.CreateFNeg(Bo->getOperand(1));
    Op = IRB.CreateFAdd(Bo->getOperand(0), Op);
  }

  Bo->replaceAllUsesWith(Op);
}

// Implementation of  r = rand (); a = b + r; a = a - c; a = a - r
void SubstitutionPass::subRand(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  if (Bo->getOpcode() == Instruction::Sub) {
    Constant *Co = ConstantInt::get(Bo->getType(), Cryptoutils->getUint64T());
    Value *Op = IRB.CreateAdd(Bo->getOperand(0), Co);
    Op = IRB.CreateSub(Op, Bo->getOperand(1));
    Op = IRB.CreateSub(Op, Co);

    // Check signed wrap
    // op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    // op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

    Bo->replaceAllUsesWith(Op);
  }
  /* else {
      Type *ty = bo->getType();
      ConstantFP *co =
  (ConstantFP*)ConstantFP::get(ty,(float)llvm::cryptoutils->get_uint64_t());
      op = BinaryOperator::Create(Instruction::FAdd,bo->getOperand(0),co,"",bo);
      op = BinaryOperator::Create(Instruction::FSub,op,bo->getOperand(1),"",bo);
      op = BinaryOperator::Create(Instruction::FSub,op,co,"",bo);
  } */
}

// Implementation of  r = rand (); a = b - r; a = a - c; a = a + r
void SubstitutionPass::subRand2(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  if (Bo->getOpcode() == Instruction::Sub) {
    Constant *Co = ConstantInt::get(Bo->getType(), Cryptoutils->getUint64T());
    Value *Op = IRB.CreateSub(Bo->getOperand(0), Co);
    Op = IRB.CreateSub(Op, Bo->getOperand(1));
    Op = IRB.CreateAdd(Op, Co);

    // Check signed wrap
    // op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    // op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

    Bo->replaceAllUsesWith(Op);
  }
  /* else {
      Type *ty = bo->getType();
      ConstantFP *co =
  (ConstantFP*)ConstantFP::get(ty,(float)llvm::cryptoutils->get_uint64_t());
      op = BinaryOperator::Create(Instruction::FSub,bo->getOperand(0),co,"",bo);
      op = BinaryOperator::Create(Instruction::FSub,op,bo->getOperand(1),"",bo);
      op = BinaryOperator::Create(Instruction::FAdd,op,co,"",bo);
  } */
}

// Implementation of a = b & c => a = (b^~c)& b
void SubstitutionPass::andSubstitution(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  // Create NOT on second operand => ~c
  Value *Op = IRB.CreateNot(Bo->getOperand(1));

  // Create XOR => (b^~c)
  Op = IRB.CreateXor(Bo->getOperand(0), Op);

  // Create AND => (b^~c) & b
  Op = IRB.CreateAnd(Op, Bo->getOperand(0));

  Bo->replaceAllUsesWith(Op);
}

// Implementation of a = a & b <=> ~(~a | ~b) & (r | ~r)
void SubstitutionPass::andSubstitutionRand(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  // r (Random number)
  Constant *Co = ConstantInt::get(Bo->getType(), Cryptoutils->getUint64T());

  // ~a
  Value *Op = IRB.CreateNot(Bo->getOperand(0));

  // ~b
  Value *Op1 = IRB.CreateNot(Bo->getOperand(1));

  // ~r
  Value *Opr = IRB.CreateNot(Co);

  // (~a | ~b)
  Value *Opa = IRB.CreateOr(Op, Op1);

  // (r | ~r)
  Opr = IRB.CreateOr(Co, Opr);

  // ~(~a | ~b)
  Op = IRB.CreateNot(Opa);

  // ~(~a | ~b) & (r | ~r)
  Op = IRB.CreateAnd(Op, Opr);

  // We replace all the old AND operators with the new one transformed
  Bo->replaceAllUsesWith(Op);
}

// Implementation of a = a | b =>
// a = (((~a & r) | (a & ~r)) ^ ((~b & r) | (b & ~r))) | (~(~a | ~b) & (r | ~r))
void SubstitutionPass::orSubstitutionRand(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  Constant *Co = ConstantInt::get(Bo->getType(), Cryptoutils->getUint64T());

  // ~a
  Value *Op = IRB.CreateNot(Bo->getOperand(0));

  // ~b
  Value *Op1 = IRB.CreateNot(Bo->getOperand(1));

  // ~r
  Value *Op2 = IRB.CreateNot(Co);

  // ~a & r
  Value *Op3 = IRB.CreateAnd(Op, Co);

  // a & ~r
  Value *Op4 = IRB.CreateAnd(Bo->getOperand(0), Op2);

  // ~b & r
  Value *Op5 = IRB.CreateAnd(Op1, Co);

  // b & ~r
  Value *Op6 = IRB.CreateAnd(Bo->getOperand(1), Op2);

  // (~a & r) | (a & ~r)
  Op3 = IRB.CreateOr(Op3, Op4);

  // (~b & r) | (b & ~r)
  Op4 = IRB.CreateOr(Op5, Op6);

  // ((~a & r) | (a & ~r)) ^ ((~b & r) | (b & ~r))
  Op5 = IRB.CreateXor(Op3, Op4);

  // ~a | ~b
  Op3 = IRB.CreateOr(Op, Op1);

  // ~(~a | ~b)
  Op3 = IRB.CreateNot(Op3);

  // r | ~r
  Op4 = IRB.CreateOr(Co, Op2);

  // ~(~a | ~b) & (r | ~r)
  Op4 = IRB.CreateAnd(Op3, Op4);

  // (((~a & r) | (a & ~r)) ^ ((~b & r) | (b & ~r))) | (~(~a | ~b) & (r | ~r))
  Op = IRB.CreateOr(Op5, Op4);

  Bo->replaceAllUsesWith(Op);
}

// Implementation of a = b | c => a = (b & c) | (b ^ c)
void SubstitutionPass::orSubstitution(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  // Creating first operand (b & c)
  Value *Op = IRB.CreateAnd(Bo->getOperand(0), Bo->getOperand(1));

  // Creating second operand (b ^ c)
  Value *Op1 = IRB.CreateXor(Bo->getOperand(0), Bo->getOperand(1));

  // final op
  Op = IRB.CreateOr(Op, Op1);

  Bo->replaceAllUsesWith(Op);
}

// Implementation of a = a ^ b => a = (~a & b) | (a & ~b)
void SubstitutionPass::xorSubstitution(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  // ~a
  Value *Op = IRB.CreateNot(Bo->getOperand(0));

  // ~a & b
  Op = IRB.CreateAnd(Bo->getOperand(1), Op);

  // ~b
  Value *Op1 = IRB.CreateNot(Bo->getOperand(1));

  // a & ~b
  Op1 = IRB.CreateAnd(Bo->getOperand(0), Op1);

  // (~a & b) | (a & ~b)
  Op = IRB.CreateOr(Op, Op1);

  Bo->replaceAllUsesWith(Op);
}

// implementation of a = a ^ b <=> (a ^ r) ^ (b ^ r) <=>
// ((~a & r) | (a & ~r)) ^ ((~b & r) | (b & ~r))
// note : r is a random number
void SubstitutionPass::xorSubstitutionRand(BinaryOperator *Bo) {
  IRBuilder<NoFolder> IRB(Bo);

  Constant *Co = ConstantInt::get(Bo->getType(), Cryptoutils->getUint64T());

  // ~a
  Value *Op = IRB.CreateNot(Bo->getOperand(0));

  // ~a & r
  Op = IRB.CreateAnd(Co, Op);

  // ~r
  Value *Opr = IRB.CreateNot(Co);

  // a & ~r
  Value *Op1 = IRB.CreateAnd(Bo->getOperand(0), Opr);

  // ~b
  Value *Op2 = IRB.CreateNot(Bo->getOperand(1));

  // ~b & r
  Op2 = IRB.CreateAnd(Op2, Co);

  // b & ~r
  Value *Op3 = IRB.CreateAnd(Bo->getOperand(1), Opr);

  // (~a & r) | (a & ~r)
  Op = IRB.CreateOr(Op, Op1);

  // (~b & r) | (b & ~r)
  Op1 = IRB.CreateOr(Op2, Op3);

  // ((~a & r) | (a & ~r)) ^ ((~b & r) | (b & ~r))
  Op = IRB.CreateXor(Op, Op1);

  Bo->replaceAllUsesWith(Op);
}
