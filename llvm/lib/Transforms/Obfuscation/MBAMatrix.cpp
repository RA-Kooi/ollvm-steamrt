#include "llvm/Transforms/Obfuscation/MBAMatrix.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>

using namespace llvm;

void MBAMatrix::simplify() {
  for (int I = 0; I < Line; I++) {
    int64_t Gcd = 0;

    for (int J = 0; J < Column; J++) {
      int64_t Val = std::abs(getElement(I, J));
      if (Val == 0)
        continue;

      if (Gcd == 0)
        Gcd = Val;
      else
        Gcd = gcd(Val, Gcd);
    }

    if (Gcd != 0 && Gcd != 1) {
      for (int J = 0; J < Column; J++)
        setElement(I, J, getElement(I, J) / Gcd);
    }
  }
}

int MBAMatrix::getLineFirstNonZero(int LineIdx) {
  for (int I = 0; I < Column; I++) {
    if (getElement(LineIdx, I) != 0)
      return I;
  }

  return Column;
}

void MBAMatrix::sortLine() {
  std::vector<uint64_t> Tbl;
  MBAMatrix TempMat(Line, Column);

  for (int I = 0; I < Line; I++) {
    for (int J = 0; J < Column; J++)
      TempMat.setElement(I, J, getElement(I, J));
  }

  for (int I = 0; I < Line; I++)
    Tbl.push_back(I);

  for (int I = 0; I < Line - 1; I++) {
    for (int J = 0; J < Line - I - 1; J++) {
      if (getLineFirstNonZero(Tbl[J]) <= getLineFirstNonZero(Tbl[J + 1]))
        continue;

      uint64_t Tmp;
      Tmp = Tbl[J];
      Tbl[J] = Tbl[J + 1];
      Tbl[J + 1] = Tmp;
    }
  }

  for (int I = 0; I < Line; I++) {
    for (int J = 0; J < Column; J++)
      setElement(I, J, TempMat.getElement(Tbl[I], J));
  }
}

void MBAMatrix::calcLine(int64_t Factor1, int Dst, int64_t Factor2, int Src,
                         bool IsAdd) {
  for (int I = 0; I < Column; I++) {
    int64_t Val = getElement(Dst, I) * Factor1;

    if (!IsAdd)
      Val -= getElement(Src, I) * Factor2;
    else
      Val += getElement(Src, I) * Factor2;

    setElement(Dst, I, Val);
  }
}

void MBAMatrix::lineElimate(int Dst, int Src, int ColumnIdx) {
  int64_t A = getElement(Dst, ColumnIdx), B = getElement(Src, ColumnIdx);

  if (A == 0 || B == 0)
    return;

  bool IsAdd = (A > 0) ^ (B > 0);
  int64_t FactorA = lcm(std::abs(A), std::abs(B)) / std::abs(A),
          FactorB = lcm(std::abs(A), std::abs(B)) / std::abs(B);

  calcLine(FactorA, Dst, FactorB, Src, IsAdd);
}

void MBAMatrix::gaussian() {
  sortLine();

  for (int I = 0; I < Line; I++) {
    sortLine();

    int Start = getLineFirstNonZero(I);
    if (Start == Column)
      break;

    for (int J = I + 1; J < Line; J++)
      lineElimate(J, I, Start);

    simplify();
    sortLine();
  }

  int Rank = getRank();
  for (int I = Rank - 1; I > 0; I--) {
    sortLine();

    int Z = getLineFirstNonZero(I);
    if (Z == Column)
      break;

    for (int J = I - 1; J >= 0; J--)
      lineElimate(J, I, Z);

    simplify();
    sortLine();
  }

  sortLine();
}

void MBAMatrix::setElement(int X, int Y, int64_t Val) {
  // assert(X < Line && Y < Column);
  Elements[X][Y] = Val;
}

int64_t MBAMatrix::getElement(int X, int Y) {
  // assert(X < Line && Y < Column);
  return Elements[X][Y];
}

void MBAMatrix::fromArray(int64_t *Arr) {
  for (int I = 0; I < Line; I++) {
    for (int J = 0; J < Column; J++)
      setElement(I, J, Arr[I * Column + J]);
  }
}

int MBAMatrix::getRank() {
  int Rank = 0;
  for (int I = 0; I < Line; I++) {
    if (getLineFirstNonZero(I) != Column)
      Rank++;
  }

  return Rank;
}

void MBAMatrix::solve(std::vector<int64_t> &Solution) {
  Solution.clear();

  for (int I = 0; I < Column; I++)
    Solution.push_back(0);

  gaussian();

  std::vector<int64_t> Result;

  std::vector<int> Vars;
  int Rank = getRank();
  for (int I = 0; I < Rank; I++)
    Vars.push_back(getLineFirstNonZero(I));

  std::map<int, int> FreeFactor;
  for (int I = Rank - 1; I >= 0; I--) {
    int VarIdx = getLineFirstNonZero(I);
    int64_t Factor = std::abs(getElement(I, VarIdx));
    // assert(Factor != 0);
    for (int J = VarIdx + 1; J < Column; J++) {
      int64_t Val = getElement(I, J);

      if (Val != 0) {
        if (FreeFactor.find(J) == FreeFactor.end())
          FreeFactor[J] = Factor;
        else
          FreeFactor[J] = lcm(FreeFactor[J], Factor);
      }
    }
  }

  for (auto Iter = FreeFactor.begin(); Iter != FreeFactor.end(); Iter++)
    Solution[Iter->first] = (rand() % 200 + 1) * Iter->second;

  for (int I = Rank - 1; I >= 0; I--) {
    int VarIdx = getLineFirstNonZero(I);
    int64_t Num = 0;

    for (int J = VarIdx + 1; J < Column; J++) {
      int C = getElement(I, J);

      if (C != 0)
        Num += C * Solution[J];
    }

    // assert(Num % getElement(i, VarIdx) == 0);
    Num /= getElement(I, VarIdx);
    Solution[VarIdx] = -Num;
  }
}
