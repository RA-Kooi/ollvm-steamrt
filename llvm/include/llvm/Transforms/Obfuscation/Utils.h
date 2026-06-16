#ifndef LLVM_UTILS_H
#define LLVM_UTILS_H

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <string>

#define INIT_CONTEXT(F) CONTEXT = &F.getContext()
#define TYPE_I32 Type::getInt32Ty(*CONTEXT)
#define CONST_I32(V) ConstantInt::get(TYPE_I32, V, false)
#define CONST(T, V) ConstantInt::get(T, V)

extern llvm::LLVMContext *CONTEXT;

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
