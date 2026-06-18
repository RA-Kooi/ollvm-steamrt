#ifndef OBFUSCATOR_MBA_MATRIX_H
#define OBFUSCATOR_MBA_MATRIX_H

#include <cstdint>
#include <vector>

namespace llvm {
class MBAMatrix {
public:
  MBAMatrix(int Line, int Column) : Line(Line), Column(Column) {
    for (int I = 0; I < Line; I++) {
      std::vector<int64_t> LineNums;

      for (int J = 0; J < Column; J++)
        LineNums.push_back(0);

      Elements.push_back(LineNums);
    }
  }

  void fromArray(int64_t *Arr);

  int64_t getElement(int X, int Y);
  void setElement(int X, int Y, int64_t Val);

  int getRank();
  void solve(std::vector<int64_t> &Solution);

private:
  int Line, Column;
  std::vector<std::vector<int64_t>> Elements;

private:
  int64_t gcd(int64_t A, int64_t B) { return B == 0 ? A : gcd(B, A % B); }
  int64_t lcm(int64_t A, int64_t B) { return A * B / gcd(A, B); }

  void simplify();

  void gaussian();

  void sortLine();
  int getLineFirstNonZero(int LineIdx);
  void lineElimate(int Dst, int Src, int ColumnIdx);
  void calcLine(int64_t Factor1, int Dst, int64_t Factor2, int Src, bool IsAdd);
};

} // namespace llvm
#endif
