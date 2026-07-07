/*
 *  LLVM SplitBasicBlock Pass
    Copyright (C) 2017 Zhang(https://github.com/Naville/)
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
#include "llvm/Transforms/Obfuscation/SplitBasicBlock.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"

#include <vector>

#define DEBUG_TYPE "split"

using namespace llvm;

STATISTIC(Split, "Basicblock splitted");

static cl::opt<bool>
    SplitEnabled("split", cl::init(false),
                 cl::desc("SplitBasicBlock: split_num=3(init)"));

static cl::opt<int> SplitNum("split_num", cl::init(3),
                             cl::desc("Split <split_num> time(s) each BB"));

// It seems that NEW PM does not currently support this type of transmission.

static void split(Function *F);
static bool containsPHI(BasicBlock *BB);
static void shuffle(std::vector<int> &Vec);

PreservedAnalyses SplitBasicBlockPass::run(Function &F,
                                           FunctionAnalysisManager &AM) {
  if (shouldObfuscate(SplitEnabled, &F, "split")) {
    ::split(&F);
    ++Split;
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

static void split(Function *F) {
  std::vector<BasicBlock *> OrigBb;
  // Save all basic blocks to prevent splitting while iterating over new basic
  // blocks.
  for (Function::iterator I = F->begin(), IE = F->end(); I != IE; ++I) {
    OrigBb.push_back(&*I);
  }

  // All basic blocks of the traversal function.
  for (std::vector<BasicBlock *>::iterator I = OrigBb.begin(),
                                           IE = OrigBb.end();
       I != IE; ++I) {
    BasicBlock *Curr = *I;

    // outs() << "\033[1;32mSplitNum : " << SplitNum << "\033[0m\n";
    // outs() << "\033[1;32mBasicBlock Size : " << curr->size() << "\033[0m\n";

    int SplitN = SplitNum;

    // No need to divide a basic block into only one instruction
    // Indivisible basic blocks containing PHI instructions
    if (Curr->size() < 2 || containsPHI(Curr)) {
      /* outs() << "\033[0;33mThis BasicBlock is lower then two or had PIH "
                "Instruction!\033[0m\n"; */
      continue;
    }

    // Check `splitN` and the size of the basic block. If the number of splits
    // passed in is greater than or equal to the size of the basic block
    // itself, then modify the number of splits to the size of the basic block
    // minus one.
    if ((size_t)SplitN >= Curr->size()) {
      /* outs()
          << "\033[0;33mSplitNum is bigger then currBasicBlock's size\033[0m\n";

      outs() << "\033[0;33mSo SplitNum Now is BasicBlock's size -1 : "
             << (curr->size() - 1) << "\033[0m\n"; */

      SplitN = Curr->size() - 1;
    } else {
      // outs() << "\033[1;32msplitNum Now is " << splitN << "\033[0m\n";
    }

    // Generate splits point
    std::vector<int> Test;
    for (unsigned I = 1; I < Curr->size(); ++I) {
      Test.push_back(I);
    }

    // Shuffle
    if (Test.size() != 1) {
      shuffle(Test);
      std::sort(Test.begin(), Test.begin() + SplitN);
    }

    // Segment
    BasicBlock::iterator It = Curr->begin();
    BasicBlock *ToSplit = Curr;
    int Last = 0;
    for (int I = 0; I < SplitN; ++I) {
      if (ToSplit->size() < 2) {
        continue;
      }
      for (int J = 0; J < Test[I] - Last; ++J) {
        ++It;
      }
      Last = Test[I];
      ToSplit = ToSplit->splitBasicBlock(It, ToSplit->getName() + ".split");
    }

    ++Split;
  }
}

static bool containsPHI(BasicBlock *BB) {
  for (Instruction &I : *BB) {
    if (isa<PHINode>(&I)) {
      return true;
    }
  }
  return false;
}

static void shuffle(std::vector<int> &Vec) {
  int N = Vec.size();
  for (int I = N - 1; I > 0; --I) {
    std::swap(Vec[I], Vec[Cryptoutils->getUint32T() % (I + 1)]);
  }
}
