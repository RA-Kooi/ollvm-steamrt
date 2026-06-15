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
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
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
    errs() << "Substitution application number -sub_loop=x must be x > 0";
    return PreservedAnalyses::all();
  }

  // Do we obfuscate
  if (shouldObfuscate(SubEnabled, &F, "sub")) {
    substitute(&F);
    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

bool SubstitutionPass::substitute(Function *F) {
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
            (*FuncAdd[llvm::Cryptoutils->getRange(NumberAddSubst)])(
                cast<BinaryOperator>(Inst));
            ++Add;
            break;
          case BinaryOperator::Sub:
            // case BinaryOperator::FSub:
            // Substitute with random sub operation
            (*FuncSub[llvm::Cryptoutils->getRange(NumberSubSubst)])(
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
            (*FuncAnd[llvm::Cryptoutils->getRange(NumberAndSubst)])(
                cast<BinaryOperator>(Inst));
            ++And;
            break;
          case Instruction::Or:
            (*FuncOr[llvm::Cryptoutils->getRange(NumberOrSubst)])(
                cast<BinaryOperator>(Inst));
            ++Or;
            break;
          case Instruction::Xor:
            (*FuncXor[llvm::Cryptoutils->getRange(NumberXorSubst)])(
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
  return false;
}

// Implementation of a = b - (-c)
void SubstitutionPass::addNeg(BinaryOperator *Bo) {
  BinaryOperator *Op = NULL;

  // Create sub
  if (Bo->getOpcode() == Instruction::Add) {
    Op = BinaryOperator::CreateNeg(Bo->getOperand(1), "", Bo);
    Op =
        BinaryOperator::Create(Instruction::Sub, Bo->getOperand(0), Op, "", Bo);

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
  BinaryOperator *Op, *Op2 = NULL;
  UnaryOperator *Op3, *Op4;
  if (Bo->getOpcode() == Instruction::Add) {
    Op = BinaryOperator::CreateNeg(Bo->getOperand(0), "", Bo);
    Op2 = BinaryOperator::CreateNeg(Bo->getOperand(1), "", Bo);
    Op = BinaryOperator::Create(Instruction::Add, Op, Op2, "", Bo);
    Op = BinaryOperator::CreateNeg(Op, "", Bo);
    Bo->replaceAllUsesWith(Op);
    // Check signed wrap
    // op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    // op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());
  } else {
    Op3 = UnaryOperator::CreateFNeg(Bo->getOperand(0), "", Bo);
    Op4 = UnaryOperator::CreateFNeg(Bo->getOperand(1), "", Bo);
    Op = BinaryOperator::Create(Instruction::FAdd, Op3, Op4, "", Bo);
    Op3 = UnaryOperator::CreateFNeg(Op, "", Bo);
    Bo->replaceAllUsesWith(Op3);
  }
}

// Implementation of  r = rand (); a = b + r; a = a + c; a = a - r
void SubstitutionPass::addRand(BinaryOperator *Bo) {
  BinaryOperator *Op = NULL;

  if (Bo->getOpcode() == Instruction::Add) {
    Type *Ty = Bo->getType();
    ConstantInt *Co =
        (ConstantInt *)ConstantInt::get(Ty, llvm::Cryptoutils->getUint64T());
    Op =
        BinaryOperator::Create(Instruction::Add, Bo->getOperand(0), Co, "", Bo);
    Op =
        BinaryOperator::Create(Instruction::Add, Op, Bo->getOperand(1), "", Bo);
    Op = BinaryOperator::Create(Instruction::Sub, Op, Co, "", Bo);

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
  BinaryOperator *Op = NULL;

  if (Bo->getOpcode() == Instruction::Add) {
    Type *Ty = Bo->getType();
    ConstantInt *Co =
        (ConstantInt *)ConstantInt::get(Ty, llvm::Cryptoutils->getUint64T());
    Op =
        BinaryOperator::Create(Instruction::Sub, Bo->getOperand(0), Co, "", Bo);
    Op =
        BinaryOperator::Create(Instruction::Add, Op, Bo->getOperand(1), "", Bo);
    Op = BinaryOperator::Create(Instruction::Add, Op, Co, "", Bo);

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
  BinaryOperator *Op = NULL;
  if (Bo->getOpcode() == Instruction::Sub) {
    Op = BinaryOperator::CreateNeg(Bo->getOperand(1), "", Bo);
    Op =
        BinaryOperator::Create(Instruction::Add, Bo->getOperand(0), Op, "", Bo);
    // Check signed wrap
    // op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    // op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());
  } else {
    auto *Op1 = UnaryOperator::CreateFNeg(Bo->getOperand(1), "", Bo);
    Op = BinaryOperator::Create(Instruction::FAdd, Bo->getOperand(0), Op1, "",
                                Bo);
  }
  Bo->replaceAllUsesWith(Op);
}

// Implementation of  r = rand (); a = b + r; a = a - c; a = a - r
void SubstitutionPass::subRand(BinaryOperator *Bo) {
  BinaryOperator *Op = NULL;

  if (Bo->getOpcode() == Instruction::Sub) {
    Type *Ty = Bo->getType();
    ConstantInt *Co =
        (ConstantInt *)ConstantInt::get(Ty, llvm::Cryptoutils->getUint64T());
    Op =
        BinaryOperator::Create(Instruction::Add, Bo->getOperand(0), Co, "", Bo);
    Op =
        BinaryOperator::Create(Instruction::Sub, Op, Bo->getOperand(1), "", Bo);
    Op = BinaryOperator::Create(Instruction::Sub, Op, Co, "", Bo);

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
  BinaryOperator *Op = NULL;

  if (Bo->getOpcode() == Instruction::Sub) {
    Type *Ty = Bo->getType();
    ConstantInt *Co =
        (ConstantInt *)ConstantInt::get(Ty, llvm::Cryptoutils->getUint64T());
    Op =
        BinaryOperator::Create(Instruction::Sub, Bo->getOperand(0), Co, "", Bo);
    Op =
        BinaryOperator::Create(Instruction::Sub, Op, Bo->getOperand(1), "", Bo);
    Op = BinaryOperator::Create(Instruction::Add, Op, Co, "", Bo);

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
  BinaryOperator *Op = NULL;

  // Create NOT on second operand => ~c
  Op = BinaryOperator::CreateNot(Bo->getOperand(1), "", Bo);

  // Create XOR => (b^~c)
  BinaryOperator *Op1 =
      BinaryOperator::Create(Instruction::Xor, Bo->getOperand(0), Op, "", Bo);

  // Create AND => (b^~c) & b
  Op = BinaryOperator::Create(Instruction::And, Op1, Bo->getOperand(0), "", Bo);
  Bo->replaceAllUsesWith(Op);
}

// Implementation of a = a & b <=> ~(~a | ~b) & (r | ~r)
void SubstitutionPass::andSubstitutionRand(BinaryOperator *Bo) {
  // Copy of the BinaryOperator type to create the random number with the
  // same type of the operands
  Type *Ty = Bo->getType();

  // r (Random number)
  ConstantInt *Co =
      (ConstantInt *)ConstantInt::get(Ty, llvm::Cryptoutils->getUint64T());

  // ~a
  BinaryOperator *Op = BinaryOperator::CreateNot(Bo->getOperand(0), "", Bo);

  // ~b
  BinaryOperator *Op1 = BinaryOperator::CreateNot(Bo->getOperand(1), "", Bo);

  // ~r
  BinaryOperator *Opr = BinaryOperator::CreateNot(Co, "", Bo);

  // (~a | ~b)
  BinaryOperator *Opa =
      BinaryOperator::Create(Instruction::Or, Op, Op1, "", Bo);

  // (r | ~r)
  Opr = BinaryOperator::Create(Instruction::Or, Co, Opr, "", Bo);

  // ~(~a | ~b)
  Op = BinaryOperator::CreateNot(Opa, "", Bo);

  // ~(~a | ~b) & (r | ~r)
  Op = BinaryOperator::Create(Instruction::And, Op, Opr, "", Bo);

  // We replace all the old AND operators with the new one transformed
  Bo->replaceAllUsesWith(Op);
}

// Implementation of a = a | b =>
// a = (((~a & r) | (a & ~r)) ^ ((~b & r) | (b & ~r))) | (~(~a | ~b) & (r | ~r))
void SubstitutionPass::orSubstitutionRand(BinaryOperator *Bo) {

  Type *Ty = Bo->getType();
  ConstantInt *Co =
      (ConstantInt *)ConstantInt::get(Ty, llvm::Cryptoutils->getUint64T());

  // ~a
  BinaryOperator *Op = BinaryOperator::CreateNot(Bo->getOperand(0), "", Bo);

  // ~b
  BinaryOperator *Op1 = BinaryOperator::CreateNot(Bo->getOperand(1), "", Bo);

  // ~r
  BinaryOperator *Op2 = BinaryOperator::CreateNot(Co, "", Bo);

  // ~a & r
  BinaryOperator *Op3 =
      BinaryOperator::Create(Instruction::And, Op, Co, "", Bo);

  // a & ~r
  BinaryOperator *Op4 =
      BinaryOperator::Create(Instruction::And, Bo->getOperand(0), Op2, "", Bo);

  // ~b & r
  BinaryOperator *Op5 =
      BinaryOperator::Create(Instruction::And, Op1, Co, "", Bo);

  // b & ~r
  BinaryOperator *Op6 =
      BinaryOperator::Create(Instruction::And, Bo->getOperand(1), Op2, "", Bo);

  // (~a & r) | (a & ~r)
  Op3 = BinaryOperator::Create(Instruction::Or, Op3, Op4, "", Bo);

  // (~b & r) | (b & ~r)
  Op4 = BinaryOperator::Create(Instruction::Or, Op5, Op6, "", Bo);

  // ((~a & r) | (a & ~r)) ^ ((~b & r) | (b & ~r))
  Op5 = BinaryOperator::Create(Instruction::Xor, Op3, Op4, "", Bo);

  // ~a | ~b
  Op3 = BinaryOperator::Create(Instruction::Or, Op, Op1, "", Bo);

  // ~(~a | ~b)
  Op3 = BinaryOperator::CreateNot(Op3, "", Bo);

  // r | ~r
  Op4 = BinaryOperator::Create(Instruction::Or, Co, Op2, "", Bo);

  // ~(~a | ~b) & (r | ~r)
  Op4 = BinaryOperator::Create(Instruction::And, Op3, Op4, "", Bo);

  // (((~a & r) | (a & ~r)) ^ ((~b & r) | (b & ~r))) | (~(~a | ~b) & (r | ~r))
  Op = BinaryOperator::Create(Instruction::Or, Op5, Op4, "", Bo);
  Bo->replaceAllUsesWith(Op);
}

// Implementation of a = b | c => a = (b & c) | (b ^ c)
void SubstitutionPass::orSubstitution(BinaryOperator *Bo) {
  BinaryOperator *Op = NULL;

  // Creating first operand (b & c)
  Op = BinaryOperator::Create(Instruction::And, Bo->getOperand(0),
                              Bo->getOperand(1), "", Bo);

  // Creating second operand (b ^ c)
  BinaryOperator *Op1 = BinaryOperator::Create(
      Instruction::Xor, Bo->getOperand(0), Bo->getOperand(1), "", Bo);

  // final op
  Op = BinaryOperator::Create(Instruction::Or, Op, Op1, "", Bo);
  Bo->replaceAllUsesWith(Op);
}

// Implementation of a = a ^ b => a = (~a & b) | (a & ~b)
void SubstitutionPass::xorSubstitution(BinaryOperator *Bo) {
  BinaryOperator *Op = NULL;

  // Create NOT on first operand
  Op = BinaryOperator::CreateNot(Bo->getOperand(0), "", Bo); // ~a

  // Create AND
  Op = BinaryOperator::Create(Instruction::And, Bo->getOperand(1), Op, "",
                              Bo); // ~a & b

  // Create NOT on second operand
  BinaryOperator *Op1 =
      BinaryOperator::CreateNot(Bo->getOperand(1), "", Bo); // ~b

  // Create AND
  Op1 = BinaryOperator::Create(Instruction::And, Bo->getOperand(0), Op1, "",
                               Bo); // a & ~b

  // Create OR
  Op = BinaryOperator::Create(Instruction::Or, Op, Op1, "",
                              Bo); // (~a & b) | (a & ~b)
  Bo->replaceAllUsesWith(Op);
}

// implementation of a = a ^ b <=> (a ^ r) ^ (b ^ r) <=>
// ((~a & r) | (a & ~r)) ^ ((~b & r) | (b & ~r))
// note : r is a random number
void SubstitutionPass::xorSubstitutionRand(BinaryOperator *Bo) {
  BinaryOperator *Op = NULL;

  Type *Ty = Bo->getType();
  ConstantInt *Co =
      (ConstantInt *)ConstantInt::get(Ty, llvm::Cryptoutils->getUint64T());

  // ~a
  Op = BinaryOperator::CreateNot(Bo->getOperand(0), "", Bo);

  // ~a & r
  Op = BinaryOperator::Create(Instruction::And, Co, Op, "", Bo);

  // ~r
  BinaryOperator *Opr = BinaryOperator::CreateNot(Co, "", Bo);

  // a & ~r
  BinaryOperator *Op1 =
      BinaryOperator::Create(Instruction::And, Bo->getOperand(0), Opr, "", Bo);

  // ~b
  BinaryOperator *Op2 = BinaryOperator::CreateNot(Bo->getOperand(1), "", Bo);

  // ~b & r
  Op2 = BinaryOperator::Create(Instruction::And, Op2, Co, "", Bo);

  // b & ~r
  BinaryOperator *Op3 =
      BinaryOperator::Create(Instruction::And, Bo->getOperand(1), Opr, "", Bo);

  // (~a & r) | (a & ~r)
  Op = BinaryOperator::Create(Instruction::Or, Op, Op1, "", Bo);

  // (~b & r) | (b & ~r)
  Op1 = BinaryOperator::Create(Instruction::Or, Op2, Op3, "", Bo);

  // ((~a & r) | (a & ~r)) ^ ((~b & r) | (b & ~r))
  Op = BinaryOperator::Create(Instruction::Xor, Op, Op1, "", Bo);
  Bo->replaceAllUsesWith(Op);
}
