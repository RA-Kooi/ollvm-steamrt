#ifndef OBFUSCATION_OBFUSCATIONOPTIONS_H
#define OBFUSCATION_OBFUSCATIONOPTIONS_H

#include "llvm/Support/YAMLParser.h"

#include <set>
#include <string>

namespace llvm {
struct ObfuscationOptions {
  explicit ObfuscationOptions(const Twine &FileName);
  ObfuscationOptions();

  bool skipFunction(const Twine &FName);
  void dump();

  bool EnableIndirectBr;
  bool EnableIndirectCall;
  bool EnableIndirectGV;
  bool EnableCFF;
  bool EnableCSE;
  bool HasFilter;

private:
  void init();
  void handleRoot(yaml::Node *N);
  bool parseOptions(const Twine &FileName);
  std::set<std::string> FunctionFilter;
};

} // namespace llvm
#endif
