#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ModuleUtils.h" // For appendToCompilerUsed

#include <chrono>
#include <iomanip>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;

namespace {
// Simpler demotion that doesn't try to split edges
static void demoteValuesToMemory(Function &F) {
  BasicBlock &Entry = F.getEntryBlock();
  IRBuilder<> AllocaBuilder(&Entry, Entry.getFirstInsertionPt());
  DenseMap<Value *, AllocaInst *> ValueToAlloca;
  std::vector<PHINode *> PhisToRemove;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *PN = dyn_cast<PHINode>(&I)) {
        PhisToRemove.push_back(PN);
      }
    }
  }
  for (PHINode *PN : PhisToRemove) {
    AllocaInst *Alloca = AllocaBuilder.CreateAlloca(
        PN->getType(), nullptr, PN->getName() + ".phialloca");
    IRBuilder<> InitBuilder(Entry.getTerminator());
    InitBuilder.CreateStore(UndefValue::get(PN->getType()), Alloca);
    for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i) {
      Value *IncomingVal = PN->getIncomingValue(i);
      BasicBlock *IncomingBB = PN->getIncomingBlock(i);
      IRBuilder<> StoreBuilder(IncomingBB->getTerminator());
      StoreBuilder.CreateStore(IncomingVal, Alloca);
    }
    std::vector<Use *> UsesToReplace;
    for (Use &U : PN->uses()) {
      UsesToReplace.push_back(&U);
    }
    for (Use *U : UsesToReplace) {
      if (auto *UserInst = dyn_cast<Instruction>(U->getUser())) {
        IRBuilder<> LoadBuilder(PN->getParent(),
                                PN->getParent()->getFirstInsertionPt());
        LoadInst *Load = LoadBuilder.CreateLoad(PN->getType(), Alloca,
                                                PN->getName() + ".reload");
        U->set(Load);
      }
    }
    PN->eraseFromParent();
  }
  std::vector<Instruction *> ToDemote;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (I.isTerminator() || isa<AllocaInst>(I) || isa<PHINode>(I))
        continue;
      bool UsedOutside = false;
      for (User *U : I.users()) {
        if (auto *UI = dyn_cast<Instruction>(U)) {
          if (UI->getParent() != &BB) {
            UsedOutside = true;
            break;
          }
        }
      }
      if (UsedOutside) {
        ToDemote.push_back(&I);
      }
    }
  }
  for (Instruction *I : ToDemote) {
    AllocaInst *Alloca = AllocaBuilder.CreateAlloca(I->getType(), nullptr,
                                                    I->getName() + ".alloca");
    IRBuilder<> StoreBuilder(I);
    StoreBuilder.SetInsertPoint(I->getParent(), ++I->getIterator());
    StoreBuilder.CreateStore(I, Alloca);
    std::vector<Use *> UsesToReplace;
    for (Use &U : I->uses()) {
      UsesToReplace.push_back(&U);
    }
    for (Use *U : UsesToReplace) {
      if (auto *UserInst = dyn_cast<Instruction>(U->getUser())) {
        if (auto *SI = dyn_cast<StoreInst>(UserInst)) {
          if (SI->getPointerOperand() == Alloca)
            continue;
        }
        IRBuilder<> LoadBuilder(UserInst);
        LoadInst *Load = LoadBuilder.CreateLoad(I->getType(), Alloca,
                                                I->getName() + ".reload");
        U->set(Load);
      }
    }
  }
}

static Value *buildNextStateForTerm(IRBuilder<> &B, Instruction *T,
                                    DenseMap<BasicBlock *, unsigned> &Id,
                                    unsigned DefaultState = 0) {
  if (auto *Br = dyn_cast<BranchInst>(T)) {
    if (Br->isUnconditional()) {
      auto It = Id.find(Br->getSuccessor(0));
      if (It != Id.end()) {
        return B.getInt32(It->second);
      }
      return nullptr;
    }
    auto It1 = Id.find(Br->getSuccessor(0));
    auto It2 = Id.find(Br->getSuccessor(1));
    if (It1 != Id.end() && It2 != Id.end()) {
      Value *TState = B.getInt32(It1->second);
      Value *FState = B.getInt32(It2->second);
      return B.CreateSelect(Br->getCondition(), TState, FState, "cff.next");
    }
    return nullptr;
  }
  if (auto *Sw = dyn_cast<SwitchInst>(T)) {
    bool HasFlattenedSuccessor = false;
    auto DefaultIt = Id.find(Sw->getDefaultDest());
    if (DefaultIt != Id.end()) {
      HasFlattenedSuccessor = true;
    }
    for (auto &C : Sw->cases()) {
      if (Id.find(C.getCaseSuccessor()) != Id.end()) {
        HasFlattenedSuccessor = true;
        break;
      }
    }
    if (!HasFlattenedSuccessor) {
      return nullptr;
    }
    Value *Cond = Sw->getCondition();
    Value *NS = (DefaultIt != Id.end()) ? B.getInt32(DefaultIt->second)
                                        : B.getInt32(DefaultState);
    for (auto &C : Sw->cases()) {
      auto CaseIt = Id.find(C.getCaseSuccessor());
      if (CaseIt != Id.end()) {
        Value *Is = B.CreateICmpEQ(Cond, C.getCaseValue());
        Value *S = B.getInt32(CaseIt->second);
        NS = B.CreateSelect(Is, S, NS, "cff.case.select");
      }
    }
    return NS;
  }
  return nullptr;
}

static bool isSupportedTerminator(Instruction *T) {
  return isa<BranchInst>(T) || isa<SwitchInst>(T) || isa<ReturnInst>(T) ||
         isa<UnreachableInst>(T);
}

static bool hasUnsupportedControlFlow(Function &F) {
  for (BasicBlock &BB : F) {
    if (BB.isEHPad() || BB.isLandingPad()) {
      return true;
    }
    Instruction *T = BB.getTerminator();
    if (!isSupportedTerminator(T)) {
      return true;
    }
  }
  return false;
}

struct ControlFlowFlatteningPass
    : public PassInfoMixin<ControlFlowFlatteningPass> {

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    bool Changed = false;
    unsigned int flattenedFunctions = 0;
    unsigned int flattenedBlocks = 0;
    unsigned int skippedFunctions = 0;
    for (Function &F : M) {
      if (F.isDeclaration() || F.isIntrinsic() || F.size() < 2)
        continue;
      if (hasUnsupportedControlFlow(F)) {
        skippedFunctions++;
        continue;
      }
      unsigned blocksBefore = F.size();
      if (flattenFunction(F)) {
        Changed = true;
        flattenedFunctions++;
        flattenedBlocks += blocksBefore - 1;
      }
    }
    if (Changed || skippedFunctions > 0) {
      outs() << "CFF_METRICS:{\"flattenedFunctions\":" << flattenedFunctions
             << ",\"flattenedBlocks\":" << flattenedBlocks
             << ",\"skippedFunctions\":" << skippedFunctions << "}\n";
      if (Changed)
        return PreservedAnalyses::none();
    }
    return PreservedAnalyses::all();
  }

private:
  bool flattenFunction(Function &F) {
    if (F.isDeclaration() || F.isIntrinsic() || F.size() < 2)
      return false;
    if (hasUnsupportedControlFlow(F))
      return false;

    LLVMContext &Ctx = F.getContext();
    demoteValuesToMemory(F);
    BasicBlock *Entry = &F.getEntryBlock();
    SmallVector<BasicBlock *, 32> OriginalBlocks;
    for (BasicBlock &BB : F) {
      OriginalBlocks.push_back(&BB);
    }
    DenseMap<BasicBlock *, unsigned> BlockId;
    SmallVector<BasicBlock *, 32> FlattenTargets;
    unsigned NextId = 1;
    for (BasicBlock *BB : OriginalBlocks) {
      if (BB == Entry)
        continue;
      BlockId[BB] = NextId++;
      FlattenTargets.push_back(BB);
    }
    if (FlattenTargets.empty())
      return false;
    IRBuilder<> EntryBuilder(Ctx);
    EntryBuilder.SetInsertPoint(Entry, Entry->getFirstInsertionPt());
    AllocaInst *StateVar =
        EntryBuilder.CreateAlloca(Type::getInt32Ty(Ctx), nullptr, "cff.state");
    BasicBlock *Dispatcher = BasicBlock::Create(Ctx, "cff.dispatch", &F);
    BasicBlock *DefaultBlock = BasicBlock::Create(Ctx, "cff.default", &F);
    IRBuilder<> DefaultBuilder(DefaultBlock);
    DefaultBuilder.CreateUnreachable();
    Instruction *EntryTerm = Entry->getTerminator();
    {
      IRBuilder<> InitBuilder(EntryTerm);
      Value *InitialState =
          buildNextStateForTerm(InitBuilder, EntryTerm, BlockId);
      if (InitialState) {
        InitBuilder.CreateStore(InitialState, StateVar);
      } else {
        return false;
      }
    }
    EntryTerm->eraseFromParent();
    IRBuilder<> NewEntryBuilder(Entry);
    NewEntryBuilder.CreateBr(Dispatcher);
    IRBuilder<> DispatchBuilder(Dispatcher);
    Value *CurrentState =
        DispatchBuilder.CreateLoad(Type::getInt32Ty(Ctx), StateVar, "cff.cur");
    SwitchInst *DispatchSwitch = DispatchBuilder.CreateSwitch(
        CurrentState, DefaultBlock, FlattenTargets.size());
    for (BasicBlock *BB : FlattenTargets) {
      DispatchSwitch->addCase(DispatchBuilder.getInt32(BlockId[BB]), BB);
    }
    for (BasicBlock *BB : FlattenTargets) {
      Instruction *Term = BB->getTerminator();
      if (isa<ReturnInst>(Term) || isa<UnreachableInst>(Term))
        continue;
      IRBuilder<> TermBuilder(Term);
      Value *NextState = buildNextStateForTerm(TermBuilder, Term, BlockId);
      if (NextState) {
        TermBuilder.CreateStore(NextState, StateVar);
        TermBuilder.CreateBr(Dispatcher);
        Term->eraseFromParent();
      }
    }
    removeUnreachableBlocks(F);
    return true;
  }
};
} // namespace

std::string escapeJsonString(StringRef S) {
  std::string Escaped;
  for (char C : S) {
    if (C == '\\') {
      Escaped += "\\\\";
    } else if (C == '"') {
      Escaped += "\\\"";
    } else {
      Escaped += C;
    }
  }
  return Escaped;
}

std::vector<char> encryptString(StringRef S, uint8_t key) {
  std::vector<char> Encrypted(S.begin(), S.end());
  for (char &C : Encrypted) {
    C ^= key;
  }
  return Encrypted;
}

struct StringEncryptionPass : public PassInfoMixin<StringEncryptionPass> {
  bool EnableCFF;
  StringEncryptionPass(bool EnableCFF) : EnableCFF(EnableCFF) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    bool Changed = false;
    std::vector<GlobalVariable *> StringGlobalsToEncrypt;
    unsigned int encryptedStringsCount = 0;
    unsigned int originalStringDataSize = 0;
    unsigned int encryptedStringDataSize = 0;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(1, 255);
    uint8_t randomKey = distrib(gen);

    for (GlobalVariable &GV : M.globals()) {
      if (GV.isConstant() && GV.hasInitializer() &&
          GV.getInitializer()->getType()->isAggregateType()) {
        if (auto *CDA = dyn_cast<ConstantDataArray>(GV.getInitializer())) {
          if (CDA->isString() && (GV.getName().starts_with(".str.") ||
                                  GV.getName().starts_with(".str"))) {
            StringGlobalsToEncrypt.push_back(&GV);
          }
        }
      }
    }

    if (StringGlobalsToEncrypt.empty()) {
      generateReport(M.getSourceFileName(), "obfuscated.ll",
                     encryptedStringsCount, originalStringDataSize,
                     encryptedStringDataSize);
      return PreservedAnalyses::all();
    }

    FunctionCallee DecryptFunc = injectDecryptionStub(M, randomKey);
    LLVMContext &Ctx = M.getContext();
    PointerType *Int8PtrTy = PointerType::get(Ctx, 0);

    for (GlobalVariable *GV : StringGlobalsToEncrypt) {
      StringRef OriginalStringRef =
          cast<ConstantDataArray>(GV->getInitializer())->getAsString();
      if (OriginalStringRef.empty())
        continue;

      encryptedStringsCount++;
      originalStringDataSize += OriginalStringRef.size();
      std::vector<char> EncryptedBytes =
          encryptString(OriginalStringRef, randomKey);
      EncryptedBytes.back() = '\0' ^ randomKey;
      encryptedStringDataSize += EncryptedBytes.size();

      ArrayType *ArrTy =
          ArrayType::get(Type::getInt8Ty(Ctx), EncryptedBytes.size());
      Constant *EncryptedConst =
          ConstantDataArray::get(Ctx, ArrayRef<char>(EncryptedBytes));
      auto *EncryptedGV =
          new GlobalVariable(M, ArrTy, true, GlobalValue::PrivateLinkage,
                             EncryptedConst, GV->getName() + ".enc");
      appendToCompilerUsed(M, {EncryptedGV});

      std::vector<Use *> UsesToReplace;
      for (Use &U : GV->uses()) {
        UsesToReplace.push_back(&U);
      }
      for (Use *U : UsesToReplace) {
        auto *CurrentUser = U->getUser();
        Instruction *InsertionPoint = nullptr;
        if (auto *Inst = dyn_cast<Instruction>(CurrentUser)) {
          InsertionPoint = Inst;
        } else {
          continue;
        }
        IRBuilder<> Builder(InsertionPoint);
        Value *Zero = Builder.getInt64(0);
        Value *EncryptedBasePtr = Builder.CreateInBoundsGEP(
            ArrTy, EncryptedGV, {Zero, Zero}, "encryptedPtr");
        Value *EncryptedArgPtr = Builder.CreateBitCast(
            EncryptedBasePtr, Int8PtrTy, "encryptedPtrCast");
        Value *DecryptedStringAlloca =
            Builder.CreateAlloca(ArrTy, nullptr, GV->getName() + ".dec.alloca");
        Value *DecryptedAllocaPtr = Builder.CreateBitCast(
            DecryptedStringAlloca, Int8PtrTy, "decryptedAllocaPtrCast");
        Builder.CreateCall(DecryptFunc,
                           {DecryptedAllocaPtr, EncryptedArgPtr,
                            Builder.getInt32(EncryptedBytes.size())});
        U->set(DecryptedAllocaPtr);
        Changed = true;
      }
      GV->eraseFromParent();
    }

    generateReport(M.getSourceFileName(), "obfuscated.ll",
                   encryptedStringsCount, originalStringDataSize,
                   encryptedStringDataSize);
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  FunctionCallee injectDecryptionStub(Module &M, uint8_t key) {
    LLVMContext &Ctx = M.getContext();
    Type *Int8PtrTy = PointerType::get(Ctx, 0);
    Type *Int32Ty = Type::getInt32Ty(Ctx);
    Type *VoidTy = Type::getVoidTy(Ctx);
    FunctionType *DecryptFTy =
        FunctionType::get(VoidTy, {Int8PtrTy, Int8PtrTy, Int32Ty}, false);
    Function *DecryptF = M.getFunction("chakravyuha_decrypt_string");
    if (!DecryptF) {
      DecryptF = Function::Create(DecryptFTy, GlobalValue::PrivateLinkage,
                                  "chakravyuha_decrypt_string", M);
      DecryptF->setCallingConv(CallingConv::C);
      DecryptF->addFnAttr(Attribute::NoInline);
      DecryptF->addFnAttr(Attribute::NoUnwind);
      Function::arg_iterator ArgIt = DecryptF->arg_begin();
      Argument *DestPtr = ArgIt++;
      DestPtr->setName("dest_ptr");
      Argument *SrcPtr = ArgIt++;
      SrcPtr->setName("src_ptr");
      Argument *Length = ArgIt++;
      Length->setName("length");
      BasicBlock *EntryBB = BasicBlock::Create(Ctx, "entry", DecryptF);
      IRBuilder<> Builder(EntryBB);
      BasicBlock *LoopHeader = BasicBlock::Create(Ctx, "loop_header", DecryptF);
      BasicBlock *LoopBody = BasicBlock::Create(Ctx, "loop_body", DecryptF);
      BasicBlock *LoopExit = BasicBlock::Create(Ctx, "loop_exit", DecryptF);
      Builder.CreateBr(LoopHeader);
      Builder.SetInsertPoint(LoopHeader);
      PHINode *IndexPhi = Builder.CreatePHI(Int32Ty, 2, "index");
      IndexPhi->addIncoming(Builder.getInt32(0), EntryBB);
      Value *LoopCondition =
          Builder.CreateICmpSLT(IndexPhi, Length, "loop_cond");
      Builder.CreateCondBr(LoopCondition, LoopBody, LoopExit);
      Builder.SetInsertPoint(LoopBody);
      Value *SrcCharPtr = Builder.CreateGEP(Type::getInt8Ty(Ctx), SrcPtr,
                                            IndexPhi, "src_char_ptr");
      Value *LoadedByte =
          Builder.CreateLoad(Type::getInt8Ty(Ctx), SrcCharPtr, "loaded_byte");
      Value *DecryptedByte =
          Builder.CreateXor(LoadedByte, Builder.getInt8(key), "decrypted_byte");
      Value *DestCharPtr = Builder.CreateGEP(Type::getInt8Ty(Ctx), DestPtr,
                                             IndexPhi, "dest_char_ptr");
      Builder.CreateStore(DecryptedByte, DestCharPtr);
      Value *NextIndex =
          Builder.CreateAdd(IndexPhi, Builder.getInt32(1), "next_index");
      IndexPhi->addIncoming(NextIndex, LoopBody);
      Builder.CreateBr(LoopHeader);
      Builder.SetInsertPoint(LoopExit);
      Builder.CreateRetVoid();
    }
    return FunctionCallee(DecryptF->getFunctionType(), DecryptF);
  }

  void generateReport(StringRef inputFileName, StringRef outputFileName,
                      unsigned int encryptedStrings, unsigned int originalSize,
                      unsigned int encryptedSize) {
    std::string reportBuffer;
    raw_string_ostream S(reportBuffer);
    S << "{\n";
    S << "  \"inputFile\": \""
      << (inputFileName.empty() ? "source_code.c"
                                : escapeJsonString(inputFileName))
      << "\",\n";
    S << "  \"outputFile\": \"" << escapeJsonString(outputFileName) << "\",\n";
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    struct tm buf;
#ifdef _WIN32
    gmtime_s(&buf, &in_time_t);
#else
    gmtime_r(&in_time_t, &buf);
#endif
    char time_str[24];
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", &buf);
    S << "  \"timestamp\": \"" << time_str << "\",\n";
    S << "  \"inputParameters\": {\n";
    S << "    \"targetPlatform\": \"linux\",\n";
    S << "    \"enableStringEncryption\": true,\n";
    S << "    \"enableControlFlowFlattening\": "
      << (EnableCFF ? "true" : "false") << "\n";
    S << "  },\n";
    S << "  \"outputAttributes\": {\n";
    S << "    \"originalIRStringDataSize\": \"" << originalSize
      << " bytes\",\n";
    S << "    \"obfuscatedIRStringDataSize\": \"" << encryptedSize
      << " bytes\",\n";
    double sizeIncrease =
        (originalSize == 0)
            ? 0.0
            : (double)(encryptedSize - originalSize) / originalSize * 100.0;
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << sizeIncrease;
    S << "    \"stringDataSizeChange\": \"" << ss.str() << "%\"\n";
    S << "  },\n";
    S << "  \"obfuscationMetrics\": {\n";
    S << "    \"stringEncryption\": {\n";
    S << "      \"count\": " << encryptedStrings << ",\n";
    S << "      \"method\": \"XOR with dynamic per-run key\"\n";
    S << "    }\n";
    S << "  }\n";
    S << "}\n";
    outs() << reportBuffer;
  }
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChakravyuhaPasses", "v0.2",
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  bool cff = false;
                  if (Name == "chakravyuha-full") {
                    cff = true;
                  } else if (Name != "chakravyuha-str-only") {
                    return false;
                  }

                  if (cff) {
                    MPM.addPass(ControlFlowFlatteningPass());
                  }
                  MPM.addPass(StringEncryptionPass(cff));
                  return true;
                });
          }};
}
