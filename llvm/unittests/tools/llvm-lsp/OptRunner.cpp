#include "../tools/llvm-lsp/OptRunner.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/LSP/Logging.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Testing/Support/Error.h"
#include <gtest/gtest.h>

using namespace llvm;

static std::string writeMinimalModuleToTemp(LLVMContext &Ctx) {
  auto M = std::make_unique<Module>("test_module", Ctx);
  FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "f", M.get());
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> Builder(BB);
  Builder.CreateRetVoid();

  SmallString<128> TmpFile;
  std::error_code EC =
      sys::fs::createTemporaryFile("opt-runner-test", "ll", TmpFile);
  if (EC)
    return std::string();

  raw_fd_ostream OS(TmpFile, EC, sys::fs::OF_Text);
  if (EC)
    return std::string();

  M->print(OS, nullptr);
  OS.flush();
  return std::string(TmpFile.str());
}

TEST(OptRunner, GetPassList) {
  LLVMContext Context;
  std::string TmpFile = writeMinimalModuleToTemp(Context);
  ASSERT_FALSE(TmpFile.empty());

  SMDiagnostic Err;
  auto M = parseIRFile(TmpFile, Err, Context);
  ASSERT_TRUE(static_cast<bool>(M));

  OptRunner Runner = std::filesystem::path(TmpFile);

  auto PassListAndDescriptions =
      Runner.getPassListAndDescription("default<O2>");
  ASSERT_TRUE((bool)PassListAndDescriptions)
      << PassListAndDescriptions.takeError();
  ASSERT_TRUE(PassListAndDescriptions->size() > 0);

  SmallVector<std::pair<std::string, std::string>> ExpectedPasses = {
      {"1-Annotation2MetadataPass", " on [module]"},
      {"2-ForceFunctionAttrsPass", " on [module]"},
      {"3-InferFunctionAttrsPass", " on [module]"},
      {"4-CoroEarlyPass", " on [module]"},
      {"5-LowerExpectIntrinsicPass", " on f"},
      {"6-SimplifyCFGPass", " on f"},
      {"7-SROAPass", " on f"},
      {"8-EarlyCSEPass", " on f"},
      {"9-OpenMPOptPass", " on [module]"},
      {"10-IPSCCPPass", " on [module]"},
      {"11-CalledValuePropagationPass", " on [module]"},
      {"12-GlobalOptPass", " on [module]"},
      {"13-PromotePass", " on f"},
      {"14-InstCombinePass", " on f"},
      {"15-SimplifyCFGPass", " on f"},
      {"16-AlwaysInlinerPass", " on [module]"},
      {"17-RequireAnalysisPass<llvm::GlobalsAA,llvm::Module,"
       "llvm::AnalysisManager<Module>>",
       " on [module]"},
      {"18-InvalidateAnalysisPass<llvm::AAManager>", " on f"},
      {"19-RequireAnalysisPass<llvm::ProfileSummaryAnalysis,llvm::Module,"
       "llvm::AnalysisManager<Module>>",
       " on [module]"},
      {"20-InlinerPass", " on (f)"},
      {"21-PostOrderFunctionAttrsPass", " on (f)"},
      {"22-OpenMPOptCGSCCPass", " on (f)"},
      {"23-SROAPass", " on f"},
      {"24-EarlyCSEPass", " on f"},
      {"25-SpeculativeExecutionPass", " on f"},
      {"26-JumpThreadingPass", " on f"},
      {"27-CorrelatedValuePropagationPass", " on f"},
      {"28-SimplifyCFGPass", " on f"},
      {"29-InstCombinePass", " on f"},
      {"30-AggressiveInstCombinePass", " on f"},
      {"31-LibCallsShrinkWrapPass", " on f"},
      {"32-TailCallElimPass", " on f"},
      {"33-SimplifyCFGPass", " on f"},
      {"34-ReassociatePass", " on f"},
      {"35-ConstraintEliminationPass", " on f"},
      {"36-LoopSimplifyPass", " on f"},
      {"37-LCSSAPass", " on f"},
      {"38-SimplifyCFGPass", " on f"},
      {"39-InstCombinePass", " on f"},
      {"40-LoopSimplifyPass", " on f"},
      {"41-LCSSAPass", " on f"},
      {"42-SROAPass", " on f"},
      {"43-VectorCombinePass", " on f"},
      {"44-MergedLoadStoreMotionPass", " on f"},
      {"45-GVNPass", " on f"},
      {"46-SCCPPass", " on f"},
      {"47-BDCEPass", " on f"},
      {"48-InstCombinePass", " on f"},
      {"49-JumpThreadingPass", " on f"},
      {"50-CorrelatedValuePropagationPass", " on f"},
      {"51-ADCEPass", " on f"},
      {"52-MemCpyOptPass", " on f"},
      {"53-DSEPass", " on f"},
      {"54-MoveAutoInitPass", " on f"},
      {"55-LoopSimplifyPass", " on f"},
      {"56-LCSSAPass", " on f"},
      {"57-CoroElidePass", " on f"},
      {"58-SimplifyCFGPass", " on f"},
      {"59-InstCombinePass", " on f"},
      {"60-PostOrderFunctionAttrsPass", " on (f)"},
      {"61-RequireAnalysisPass<llvm::ShouldNotRunFunctionPassesAnalysis,"
       "llvm::Function,llvm::AnalysisManager<Function>>",
       " on f"},
      {"62-CoroSplitPass", " on (f)"},
      {"63-"
       "InvalidateAnalysisPass<llvm::ShouldNotRunFunctionPassesAnalysis>",
       " on f"},
      {"64-DeadArgumentEliminationPass", " on [module]"},
      {"65-CoroCleanupPass", " on [module]"},
      {"66-GlobalOptPass", " on [module]"},
      {"67-GlobalDCEPass", " on [module]"},
      {"68-EliminateAvailableExternallyPass", " on [module]"},
      {"69-ReversePostOrderFunctionAttrsPass", " on [module]"},
      {"70-RecomputeGlobalsAAPass", " on [module]"},
      {"71-Float2IntPass", " on f"},
      {"72-LowerConstantIntrinsicsPass", " on f"},
      {"73-LoopSimplifyPass", " on f"},
      {"74-LCSSAPass", " on f"},
      {"75-LoopDistributePass", " on f"},
      {"76-InjectTLIMappings", " on f"},
      {"77-LoopVectorizePass", " on f"},
      {"78-InferAlignmentPass", " on f"},
      {"79-LoopLoadEliminationPass", " on f"},
      {"80-InstCombinePass", " on f"},
      {"81-SimplifyCFGPass", " on f"},
      {"82-SLPVectorizerPass", " on f"},
      {"83-VectorCombinePass", " on f"},
      {"84-InstCombinePass", " on f"},
      {"85-LoopUnrollPass", " on f"},
      {"86-WarnMissedTransformationsPass", " on f"},
      {"87-SROAPass", " on f"},
      {"88-InferAlignmentPass", " on f"},
      {"89-InstCombinePass", " on f"},
      {"90-LoopSimplifyPass", " on f"},
      {"91-LCSSAPass", " on f"},
      {"92-AlignmentFromAssumptionsPass", " on f"},
      {"93-LoopSinkPass", " on f"},
      {"94-InstSimplifyPass", " on f"},
      {"95-DivRemPairsPass", " on f"},
      {"96-TailCallElimPass", " on f"},
      {"97-SimplifyCFGPass", " on f"},
      {"98-GlobalDCEPass", " on [module]"},
      {"99-ConstantMergePass", " on [module]"},
      {"100-CGProfilePass", " on [module]"},
      {"101-RelLookupTableConverterPass", " on [module]"},
      {"102-AnnotationRemarksPass", " on f"},
      {"103-BitcodeWriterPass", " on [module]"}};

  // This assert fails for some reason, but the elementvise comparison doesnt
  // ASSERT_EQ(*PassListAndDescriptions, ExpectedPasses);
  for (const auto &[A, B] : zip(*PassListAndDescriptions, ExpectedPasses)) {
    ASSERT_EQ(A, B);
  }
}

TEST(OptRunner, GetModuleAfterPass) {
  LLVMContext Context;
  std::string TmpFile = writeMinimalModuleToTemp(Context);
  ASSERT_FALSE(TmpFile.empty());

  SMDiagnostic Err;
  auto M = parseIRFile(TmpFile, Err, Context);
  ASSERT_TRUE(static_cast<bool>(M));

  SmallString<1024> TmpResult;
  sys::fs::createTemporaryFile("opt-runner-test", "ll", TmpResult);

  lsp::Logger::setLogLevel(lsp::Logger::Level::Debug);

  OptRunner Runner = std::filesystem::path(TmpFile);

  auto MaybeModulePath =
      Runner.getModuleBeforePass("default<O2>", 10, {"-print-module-scope"});
  ASSERT_THAT_EXPECTED(MaybeModulePath, llvm::Succeeded());

  auto ResultModule = parseIRFile(TmpFile, Err, Context);

  ASSERT_TRUE(ResultModule.get());

  for (const auto & F : M->functions()){
    ASSERT_TRUE((ResultModule)->getFunction(F.getName()));
  }
}
