#ifndef LLVM_UTILS_H
#define LLVM_UTILS_H

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <string>

namespace llvm {
// Read annotation values from llvm.global.annotations
std::string readAnnotate(Function *F);

// Determine whether obfuscation is enabled.
bool shouldObfuscate(bool Flag, llvm::Function *F,
                     std::string const &Attribute);

void fixStack(Function &F);

void fixBasicBlockConstantExpr(BasicBlock *BB);
void fixFunctionConstantExpr(Function *Func);

// LLVM-MSVC has this function, but the official LLVM version does not
// (LLVM: 17.0.6 | LLVM-MSVC: 3.2.6).
void lowerConstantExpr(Function &F);
} // namespace llvm

#endif // LLVM_UTILS_H
