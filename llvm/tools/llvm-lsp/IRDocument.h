//===-- IRDocument.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_LSP_IRDOCUMENT_H
#define LLVM_TOOLS_LLVM_LSP_IRDOCUMENT_H

#include "Logger.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/AsmParser/AsmParserContext.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include "OptRunner.h"
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace {

class IRDocumentHelpers {
public:
  static std::optional<std::string>
  basicBlockIdFormatter(const llvm::BasicBlock *BB,
                        const llvm::AsmParserContext &ParserContext) {
    return ParserContext.getBlockLocation(BB).transform([](const auto &Loc) {
      return llvm::formatv("range_{0}_{1}_{2}_{3}", Loc.Start.Line,
                           Loc.Start.Col, Loc.End.Line, Loc.End.Col);
    });
  }

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
  Logger &LoggerObj;
  const Module &IR;
  std::filesystem::path ArtifactsFolderPath;

  // FIXME: Can perhaps maintain a single list of only SVG/Dot files
  DenseMap<Function *, std::filesystem::path> DotFileList;
  DenseMap<Function *, std::filesystem::path> SVGFileList;
  DenseMap<unsigned, std::filesystem::path> IntermediateIRDirectories;

  // TODO: Add support to store locations of Intermediate IR file locations

public:
  IRArtifacts(StringRef Filepath, Logger &LO, Module &M)
      : LoggerObj(LO), IR(M) {
    // Make Artifacts folder, if it does not exist
    LoggerObj.log("Creating IRArtifacts Directory for " + Filepath.str());
    std::filesystem::path FilepathObj(Filepath.str());
    ArtifactsFolderPath = FilepathObj.parent_path().string() + "/Artifacts-" +
                          FilepathObj.stem().string();
    if (!std::filesystem::exists(ArtifactsFolderPath)) {
      std::filesystem::create_directory(ArtifactsFolderPath);
      LoggerObj.log("Finished creating IR Artifacts Directory " +
                    ArtifactsFolderPath.string() + " for " + Filepath.str());
    } else
      LoggerObj.log("Directory " + ArtifactsFolderPath.string() +
                    " already exists");
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

  void addIntermediateIR(Module &M, unsigned PassNum, StringRef PassName) {
    auto IRFolder =
        ArtifactsFolderPath / (std::to_string(PassNum) + "-" + PassName.str());
    if (!std::filesystem::exists(IRFolder))
      std::filesystem::create_directory(IRFolder);
    IntermediateIRDirectories[PassNum] = IRFolder;
    LoggerObj.log("Created directory for intermediate IR artifacts!");

    auto IRFilepath = IRFolder / "ir.ll";
    if (!std::filesystem::exists(IRFilepath)) {
      LoggerObj.log("Creating new file to store Intermediate IR: " +
                    IRFilepath.string());
      std::error_code EC;
      raw_fd_ostream OutFile(IRFilepath.string(), EC, sys::fs::OF_None);
      M.print(OutFile, nullptr);
      OutFile.flush();
      OutFile.close();
      LoggerObj.log("Finished creating IR file");
    } else {
      LoggerObj.log("IR File path already exists: " + IRFilepath.string());
    }
  }

  std::optional<std::string> getIRAfterPassNumber(unsigned N) {
    if (!IntermediateIRDirectories.contains(N)) {
      LoggerObj.log("Did not find IR Directory!");
      return std::nullopt;
    }
    return IntermediateIRDirectories[N].string() + "/ir.ll";
  }

  std::optional<std::string> getDotFilePath(Function *F) {
    if (DotFileList.contains(F)) {
      return DotFileList[F].string();
    }
    return std::nullopt;
  }

  std::optional<std::string> getSVGFilePath(Function *F) {
    if (SVGFileList.contains(F)) {
      return SVGFileList[F].string();
    }
    return std::nullopt;
  }

private:
  void generateSVGFromDot(std::filesystem::path Dotpath, Function *F) {
    std::filesystem::path SVGFilePath =
        std::filesystem::path(Dotpath).replace_extension(".svg");
    std::string Cmd =
        "dot -Tsvg " + Dotpath.string() + " -o " + SVGFilePath.string();
    LoggerObj.log("Running command: " + Cmd);
    int Result = std::system(Cmd.c_str());

    if (Result == 0) {
      LoggerObj.log("SVG Generated : " + SVGFilePath.string());
      SVGFileList[F] = SVGFilePath;
    } else
      LoggerObj.log("Failed to generate SVG!");
  }
};

// LSP Server will use this class to query details about the IR file.
// FIXME: For the moment we assume that we can only run "default<O3>" on the IR.
class IRDocument {
  LLVMContext C;
  std::unique_ptr<Module> ParsedModule;
  Logger &LoggerObj;
  StringRef Filepath;

  std::unique_ptr<OptRunner> Optimizer;
  std::unique_ptr<IRArtifacts> IRA;

public:
  IRDocument(StringRef PathToIRFile, Logger &LO)
      : LoggerObj(LO), Filepath(PathToIRFile) {
    ParsedModule = loadModuleFromIR(PathToIRFile, C);
    IRA = std::make_unique<IRArtifacts>(PathToIRFile, LO, *ParsedModule);
    Optimizer = std::make_unique<OptRunner>(*ParsedModule, LO);

    // Eagerly generate all CFG for all functions in the IRDocument.
    IRA->generateGraphs(ParserContext);
    LoggerObj.log("Finished setting up IR Document: " + PathToIRFile.str());
  }

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
  void generateCFGs() { IRA->generateGraphs(ParserContext); }

  std::optional<std::string> getPathForSVGFile(Function *F) {
    return IRA->getSVGFilePath(F);
  }

  Function *getFunctionAtLocation(unsigned Line, unsigned Col) {
    FileLoc FL(Line, Col);
    if (auto MaybeF = ParserContext.getFunctionAtLocation(FL))
      return MaybeF.value();
    return nullptr;
  }

  BasicBlock *getBlockAtLocation(unsigned Line, unsigned Col) {
    FileLoc FL(Line, Col);
    if (auto MaybeBB = ParserContext.getBlockAtLocation(FL))
      return MaybeBB.value();
    return nullptr;
  }

  Instruction *getInstructionAtLocation(unsigned Line, unsigned Col) {
    FileLoc FL(Line, Col);
    if (auto MaybeI = ParserContext.getInstructionAtLocation(FL))
      return MaybeI.value();
    return nullptr;
  }

  // N is 1-Indexed here, but IRA expects 0-Indexed
  std::string getIRAfterPassNumber(unsigned N) {
    auto ExistingIR = IRA->getIRAfterPassNumber(N);
    if (ExistingIR) {
      LoggerObj.log("Found Existing IR");
      return *ExistingIR;
    }
    auto PassName = Optimizer->getPassName("default<O3>", N);
    LoggerObj.log("Found Pass name for pass number " + std::to_string(N) +
                  " as " + PassName);

    auto IntermediateIR = Optimizer->getModuleAfterPass("default<O3>", N);
    LoggerObj.log("Got intermediate IR. Storing it in Artifacts Directory!");
    IRA->addIntermediateIR(*IntermediateIR.get(), N, PassName);
    LoggerObj.log("Finished storing in Artifacts directory!");
    return *IRA->getIRAfterPassNumber(N);
  }

  // FIXME: We are doing some redundant work here in below functions, which can
  // be fused together.
  const SmallVector<std::string, 256> getPassList() {
    SmallVector<std::string, 256> PassList;
    auto PassNameAndDescriptionList =
        Optimizer->getPassListAndDescription("default<O3>");

    for (auto &P : PassNameAndDescriptionList)
      PassList.push_back(P.first);

    return PassList;
  }
  const SmallVector<std::string, 256> getPassDescriptions() {
    SmallVector<std::string, 256> PassDesc;
    auto PassNameAndDescriptionList =
        Optimizer->getPassListAndDescription("default<O3>");
    for (auto &P : PassNameAndDescriptionList)
      PassDesc.push_back(P.second);

    return PassDesc;
  }

  AsmParserContext ParserContext;

private:
  std::unique_ptr<Module> loadModuleFromIR(StringRef Filepath, LLVMContext &C) {
    SMDiagnostic Err;
    // Try to parse as textual IR
    auto M = parseIRFile(Filepath, Err, C, {}, &ParserContext);
    if (!M)
      // If parsing failed, print the error and crash
      LoggerObj.error("Failed parsing IR file: " + Err.getMessage().str());
    return M;
  }
};

} // namespace llvm

#endif // LLVM_TOOLS_LLVM_LSP_IRDOCUMENT_H
