/*
 *  LLVM StringEncryption Pass
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
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"

#include <vector>

#define DEBUG_TYPE "split"

using namespace llvm;

STATISTIC(Split, "Basicblock splitted");

static cl::opt<int> SplitNum("split_num", cl::init(3),
                             cl::desc("Split <split_num> time(s) each BB"));

// It seems that NEW PM does not currently support this type of transmission.

PreservedAnalyses SplitBasicBlockPass::run(Function &F,
                                           FunctionAnalysisManager &AM) {
  Function *tmp = &F;
  if (toObfuscate(flag, tmp, "split")) {
    split(tmp);
    ++Split;
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

void SplitBasicBlockPass::split(Function *f) {
  std::vector<BasicBlock *> origBB;
  // Save all basic blocks to prevent splitting while iterating over new basic
  // blocks.
  for (Function::iterator I = f->begin(), IE = f->end(); I != IE; ++I) {
    origBB.push_back(&*I);
  }

  // All basic blocks of the traversal function.
  for (std::vector<BasicBlock *>::iterator I = origBB.begin(),
                                           IE = origBB.end();
       I != IE; ++I) {
    BasicBlock *curr = *I;

    // outs() << "\033[1;32mSplitNum : " << SplitNum << "\033[0m\n";
    // outs() << "\033[1;32mBasicBlock Size : " << curr->size() << "\033[0m\n";

    int splitN = SplitNum;

    // No need to divide a basic block into only one instruction
    // Indivisible basic blocks containing PHI instructions
    if (curr->size() < 2 || containsPHI(curr)) {
      /* outs() << "\033[0;33mThis BasicBlock is lower then two or had PIH "
                "Instruction!\033[0m\n"; */
      continue;
    }

    // Check `splitN` and the size of the basic block. If the number of splits
    // passed in is greater than or equal to the size of the basic block
    // itself, then modify the number of splits to the size of the basic block
    // minus one.
    if ((size_t)splitN >= curr->size()) {
      /* outs()
          << "\033[0;33mSplitNum is bigger then currBasicBlock's size\033[0m\n";

      outs() << "\033[0;33mSo SplitNum Now is BasicBlock's size -1 : "
             << (curr->size() - 1) << "\033[0m\n"; */

      splitN = curr->size() - 1;
    } else {
      // outs() << "\033[1;32msplitNum Now is " << splitN << "\033[0m\n";
    }

    // Generate splits point
    std::vector<int> test;
    for (unsigned i = 1; i < curr->size(); ++i) {
      test.push_back(i);
    }

    // Shuffle
    if (test.size() != 1) {
      shuffle(test);
      std::sort(test.begin(), test.begin() + splitN);
    }

    // Segment
    BasicBlock::iterator it = curr->begin();
    BasicBlock *toSplit = curr;
    int last = 0;
    for (int i = 0; i < splitN; ++i) {
      if (toSplit->size() < 2) {
        continue;
      }
      for (int j = 0; j < test[i] - last; ++j) {
        ++it;
      }
      last = test[i];
      toSplit = toSplit->splitBasicBlock(it, toSplit->getName() + ".split");
    }

    ++Split;
  }
}

bool SplitBasicBlockPass::containsPHI(BasicBlock *BB) {
  for (Instruction &I : *BB) {
    if (isa<PHINode>(&I)) {
      return true;
    }
  }
  return false;
}

void SplitBasicBlockPass::shuffle(std::vector<int> &vec) {
  int n = vec.size();
  for (int i = n - 1; i > 0; --i) {
    std::swap(vec[i], vec[cryptoutils->get_uint32_t() % (i + 1)]);
  }
}

SplitBasicBlockPass *llvm::createSplitBasicBlock(bool flag) {
  return new SplitBasicBlockPass(flag);
}
