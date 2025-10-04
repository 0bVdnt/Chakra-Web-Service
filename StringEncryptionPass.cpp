#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <chrono>
#include <iomanip>
#include <random> // Required for generating random keys
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;

// Helper to escape backslashes for JSON
std::string escapeJsonString(StringRef S) {
  std::string Escaped;
  for (char C : S) {
    if (C == '\\') {
      Escaped += "\\\\";
    } else {
      Escaped += C;
    }
  }
  return Escaped;
}

// The encryptString function now takes the key as an argument
std::vector<char> encryptString(StringRef S, uint8_t key) {
  std::vector<char> Encrypted(S.begin(), S.end());
  for (char &C : Encrypted) {
    C ^= key;
  }
  return Encrypted;
}

struct StringEncryptionPass : public PassInfoMixin<StringEncryptionPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    bool Changed = false;
    std::vector<GlobalVariable *> StringGlobalsToEncrypt;
    unsigned int encryptedStringsCount = 0;
    unsigned int originalStringDataSize = 0;
    unsigned int encryptedStringDataSize = 0;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(1, 255); // Avoid 0, as it does nothing
    uint8_t randomKey = distrib(gen);

    for (GlobalVariable &GV : M.globals()) {
      if (GV.isConstant() && GV.hasInitializer() &&
          GV.getInitializer()->getType()->isAggregateType()) {
        if (ConstantDataArray *CDA =
                dyn_cast<ConstantDataArray>(GV.getInitializer())) {
          if (CDA->isString()) {
            if (GV.getName().starts_with(".str.") ||
                GV.getName().starts_with(".str")) {
              StringGlobalsToEncrypt.push_back(&GV);
            }
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

    // Pass the new random key to the decryption stub injector
    FunctionCallee DecryptFunc = injectDecryptionStub(M, randomKey);
    LLVMContext &Ctx = M.getContext();
    PointerType *Int8PtrTy = PointerType::get(Ctx, 0);

    for (GlobalVariable *GV : StringGlobalsToEncrypt) {
      StringRef OriginalStringRef =
          cast<ConstantDataArray>(GV->getInitializer())->getAsString();
      if (OriginalStringRef.empty()) continue;
      encryptedStringsCount++;
      originalStringDataSize += OriginalStringRef.size();

      // Pass the new random key to the encryption function
      std::vector<char> EncryptedBytes = encryptString(OriginalStringRef, randomKey);
      EncryptedBytes.back() = '\0' ^ randomKey;
      encryptedStringDataSize += EncryptedBytes.size();

      ArrayType *ArrTy = ArrayType::get(Type::getInt8Ty(Ctx), EncryptedBytes.size());
      Constant *EncryptedConst = ConstantDataArray::get(Ctx, ArrayRef<char>(EncryptedBytes));
      GlobalVariable *EncryptedGV = new GlobalVariable(M, ArrTy, true, GlobalValue::PrivateLinkage, EncryptedConst, GV->getName() + ".enc");
      appendToCompilerUsed(M, {EncryptedGV});
      std::vector<Use *> UsesToReplace;
      for (Use &U : GV->uses()) { UsesToReplace.push_back(&U); }
      for (Use *U : UsesToReplace) {
        User *CurrentUser = U->getUser();
        Instruction *InsertionPoint = nullptr;
        if (Instruction *Inst = dyn_cast<Instruction>(CurrentUser)) {
          InsertionPoint = Inst;
        } else { continue; }
        IRBuilder<> Builder(InsertionPoint);
        Value *Zero = Builder.getInt64(0);
        Value *EncryptedBasePtr = Builder.CreateInBoundsGEP(ArrTy, EncryptedGV, {Zero, Zero}, "encryptedPtr");
        Value *EncryptedArgPtr = Builder.CreateBitCast(EncryptedBasePtr, Int8PtrTy, "encryptedPtrCast");
        Value *DecryptedStringAlloca = Builder.CreateAlloca(ArrTy, nullptr, GV->getName() + ".dec.alloca");
        Value *DecryptedAllocaPtr = Builder.CreateBitCast(DecryptedStringAlloca, Int8PtrTy, "decryptedAllocaPtrCast");
        Builder.CreateCall(DecryptFunc, {DecryptedAllocaPtr, EncryptedArgPtr, Builder.getInt32(EncryptedBytes.size())});
        U->set(DecryptedAllocaPtr);
        Changed = true;
      }
      GV->eraseFromParent();
    }

    generateReport(M.getSourceFileName(), "obfuscated.ll", encryptedStringsCount, originalStringDataSize, encryptedStringDataSize);
    if (Changed) { return PreservedAnalyses::none(); }
    return PreservedAnalyses::all();
  }

  // The decryption stub injector now takes the key as an argument
  FunctionCallee injectDecryptionStub(Module &M, uint8_t key) {
    LLVMContext &Ctx = M.getContext();
    Type *Int8PtrTy = PointerType::get(Ctx, 0);
    Type *Int32Ty = Type::getInt32Ty(Ctx);
    Type *VoidTy = Type::getVoidTy(Ctx);
    FunctionType *DecryptFTy =
        FunctionType::get(VoidTy, {Int8PtrTy, Int8PtrTy, Int32Ty}, false);

    Function *DecryptF = M.getFunction("chakravyuha_decrypt_string");
    if (!DecryptF) {
      DecryptF = Function::Create(DecryptFTy, GlobalValue::PrivateLinkage, "chakravyuha_decrypt_string", M);
      DecryptF->setCallingConv(CallingConv::C);
      DecryptF->addFnAttr(Attribute::NoInline);
      DecryptF->addFnAttr(Attribute::NoUnwind);
      Function::arg_iterator ArgIt = DecryptF->arg_begin();
      Argument *DestPtr = ArgIt++; DestPtr->setName("dest_ptr");
      Argument *SrcPtr = ArgIt++; SrcPtr->setName("src_ptr");
      Argument *Length = ArgIt++; Length->setName("length");
      BasicBlock *EntryBB = BasicBlock::Create(Ctx, "entry", DecryptF);
      IRBuilder<> Builder(EntryBB);
      BasicBlock *LoopHeader = BasicBlock::Create(Ctx, "loop_header", DecryptF);
      BasicBlock *LoopBody = BasicBlock::Create(Ctx, "loop_body", DecryptF);
      BasicBlock *LoopExit = BasicBlock::Create(Ctx, "loop_exit", DecryptF);
      Builder.CreateBr(LoopHeader);
      Builder.SetInsertPoint(LoopHeader);
      PHINode *IndexPhi = Builder.CreatePHI(Int32Ty, 2, "index");
      IndexPhi->addIncoming(Builder.getInt32(0), EntryBB);
      Value *LoopCondition = Builder.CreateICmpSLT(IndexPhi, Length, "loop_cond");
      Builder.CreateCondBr(LoopCondition, LoopBody, LoopExit);
      Builder.SetInsertPoint(LoopBody);
      Value *SrcCharPtr = Builder.CreateGEP(Type::getInt8Ty(Ctx), SrcPtr, IndexPhi, "src_char_ptr");
      Value *LoadedByte = Builder.CreateLoad(Type::getInt8Ty(Ctx), SrcCharPtr, "loaded_byte");

      Value *DecryptedByte = Builder.CreateXor(LoadedByte, Builder.getInt8(key), "decrypted_byte");

      Value *DestCharPtr = Builder.CreateGEP(Type::getInt8Ty(Ctx), DestPtr, IndexPhi, "dest_char_ptr");
      Builder.CreateStore(DecryptedByte, DestCharPtr);
      Value *NextIndex = Builder.CreateAdd(IndexPhi, Builder.getInt32(1), "next_index");
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
    S << "  \"inputFile\": \"" << (inputFileName.empty() ? "<stdin>" : escapeJsonString(inputFileName)) << "\",\n";
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
    S << "    \"obfuscationLevel\": \"medium\",\n";
    #ifdef _WIN32
        S << "  \"targetPlatform\": \"Windows\",\n";
    #else
        S << "    \"targetPlatform\": \"linux\",\n";
    #endif
    S << "    \"enableStringEncryption\": true,\n";
    S << "    \"enableControlFlowFlattening\": false,\n";
    S << "    \"enableAntiDebug\": false\n";
    S << "  },\n";
    S << "  \"outputAttributes\": {\n";
    S << "    \"originalIRStringDataSize\": \"" << originalSize << " bytes\",\n";
    S << "    \"obfuscatedIRStringDataSize\": \"" << encryptedSize << " bytes\",\n";
    double sizeIncrease = (originalSize == 0) ? 0.0 : (double)(encryptedSize - originalSize) / originalSize * 100.0;
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << sizeIncrease;
    S << "    \"stringDataSizeChange\": \"" << ss.str() << "%\"\n";
    S << "  },\n";
    S << "  \"obfuscationMetrics\": {\n";
    S << "    \"cyclesCompleted\": 1,\n";
    S << "    \"passesRun\": [\"StringEncrypt\"],\n";
    S << "    \"stringEncryption\": {\n";
    S << "      \"count\": " << encryptedStrings << ",\n";
    // Report that the key is now dynamic.
    S << "      \"method\": \"XOR with dynamic per-run key\"\n";
    S << "    }\n";
    S << "  }\n";
    S << "}\n";
    outs() << reportBuffer;
  }
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChakravyuhaStringEncryptionPassPlugin",
          "v0.1", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "chakravyuha-string-encrypt") {
                    MPM.addPass(StringEncryptionPass());
                    return true;
                  }
                  return false;
                });
          }};
}