#include "llvm/Transforms/Obfuscation/LinearMBA.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/MBAMatrix.h"
#include "llvm/Transforms/Obfuscation/Utils.h"

#include <algorithm>
#include <random>
#include <vector>

#define DEBUG_TYPE "lmba"

using namespace llvm;

static cl::opt<bool> LMBAEnabled(
    "lmba",
    cl::init(false),
    cl::desc("Linear MBA: Replace bitwise operators with linear MBA expressions."));

#define DEFINE_TERM_FUNC(ID)                                                   \
  static Value *buildExpr##ID(IRBuilder<> &, Value *, Value *)
#define DEFINE_TERM_INFO(ID)                                                   \
  {{(TERM_##ID(0, 0)) & 1, (TERM_##ID(0, 1)) & 1, (TERM_##ID(1, 0)) & 1,       \
    (TERM_##ID(1, 1)) & 1},                                                    \
   buildExpr##ID}

#define TERM_0(x, y) x &y
DEFINE_TERM_FUNC(0);

#define TERM_1(x, y) x & ~y
DEFINE_TERM_FUNC(1);

#define TERM_2(x, y) x
DEFINE_TERM_FUNC(2);

#define TERM_3(x, y) ~x ^ y
DEFINE_TERM_FUNC(3);

#define TERM_4(x, y) y
DEFINE_TERM_FUNC(4);

#define TERM_5(x, y) x ^ y
DEFINE_TERM_FUNC(5);

#define TERM_6(x, y) x | y
DEFINE_TERM_FUNC(6);

#define TERM_7(x, y) ~(x | y)
DEFINE_TERM_FUNC(7);

#define TERM_8(x, y) ~(x ^ y)
DEFINE_TERM_FUNC(8);

#define TERM_9(x, y) ~y
DEFINE_TERM_FUNC(9);

#define TERM_10(x, y) x | ~y
DEFINE_TERM_FUNC(10);

#define TERM_11(x, y) ~x
DEFINE_TERM_FUNC(11);

#define TERM_12(x, y) ~x | y
DEFINE_TERM_FUNC(12);

#define TERM_13(x, y) ~(x & y)
DEFINE_TERM_FUNC(13);

#define TERM_TYPE_NUM 14

Value *buildExpr0(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateAnd(X, Y);
}

Value *buildExpr1(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateAnd(X, IRB.CreateNot(Y));
}

Value *buildExpr2(IRBuilder<> &IRB, Value *X, Value *Y) { return X; }

Value *buildExpr3(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateXor(IRB.CreateNot(X), Y);
}

Value *buildExpr4(IRBuilder<> &IRB, Value *X, Value *Y) { return Y; }

Value *buildExpr5(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateXor(X, Y);
}

Value *buildExpr6(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateOr(X, Y);
}

Value *buildExpr7(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateNot(IRB.CreateOr(X, Y));
}

Value *buildExpr8(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateNot(IRB.CreateXor(X, Y));
}

Value *buildExpr9(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateNot(Y);
}

Value *buildExpr10(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateOr(X, IRB.CreateNot(Y));
}

Value *buildExpr11(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateNot(X);
}

Value *buildExpr12(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateOr(IRB.CreateNot(X), Y);
}

Value *buildExpr13(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateNot(IRB.CreateAnd(X, Y));
}

static std::vector<BitwiseTerm> TermType = {
    DEFINE_TERM_INFO(0),  DEFINE_TERM_INFO(1),  DEFINE_TERM_INFO(2),
    DEFINE_TERM_INFO(3),  DEFINE_TERM_INFO(4),  DEFINE_TERM_INFO(5),
    DEFINE_TERM_INFO(6),  DEFINE_TERM_INFO(7),  DEFINE_TERM_INFO(8),
    DEFINE_TERM_INFO(9),  DEFINE_TERM_INFO(10), DEFINE_TERM_INFO(11),
    DEFINE_TERM_INFO(12), DEFINE_TERM_INFO(13)
};

static void process(Function &F);

PreservedAnalyses LinearMBA::run(Function &F, FunctionAnalysisManager &AM) {
  if (shouldObfuscate(LMBAEnabled, &F, "lmba")) {
    process(F);

    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

static void randomSelectTerms(std::vector<LinearMBATerm> &SelectedTerms) {
  std::vector<BitwiseTerm *> Available;
  for (BitwiseTerm &BT : TermType) {
    bool B = false;
    for (auto Iter = SelectedTerms.begin(); Iter != SelectedTerms.end();
         Iter++) {
      if (Iter->TermInfo == &BT) {
        B = true;
        break;
      }
    }

    if (!B)
      Available.push_back(&BT);
  }

  uint32_t Seed = Cryptoutils->getUint32T();
  std::shuffle(Available.begin(), Available.end(), std::mt19937{Seed});

  int Num = 5 - (int)SelectedTerms.size();
  for (int I = 0; I < Num; I++) {
    LinearMBATerm NewTerm = {Available[I], 0};
    SelectedTerms.push_back(NewTerm);
  }

  LinearMBATerm LastTerm = {nullptr, 0};
  SelectedTerms.push_back(LastTerm);
}

static void calcCoefficients(std::vector<LinearMBATerm> &SelectedTerms) {
  int Size = SelectedTerms.size();
  MBAMatrix Mat(4, Size);

  for (int I = 0; I < Size; I++) {
    for (int J = 0; J < 4; J++) {
      if (SelectedTerms[I].TermInfo != nullptr)
        Mat.setElement(J, I, SelectedTerms[I].TermInfo->TruthTable[J]);
      else
        Mat.setElement(J, I, 1);
    }
  }

  std::vector<int64_t> Result;
  Mat.solve(Result);
  for (int I = 0; I < Size; I++)
    SelectedTerms[I].Coefficient = Result[I];
}

static Value *buildLinearMBA(BinaryOperator *OriginalInsn,
                             std::vector<LinearMBATerm> &Terms) {
  IRBuilder<> IRB(OriginalInsn);

  uint32_t Seed = Cryptoutils->getUint32T();
  std::shuffle(Terms.begin(), Terms.end(), std::mt19937{Seed});

  bool NSW = OriginalInsn->hasNoSignedWrap();
  bool NUW = OriginalInsn->hasNoUnsignedWrap();
  Value *X = OriginalInsn->getOperand(0), *Y = OriginalInsn->getOperand(1);
  Value *Expr = nullptr;

  for (LinearMBATerm &Term : Terms) {
    if (Term.Coefficient == 0)
      continue;

    Value *TermVal = nullptr;

    if (Term.TermInfo != nullptr)
      TermVal = Term.TermInfo->Builder(IRB, X, Y);
    else
      TermVal = ConstantInt::getSigned(X->getType(), -1);

    Value *Mul = ConstantInt::getSigned(X->getType(), Term.Coefficient);
    TermVal = IRB.CreateMul(Mul, TermVal, "", NUW, NSW);

    if (Expr != nullptr)
      Expr = IRB.CreateAdd(Expr, TermVal, "", NUW, NSW);
    else
      Expr = TermVal;
  }
  return Expr;
}

static bool processAt(Instruction &Insn) {
  std::vector<LinearMBATerm> TermsSelected;
  Value *Result = nullptr;

  if (isa<BinaryOperator>(Insn)) {
    BinaryOperator &BI = cast<BinaryOperator>(Insn);

    switch (BI.getOpcode()) {
    case BinaryOperator::Add:
    {
      TermsSelected.push_back({&TermType[2], 0});
      TermsSelected.push_back({&TermType[4], 0});
      randomSelectTerms(TermsSelected);

      calcCoefficients(TermsSelected);
      TermsSelected[0].Coefficient += 1;
      TermsSelected[1].Coefficient += 1;

      Result = buildLinearMBA(&BI, TermsSelected);
    } break;
    case BinaryOperator::Sub:
    {
      TermsSelected.push_back({&TermType[2], 0});
      TermsSelected.push_back({&TermType[4], 0});
      randomSelectTerms(TermsSelected);

      calcCoefficients(TermsSelected);
      TermsSelected[0].Coefficient += 1;
      TermsSelected[1].Coefficient -= 1;

      Result = buildLinearMBA(&BI, TermsSelected);
    } break;
    case BinaryOperator::And:
    {
      TermsSelected.push_back({&TermType[0], 0});
      randomSelectTerms(TermsSelected);

      calcCoefficients(TermsSelected);
      TermsSelected[0].Coefficient += 1;

      Result = buildLinearMBA(&BI, TermsSelected);
    } break;
    case BinaryOperator::Or:
    {
      TermsSelected.push_back({&TermType[6], 0});
      randomSelectTerms(TermsSelected);

      calcCoefficients(TermsSelected);
      TermsSelected[0].Coefficient += 1;

      Result = buildLinearMBA(&BI, TermsSelected);
    } break;
    case BinaryOperator::Xor:
    {
      TermsSelected.push_back({&TermType[5], 0});
      randomSelectTerms(TermsSelected);

      calcCoefficients(TermsSelected);
      TermsSelected[0].Coefficient += 1;

      Result = buildLinearMBA(&BI, TermsSelected);
    } break;
    default:
      break;
    }
  }

  if (Result != nullptr) {
    Insn.replaceAllUsesWith(Result);
    return true;
  }

  return false;
}

static void process(Function &F) {
  std::vector<Instruction *> ToRemove;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (processAt(I))
        ToRemove.push_back(&I);
    }
  }

  for (Instruction *I : ToRemove)
    I->eraseFromParent();
}
