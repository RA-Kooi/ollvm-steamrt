#include "llvm/Transforms/Obfuscation/AntiAnalysisJump.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <iterator>
#include <vector>

#define DEBUG_TYPE "antianalysis"

using namespace llvm;

static cl::opt<bool> AajEnabled(
    "aaj",
    cl::init(false),
    cl::desc("Anti analysis jumps: Breaks IDA's disassembler by inserting "
             "single byte jumps followed by an instruction prefix."
             "See -aaj_prob"));

static cl::opt<int> AajProb(
    "aaj_prob",
    cl::init(20),
    cl::desc("Choose the probability [%] each basic block will have a jump "
             "inserted by the -aaj pass. -aaj_prob=x where x >= 0 <= 100."),
    cl::value_desc("probability"),
    cl::Optional);

static void applyAntiAnalysis(Function &F, DominatorTree &DT);

static bool isValidCandidateInstruction(Instruction &I);
static bool isValidCandidateOperand(Value *V);

PreservedAnalyses AntiAnalysisJumpPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (!shouldObfuscate(AajEnabled, &F, "aap"))
    return PreservedAnalyses::all();

  if (AajProb < 0 || AajProb > 100) {
    errs() << "Error: -aap_prob=x x must be >= 0 <= 100!\n";

    return PreservedAnalyses::all();
  }

  Triple TargetTriple(F.getParent()->getTargetTriple());

  if(!TargetTriple.isX86()) {
    errs() << "[AntiAnalysisJumps] Warning: Only X86(-64) is supported "
      "for this pass.\n";

    return PreservedAnalyses::all();
  }

  DominatorTree &DT = AM.getResult<DominatorTreeAnalysis>(F);

  if (AajProb == 0)
    return PreservedAnalyses::all();

  applyAntiAnalysis(F, DT);

  return PreservedAnalyses::none();
}

static void applyAntiAnalysis(Function &F, DominatorTree &DT) {
  std::vector<BasicBlock*> OrigBlocks;

  auto IsExtendedLandingPad = [](BasicBlock &BB) -> bool {
    for (PHINode &Phi : BB.phis()) {
      for (auto &Inc : Phi.incoming_values()) {
        auto *I = dyn_cast_or_null<Instruction>(&Inc);
        if(!I)
          continue;

        if (I->getParent()->isLandingPad())
          return true;
      }
    }

    return false;
  };

  for (BasicBlock &BB : F) {
    if (BB.isLandingPad() || isa<ResumeInst>(BB.getTerminator()))
      continue;

    if (IsExtendedLandingPad(BB))
      continue;

    OrigBlocks.push_back(&BB);
  }

  for (BasicBlock *BB : OrigBlocks) {
    int32_t Prob = Cryptoutils->getRange(101);
    if (Prob > AajProb)
      continue;

    auto Begin = BB->getFirstNonPHIIt();
    // NOTE(Rafaël): -1 to ignore the terminator
    size_t InstCount = std::distance(Begin, BB->end()) - 1;

    if (InstCount == 0)
      continue;

    Prob = Cryptoutils->getRange(InstCount + 1);
    auto It = Begin;
    std::advance(It, Prob);

    std::vector<Value*> Inputs = findUsableValues(
        *It,
        isValidCandidateInstruction,
        isValidCandidateOperand,
        DT,
        1);

    if (Inputs.empty())
      continue;

    Value *Input = Inputs[0];

    BasicBlock *Split = SplitBlock(BB, It, &DT);
    BasicBlock *Bogus = BasicBlock::Create(BB->getContext(), "Bogus", &F, Split);

    DT.addNewBlock(Bogus, BB);

    BB->getTerminator()->eraseFromParent();
    IRBuilder<> IRB(BB);

    Input = IRB.CreateXor(Input, Input);
    Constant *Zero = ConstantInt::get(Input->getType(), 0);
    Value *Cond = IRB.CreateICmpEQ(Input, Zero);

    IRB.CreateCondBr(Cond, Split, Bogus);

    IRB.SetInsertPoint(Bogus);

    bool Prefix = Cryptoutils->getUint8T() & 1;

    InlineAsm *IA = InlineAsm::get(
        FunctionType::get(IRB.getVoidTy(), false),
        Prefix ? ".byte 0x0F" : ".byte 0x68",
        "",
        true);

    IRB.CreateCall(IA);

    IRB.CreateBr(Split);
  }
}

static bool isValidCandidateInstruction(Instruction &I) {
  if (isa<GetElementPtrInst>(&I))
    return false;
  if (isa<SwitchInst>(&I))
    return false;
  if (isa<CallInst>(&I))
    return false;

  return true;
}

static bool isValidCandidateOperand(Value *V) {
  if (isa<Constant>(V))
    return false;

  if (V->getType()->isIntegerTy()) {
    Type *VType = V->getType();
    if (VType->getIntegerBitWidth() == 1)
      return false;

    return true;
  }

  return false;
}
