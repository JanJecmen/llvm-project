//===-- IRDocument.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_LSP_IRDOCUMENT_H
#define LLVM_TOOLS_LLVM_LSP_IRDOCUMENT_H

#include "OptRunner.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/AsmParser/AsmParserContext.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/LSP/Protocol.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"

#include <filesystem>
#include <memory>
#include <string>

namespace {

constexpr const char *IrLLFilename = "ir.ll";

class IRDocumentHelpers {
public:
  static std::optional<std::string>
  basicBlockIdFormatter(const llvm::BasicBlock *BB,
                        const llvm::AsmParserContext &ParserContext) {
    auto MaybeBBLoc = ParserContext.getBlockLocation(BB);
    if (MaybeBBLoc.has_value()) {
      auto Loc = *MaybeBBLoc;
      return llvm::formatv("range_{0}_{1}_{2}_{3}", Loc.Start.Line,
                           Loc.Start.Col, Loc.End.Line, Loc.End.Col);
    }
    return std::nullopt;
  };

  static std::optional<llvm::FileLocRange>
  basicBlockIdParser(std::string BBId) {
    unsigned StartLine, StartCol, EndLine, EndCol;
    auto [part0, rest0] = llvm::StringRef{BBId}.split('_');
    if (part0 != "range")
      return std::nullopt;
    auto [part1, rest1] = rest0.split('_');
    if (part1.getAsInteger(10, StartLine))
      return std::nullopt;
    auto [part2, rest2] = rest1.split('_');
    if (part2.getAsInteger(10, StartCol))
      return std::nullopt;
    auto [part3, rest3] = rest2.split('_');
    if (part3.getAsInteger(10, EndLine))
      return std::nullopt;
    if (rest3.contains('_') || rest3.getAsInteger(10, EndCol))
      return std::nullopt;
    if (part1.empty() || part2.empty() || part3.empty() || rest3.empty())
      return std::nullopt;
    return llvm::FileLocRange{llvm::FileLoc{StartLine, StartCol},
                              llvm::FileLoc{EndLine, EndCol}};
  }
};

} // namespace

namespace llvm {
// Tracks and Manages the Cache of all Artifacts for a given IR.
class IRArtifacts {
  const Module &IR;
  std::filesystem::path ArtifactsFolderPath;

  // FIXME: Can perhaps maintain a single list of only SVG/Dot files
  DenseMap<Function *, std::filesystem::path> DotFileList;
  DenseMap<Function *, std::filesystem::path> SVGFileList;
  DenseMap<unsigned, std::filesystem::path> IntermediateIRDirectories;

  // TODO: Add support to store locations of Intermediate IR file locations

public:
  IRArtifacts(const std::filesystem::path &Filepath, Module &M) : IR(M) {
    // Make Artifacts folder, if it does not exist
    lsp::Logger::info("Creating IRArtifacts Directory for {}",
                      Filepath.string());
    ArtifactsFolderPath =
        Filepath.parent_path() / ("Artifacts-" + Filepath.stem().string());
    if (!std::filesystem::exists(ArtifactsFolderPath)) {
      std::filesystem::create_directory(ArtifactsFolderPath);
      lsp::Logger::info("Finished creating IR Artifacts Directory {} for {}",
                        ArtifactsFolderPath.string(), Filepath.string());
    } else
      lsp::Logger::info("Directory {} already exists",
                        ArtifactsFolderPath.string());
  }

  void generateGraphs(const AsmParserContext &ParserContext) {
    for (auto &F : IR.getFunctionList())
      if (!F.isDeclaration())
        generateGraphsForFunc(F.getName(), ParserContext);
  }

  void generateGraphsForFunc(StringRef FuncName,
                             const AsmParserContext &ParserContext) {
    Function *F = IR.getFunction(FuncName);
    assert(F && "Function does not exist to generate Dot file");

    // Generate Dot file
    std::filesystem::path DotFilePath =
        ArtifactsFolderPath / std::filesystem::path(FuncName.str() + ".dot");
    if (!std::filesystem::exists(DotFilePath)) {
      PassBuilder PB;
      FunctionAnalysisManager FAM;
      PB.registerFunctionAnalyses(FAM);
      auto &BFI = FAM.getResult<BlockFrequencyAnalysis>(*F);
      auto &BPI = FAM.getResult<BranchProbabilityAnalysis>(*F);
      DOTFuncInfo DFI(
          F, &BFI, &BPI, getMaxFreq(*F, &BFI), [&](const BasicBlock *BB) {
            return IRDocumentHelpers::basicBlockIdFormatter(BB, ParserContext);
          });
      DFI.setHeatColors(true);
      DFI.setEdgeWeights(true);
      DFI.setRawEdgeWeights(false);
      // FIXME: I think this dumps something to the stdout (or stderr?) that in
      // any case gets
      //   sent to the client and shows in the trace log, eg. I see messages
      //   like this: "writing to the newly created file
      //   /remote-home/jjecmen/irviz-2.0/test/Artifacts-foo/main.dot" We should
      //   prevent that.
      WriteGraph(&DFI, FuncName, false, "CFG for " + FuncName.str(),
                 DotFilePath.string());
    }

    // Generate SVG file
    generateSVGFromDot(DotFilePath, F);

    DotFileList[F] = DotFilePath;
  }

  void addIntermediateIR(const std::filesystem::path &IRFile, unsigned PassNum,
                         StringRef PassName) {
    auto IRFolder =
        ArtifactsFolderPath / (std::to_string(PassNum) + "-" + PassName.str());
    if (!std::filesystem::exists(IRFolder))
      std::filesystem::create_directory(IRFolder);
    IntermediateIRDirectories[PassNum] = IRFolder;
    lsp::Logger::info("Created directory for intermediate IR artifacts!");

    auto IRFilepath = IRFolder / IrLLFilename;
    if (!std::filesystem::exists(IRFilepath)) {
      lsp::Logger::info("Copying IR file to intermediate IR: {} -> {}", IRFile,
                        IRFilepath.string());
      std::filesystem::copy_file(IRFile, IRFilepath);
      lsp::Logger::info("Finished copying IR file");
    } else {
      lsp::Logger::info("IR File path already exists: {}", IRFilepath.string());
    }
  }

  std::optional<std::filesystem::path> getIRBeforePassNumber(unsigned N) {
    if (!IntermediateIRDirectories.contains(N) ||
        !std::filesystem::exists(IntermediateIRDirectories[N] / IrLLFilename)) {
      lsp::Logger::info("Did not find IR!");
      return std::nullopt;
    }
    return IntermediateIRDirectories[N] / IrLLFilename;
  }

  std::optional<std::string> getDotFilePath(Function *F) {
    if (DotFileList.contains(F)) {
      return DotFileList[F].string();
    }
    return std::nullopt;
  }

  std::optional<lsp::URIForFile> getSVGFilePath(Function *F) {
    if (SVGFileList.contains(F)) {
      if (auto Ret = lsp::URIForFile::fromFile(SVGFileList[F].string()); Ret)
        return *Ret;
      return std::nullopt;
    }
    return std::nullopt;
  }

private:
  void generateSVGFromDot(std::filesystem::path Dotpath, Function *F) {
    std::filesystem::path SVGFilePath =
        std::filesystem::path(Dotpath).replace_extension(".svg");
    std::string Cmd = "dot -Tsvg '" + Dotpath.string() + "' -o '" +
                      SVGFilePath.string() + "'";
    lsp::Logger::info("Running command: {}", Cmd);
    int Result = std::system(Cmd.c_str());

    if (Result == 0) {

      // if (DotExitCode == 0) {
      lsp::Logger::info("SVG Generated : {}", SVGFilePath.string());
      SVGFileList[F] = SVGFilePath;
    } else
      lsp::Logger::error("Failed to generate SVG!");
  }
};

// LSP Server will use this class to query details about the IR file.
class IRDocument {
  LLVMContext C;
  std::unique_ptr<Module> ParsedModule;
  std::filesystem::path Filepath;

  std::unique_ptr<OptRunner> Optimizer;
  std::unique_ptr<IRArtifacts> IRA;
  std::optional<std::string> OpenError = std::nullopt;

public:
  IRDocument(const std::filesystem::path &PathToIRFile,
             std::optional<std::string> OptPath = std::nullopt)
      : Filepath(PathToIRFile) {
    lsp::Logger::debug("Trying to open {}", PathToIRFile);
    auto MaybeParsedModule =
        loadModuleFromIR(PathToIRFile.string(), C, ParserContext);
    if (!MaybeParsedModule) {
      std::string ErrMsg;
      raw_string_ostream OS(ErrMsg);
      logAllUnhandledErrors(MaybeParsedModule.takeError(), OS);
      lsp::Logger::error("Error while parsing IR: {}", ErrMsg);
      OpenError = ErrMsg;
      return;
    }
    ParsedModule = std::move(*MaybeParsedModule);
    IRA = std::make_unique<IRArtifacts>(Filepath, *ParsedModule);
    Optimizer = std::make_unique<OptRunner>(Filepath, OptPath);

    // Eagerly generate all CFG for all functions in the IRDocument.
    IRA->generateGraphs(ParserContext);
    lsp::Logger::info("Finished setting up IR Document: {}", PathToIRFile);
  }

  std::optional<std::string> open() { return OpenError; }

  // ---------------- APIs that the Language Server can use  -----------------

  std::string getNodeId(const BasicBlock *BB) {
    if (auto Id = IRDocumentHelpers::basicBlockIdFormatter(BB, ParserContext))
      return *Id;
    return "";
  }

  FileLocRange parseNodeId(std::string BBId) {
    if (auto FLR = IRDocumentHelpers::basicBlockIdParser(BBId))
      return *FLR;
    return FileLocRange{};
  }

  Function *getFirstFunction() {
    return &ParsedModule->getFunctionList().front();
  }

  std::optional<lsp::URIForFile> getPathForSVGFile(Function *F) {
    return IRA->getSVGFilePath(F);
  }

  auto &getFunctions() { return ParsedModule->getFunctionList(); }

  Function *getFunctionAtLocation(unsigned Line, unsigned Col) {
    FileLoc FL(Line, Col);
    return ParserContext.getFunctionAtLocation(FL);
  }

  BasicBlock *getBlockAtLocation(unsigned Line, unsigned Col) {
    FileLoc FL(Line, Col);
    return ParserContext.getBlockAtLocation(FL);
  }

  Instruction *getInstructionAtLocation(unsigned Line, unsigned Col) {
    FileLoc FL(Line, Col);
    lsp::Logger::debug("Geting instruction at location");
    auto R = ParserContext.getInstructionAtLocation(FL);
    lsp::Logger::debug("Got instruction at location");
    return R;
  }

  // This ↓ doesn't seem to be true
  // N is 1-Indexed here, but IRA expects 0-Indexed
  llvm::Expected<std::filesystem::path>
  getIRBeforePassNumber(const std::string &Pipeline, unsigned N,
                        ArrayRef<StringRef> AdditionalOptArgs = {}) {
    auto ExistingIR = IRA->getIRBeforePassNumber(N);
    if (ExistingIR) {
      lsp::Logger::info("Found Existing IR");
      return *ExistingIR;
    }
    auto PassNameResult = Optimizer->getPassName(Pipeline, N);
    if (!PassNameResult)
      return PassNameResult.takeError();
    auto PassName = PassNameResult.get();
    lsp::Logger::info("Found Pass name for pass number {} as {}",
                      std::to_string(N), PassName);

    auto IntermediateIR =
        Optimizer->getModuleBeforePass(Pipeline, N, AdditionalOptArgs);
    if (!IntermediateIR) {
      lsp::Logger::info("Error while getting intermediate IR");
      return IntermediateIR.takeError();
    }
    IRA->addIntermediateIR(*IntermediateIR, N, PassName);
    return *IRA->getIRBeforePassNumber(N);
  }

  // FIXME: We are doing some redundant work here in below functions, which can
  // be fused together.
  llvm::Expected<SmallVector<std::string, 256>>
  getPassList(const std::string &Pipeline,
              const std::optional<std::vector<std::string>> &AdditionalOptArgs =
                  std::nullopt) {
    SmallVector<std::string, 256> PassList;
    auto PassNameAndDescriptionListResult =
        Optimizer->getPassListAndDescription(Pipeline, AdditionalOptArgs);

    if (!PassNameAndDescriptionListResult) {
      lsp::Logger::info("Handling error in getPassList()");
      return PassNameAndDescriptionListResult.takeError();
    }

    for (auto &P : PassNameAndDescriptionListResult.get())
      PassList.push_back(P.first);

    return PassList;
  }
  llvm::Expected<SmallVector<std::string, 256>> getPassDescriptions(
      const std::string &Pipeline,
      const std::optional<std::vector<std::string>> &AdditionalOptArgs =
          std::nullopt) {
    SmallVector<std::string, 256> PassDesc;
    auto PassNameAndDescriptionListResult =
        Optimizer->getPassListAndDescription(Pipeline, AdditionalOptArgs);

    if (!PassNameAndDescriptionListResult)
      return PassNameAndDescriptionListResult.takeError();

    for (auto &P : PassNameAndDescriptionListResult.get())
      PassDesc.push_back(P.second);

    return PassDesc;
  }

  AsmParserContext ParserContext;

private:
  static llvm::Expected<std::unique_ptr<Module>>
  loadModuleFromIR(StringRef Filepath, LLVMContext &C,
                   AsmParserContext &ParserContext) {
    SMDiagnostic Err;
    // Try to parse as textual IR
    auto M = parseIRFile(Filepath, Err, C, {}, &ParserContext);
    if (!M) {
      // If parsing failed, print the error and return it
      lsp::Logger::error("Failed parsing IR file: {}", Err.getMessage().str());
      return llvm::createStringError(Err.getMessage().str());
    }
    return M;
  }
};

} // namespace llvm

#endif // LLVM_TOOLS_LLVM_LSP_IRDOCUMENT_H
