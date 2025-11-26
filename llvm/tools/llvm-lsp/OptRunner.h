//===-- OptRunner.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_LSP_OPTRUNNER_H
#define LLVM_TOOLS_LLVM_LSP_OPTRUNNER_H

#include "Protocol.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LSP/Logging.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

namespace llvm {

SmallVector<std::pair<std::string, std::string>, 256>
parseOptPassList(StringRef OptOutput) {
  SmallVector<std::pair<std::string, std::string>, 256> PassListAndDescription;
  while (!(OptOutput.empty() || OptOutput == "\n")) {
    while (!OptOutput.starts_with("Running pass"))
      OptOutput = OptOutput.drop_front();
    auto NumberPlus = OptOutput.drop_while([](char C) { return !isDigit(C); });
    auto Number = NumberPlus.take_while(isDigit);
    auto AfterNumber =
        NumberPlus.drop_while([](char C) { return isDigit(C) || isSpace(C); });
    auto DescriptionStart = AfterNumber.find(" on ");
    auto Name = AfterNumber.take_front(DescriptionStart);
    auto Description =
        AfterNumber.drop_front(DescriptionStart).take_while([](char C) {
          return C != '\n';
        });
    auto Next = AfterNumber.drop_while([](char C) { return C != '\n'; });

    std::string OutName = Number.str() + "-";
    for (const auto &C : Name) {
      if (isSpace(C))
        continue;
      OutName += C;
    }

    PassListAndDescription.emplace_back(OutName, Description.str());
    OptOutput = Next;
  }
  return PassListAndDescription;
}

// FIXME: Maybe a better name?
class OptRunner {
  LLVMContext Context;
  const std::filesystem::path File;
  const std::optional<std::string> OptPath = std::nullopt;

  SmallVector<std::unique_ptr<Module>, 256> IntermediateIRList;

public:
  OptRunner(const std::filesystem::path &File,
            std::optional<std::string> OptPath = std::nullopt)
      : File(File), OptPath(OptPath) {}

  llvm::Expected<SmallVector<std::pair<std::string, std::string>, 256>>
  getPassListAndDescription(const std::string PipelineText,
                            const std::optional<std::vector<std::string>>
                                &AdditionalOptArgs = std::nullopt) {
    // First is Passname, Second is Pass Description.
    std::vector<std::string> OptArgs = {"-S", "--print-pass-numbers",
                                        "--disable-output", "--passes",
                                        PipelineText};
    if (AdditionalOptArgs.has_value()) {
      for (const auto &Arg : *AdditionalOptArgs) {
        OptArgs.emplace_back(Arg);
      }
    }
    auto MaybeOutErr = runShellOpt(OptArgs);
    if (!MaybeOutErr)
      return MaybeOutErr.takeError();
    auto [_, Stderr] = *MaybeOutErr;
    SmallString<1024> StderrContent;
    auto MaybeStderrFD = llvm::sys::fs::openNativeFileForRead(Stderr);
    if (!MaybeStderrFD) {
      lsp::Logger::error("Can't open error file from opt.");
      return MaybeStderrFD.takeError();
    }
    auto Res =
        llvm::sys::fs::readNativeFileToEOF(*MaybeStderrFD, StderrContent);

    if (Res)
      return Res;
    return parseOptPassList(StderrContent);
  }

  llvm::Expected<std::pair<SmallString<32>, SmallString<32>>>
  runShellOpt(std::vector<std::string> Args,
              std::optional<StringRef> StdoutPath = std::nullopt,
              std::optional<StringRef> StderrPath = std::nullopt) {
    std::string OptStr;
    if (OptPath) {
      lsp::Logger::debug("Using provided opt path: {}", OptPath.value());
      OptStr = OptPath.value();
    } else {
      lsp::Logger::debug("Searching for opt in PATH");
      auto Opt = llvm::sys::findProgramByName("opt");
      if (!Opt)
        return llvm::make_error<StringError>(Opt.getError(), "opt not found");
      OptStr = *Opt;
    }

    SmallString<32> StdoutPathStr;
    if (!StdoutPath.has_value()) {
      llvm::sys::fs::createTemporaryFile("llvm-lsp-stdout", "ll",
                                         StdoutPathStr);
      StdoutPath = StdoutPathStr;
    }
    SmallString<32> StderrPathStr;
    if (!StderrPath.has_value()) {
      llvm::sys::fs::createTemporaryFile("llvm-lsp-stderr", "ll",
                                         StderrPathStr);
      StderrPath = StderrPathStr;
    }

    std::optional<StringRef> Redirects[] = {std::nullopt, StdoutPath,
                                            StderrPath};

    std::vector<StringRef> AllArgs;
    for (const auto &Arg : Args)
      AllArgs.emplace_back(Arg);

    auto FileStr = File.string();
    AllArgs.emplace_back(FileStr);

    lsp::Logger::debug("Trying to run opt with these options:");
    for (const auto &Arg : AllArgs) {
      lsp::Logger::debug("{}", Arg);
    }
    lsp::Logger::debug("Shell command: {} {}", OptStr,
                       llvm::join(AllArgs, " "));

    lsp::Logger::debug("Output files:\n\tStdout: {}\n\tStderr: {}", StdoutPath,
                       StderrPath);

    auto ExitCode =
        llvm::sys::ExecuteAndWait(OptStr, AllArgs, std::nullopt, Redirects);
    llvm::sys::fs::file_status S;
    llvm::sys::fs::status(*StderrPath, S);
    lsp::Logger::debug("stderr size: {}", S.getSize());
    lsp::Logger::debug("Opt run done. ExitCode: {}", ExitCode);
    if (ExitCode) {
      SmallString<128> StderrContent;
      auto MaybeStderrFD = llvm::sys::fs::openNativeFileForRead(*StderrPath);
      if (!MaybeStderrFD) {
        lsp::Logger::error("Can't open error file from opt.");
        return MaybeStderrFD.takeError();
      }
      auto Res =
          llvm::sys::fs::readNativeFileToEOF(*MaybeStderrFD, StderrContent);
      if (Res) {
        lsp::Logger::error("Can't open error file from opt.");
        return Res;
      }
      lsp::Logger::error("Opt error: Exit code: {}, Stderr: {}", ExitCode,
                         StderrContent);
      return llvm::createStringError(
          "Running opt failed. Exit code: %d, Stderr: %s", ExitCode,
          StderrContent.c_str());
    }
    lsp::Logger::debug("Opt run handle done");
    return std::make_pair(*StdoutPath, *StderrPath);
  }

  // TODO: Check if N lies with in bounds for below methods. And to verify that
  // they are populated.
  // N is 1-Indexed
  llvm::Expected<std::filesystem::path>
  getModuleBeforePass(const std::string PipelineText, unsigned N,
                      const ArrayRef<StringRef> AdditionalOptArgs = {}) {

    SmallString<128> IRFolder;
    llvm::sys::fs::createUniqueDirectory("llvm-lsp-server-opt-output",
                                         IRFolder);

    std::vector<std::string> Args = {"-S",
                                     "--disable-output",
                                     "--print-before-pass-number",
                                     std::to_string(N),
                                     "--passes",
                                     PipelineText,
                                     "--ir-dump-directory",
                                     IRFolder.c_str()};
    for (const auto &Arg : AdditionalOptArgs)
      Args.emplace_back(Arg);
    auto MaybeOutErr = runShellOpt(Args, std::nullopt, std::nullopt);
    if (!MaybeOutErr)
      return MaybeOutErr.takeError();
    std::filesystem::path IRFolderPath(IRFolder.c_str());

    for (const auto &IRFile :
         std::filesystem::directory_iterator(IRFolderPath)) {
      if (!IRFile.path().filename().string().starts_with(std::to_string(N))) {
        continue;
      }
      return IRFile.path();
    }

    return llvm::createStringError("No intermediate IR was created");
  }

  /// Get's name of N-th pass
  /// N is 1 indexed
  llvm::Expected<std::string> getPassName(std::string PipelineText,
                                          unsigned N) {
    auto Passes = getPassListAndDescription(PipelineText);
    if (!Passes)
      return Passes.takeError();
    return Passes->operator[](N - 1).first;
  }
};

} // namespace llvm

#endif // LLVM_TOOLS_LLVM_LSP_OPTRUNNER_H
