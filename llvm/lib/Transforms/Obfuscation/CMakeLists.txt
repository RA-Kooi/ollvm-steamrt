add_llvm_component_library(LLVMObfuscation
  BogusControlFlow.cpp
  CryptoUtils.cpp
  Flattening.cpp
  IndirectBranch.cpp
  IndirectCall.cpp
  IndirectGlobalVariable.cpp
  IPObfuscationContext.cpp
  ObfuscationOptions.cpp
  SplitBasicBlock.cpp
  StringEncryption.cpp
  Substitution.cpp
  Utils.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/Obfuscation
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/Obfuscation/compat

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  Scalar
  Support
  TransformUtils)
