//===- SubstitutionIncludes.h - Substitution Obfuscation
// pass-------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains includes and defines for the substitution pass
//
//===----------------------------------------------------------------------===//

#ifndef _SUBSTITUTIONS_H_
#define _SUBSTITUTIONS_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/PassManager.h"

constexpr int NumberAddSubst = 4;
constexpr int NumberSubSubst = 3;
constexpr int NumberAndSubst = 2;
constexpr int NumberOrSubst = 2;
constexpr int NumberXorSubst = 2;

namespace llvm {
class SubstitutionPass : public PassInfoMixin<SubstitutionPass> {
public:
  using Predicate = void (*)(BinaryOperator *Bo);

  Predicate FuncAdd[NumberAddSubst];
  Predicate FuncSub[NumberSubSubst];
  Predicate FuncAnd[NumberAndSubst];
  Predicate FuncOr[NumberOrSubst];
  Predicate FuncXor[NumberXorSubst];

  SubstitutionPass() {
    FuncAdd[0] = &SubstitutionPass::addNeg;
    FuncAdd[1] = &SubstitutionPass::addDoubleNeg;
    FuncAdd[2] = &SubstitutionPass::addRand;
    FuncAdd[3] = &SubstitutionPass::addRand2;

    FuncSub[0] = &SubstitutionPass::subNeg;
    FuncSub[1] = &SubstitutionPass::subRand;
    FuncSub[2] = &SubstitutionPass::subRand2;

    FuncAnd[0] = &SubstitutionPass::andSubstitution;
    FuncAnd[1] = &SubstitutionPass::andSubstitutionRand;

    FuncOr[0] = &SubstitutionPass::orSubstitution;
    FuncOr[1] = &SubstitutionPass::orSubstitutionRand;

    FuncXor[0] = &SubstitutionPass::xorSubstitution;
    FuncXor[1] = &SubstitutionPass::xorSubstitutionRand;
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  bool substitute(Function *F);

  static void addNeg(BinaryOperator *Bo);
  static void addDoubleNeg(BinaryOperator *Bo);
  static void addRand(BinaryOperator *Bo);
  static void addRand2(BinaryOperator *Bo);

  static void subNeg(BinaryOperator *Bo);
  static void subRand(BinaryOperator *Bo);
  static void subRand2(BinaryOperator *Bo);

  static void andSubstitution(BinaryOperator *Bo);
  static void andSubstitutionRand(BinaryOperator *Bo);

  static void orSubstitution(BinaryOperator *Bo);
  static void orSubstitutionRand(BinaryOperator *Bo);

  static void xorSubstitution(BinaryOperator *Bo);
  static void xorSubstitutionRand(BinaryOperator *Bo);

  static bool isRequired() { return true; }
};
} // namespace llvm

#endif
