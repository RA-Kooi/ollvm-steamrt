#include "llvm/Transforms/Obfuscation/ObfuscationOptions.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

using namespace llvm;

void ObfuscationOptions::init() {
  EnableIndirectBr = false;
  EnableIndirectCall = false;
  EnableIndirectGV = false;
  EnableCFF = false;
  EnableCSE = false;
  HasFilter = false;
}

ObfuscationOptions::ObfuscationOptions() { init(); }
ObfuscationOptions::ObfuscationOptions(const Twine &FileName) {
  init();
  if (sys::fs::exists(FileName)) {
    parseOptions(FileName);
  }
}

static StringRef getNodeString(yaml::Node *N) {
  if (yaml::ScalarNode *Sn = dyn_cast<yaml::ScalarNode>(N)) {
    SmallString<32> Storage;
    StringRef Val = Sn->getValue(Storage);
    return Val;
  }
  return "";
}

static unsigned long getIntVal(yaml::Node *N) {
  return strtoul(getNodeString(N).str().c_str(), nullptr, 10);
}

static std::set<std::string> getStringList(yaml::Node *N) {
  std::set<std::string> Filter;
  if (yaml::SequenceNode *Sn = dyn_cast<yaml::SequenceNode>(N)) {
    for (yaml::SequenceNode::iterator I = Sn->begin(), E = Sn->end(); I != E;
         ++I) {
      Filter.insert(getNodeString(I).str());
    }
  }
  return Filter;
}

bool ObfuscationOptions::skipFunction(const Twine &FName) {
  if (FName.str().find("goron_") == std::string::npos) {
    return HasFilter && FunctionFilter.count(FName.str()) == 0;
  }
  return true;
}

void ObfuscationOptions::handleRoot(yaml::Node *N) {
  if (!N) {
    return;
  }
  if (yaml::MappingNode *Mn = dyn_cast<yaml::MappingNode>(N)) {
    for (yaml::MappingNode::iterator I = Mn->begin(), E = Mn->end(); I != E;
         ++I) {
      StringRef K = getNodeString(I->getKey());
      if (K == "IndirectBr") {
        EnableIndirectBr = static_cast<bool>(getIntVal(I->getValue()));
      } else if (K == "IndirectCall") {
        EnableIndirectCall = static_cast<bool>(getIntVal(I->getValue()));
      } else if (K == "IndirectGV") {
        EnableIndirectGV = static_cast<bool>(getIntVal(I->getValue()));
      } else if (K == "ControlFlowFlatten") {
        EnableCFF = static_cast<bool>(getIntVal(I->getValue()));
      } else if (K == "ConstantStringEncryption") {
        EnableCSE = static_cast<bool>(getIntVal(I->getValue()));
      } else if (K == "Filter") {
        HasFilter = true;
        FunctionFilter = getStringList(I->getValue());
      }
    }
  }
}

bool ObfuscationOptions::parseOptions(const Twine &FileName) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> BufOrErr =
      MemoryBuffer::getFileOrSTDIN(FileName);
  MemoryBuffer &Buf = *BufOrErr.get();
  llvm::SourceMgr Sm;
  yaml::Stream Stream(Buf.getBuffer(), Sm, false);
  for (yaml::document_iterator Di = Stream.begin(), De = Stream.end(); Di != De;
       ++Di) {
    yaml::Node *N = Di->getRoot();
    if (N)
      handleRoot(N);
    else
      break;
  }
  return true;
}

void ObfuscationOptions::dump() {
  dbgs() << "EnableIndirectBr: " << EnableIndirectBr << "\n"
         << "EnableIndirectCall: " << EnableIndirectCall << "\n"
         << "EnableIndirectGV: " << EnableIndirectGV << "\n"
         << "EnableCFF: " << EnableCFF << "\n"
         << "HasFilter:" << HasFilter << "\n";
}
