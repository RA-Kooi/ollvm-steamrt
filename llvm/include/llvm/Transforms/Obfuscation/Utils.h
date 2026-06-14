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

// FLA and BCF will throw errors when obfuscating partial functions,
// so full obfuscation cannot be enabled via the command line.
// Furthermore, Visual Studio seems unable to pass annotate to LLVM;
// it can only control it using function names.
extern bool obf_function_name_cmd;

namespace llvm {
// Read annotation values from llvm.global.annotations
std::string readAnnotate(Function *f);

// Determine whether obfuscation is enabled.
bool toObfuscate(bool flag, llvm::Function *f, std::string const &attribute);

void fixStack(Function &F);

void FixBasicBlockConstantExpr(BasicBlock *BB);
void FixFunctionConstantExpr(Function *Func);
std::string rand_str(int len);

// LLVM-MSVC has this function, but the official LLVM version does not
// (LLVM: 17.0.6 | LLVM-MSVC: 3.2.6).
void LowerConstantExpr(Function &F);
} // namespace llvm

#endif // LLVM_UTILS_H
