//===-- llvm-lsp-server.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include "IRDocument.h"
#include "Protocol.h"
#include "llvm-lsp-server.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

using namespace llvm;

static cl::OptionCategory LlvmLspServerCategory("llvm-lsp-server options");
static cl::opt<std::string> LogFilePath("log-file",
                                        cl::desc("Path to log file"),
                                        cl::init("/tmp/llvm-lsp-server.log"),
                                        cl::cat(LlvmLspServerCategory));

static cl::opt<std::string> OptPath("opt-path", cl::desc("Path to opt file"),
                                    cl::init(""),
                                    cl::cat(LlvmLspServerCategory));

static lsp::Position llvmFileLocToLspPosition(const FileLoc &Pos) {
  return lsp::Position(Pos.Line, Pos.Col);
}

static lsp::Range llvmFileLocRangeToLspRange(const FileLocRange &Range) {
  return lsp::Range(llvmFileLocToLspPosition(Range.Start),
                    llvmFileLocToLspPosition(Range.End));
}

llvm::Error LspServer::run() {
  registerMessageHandlers();
  return Transport.run(MessageHandler);
}

void LspServer::sendInfo(const std::string &Message) {
  ShowMessageSender(lsp::ShowMessageParams(lsp::MessageType::Info, Message));
}

void LspServer::sendError(const std::string &Message) {
  ShowMessageSender(lsp::ShowMessageParams(lsp::MessageType::Error, Message));
}

void LspServer::handleRequestInitialize(
    const lsp::InitializeParams &Params,
    lsp::Callback<llvm::json::Value> Reply) {
  lsp::Logger::info("Received Initialize Message!");
  sendInfo("Hello! Welcome to LLVM IR Language Server!");

  // clang-format off
  json::Object ResponseParams{
    {"capabilities",
      json::Object{
          {"textDocumentSync",
          json::Object{
              {"openClose", true},
              {"change", 0}, // We dont want to sync the documents.
          }
        },
        {"referencesProvider", true},
        {"codeActionProvider", true},
        {"documentSymbolProvider", true},
      }
    }
  };
  // clang-format on
  Reply(json::Value(std::move(ResponseParams)));
}

void LspServer::handleNotificationTextDocumentDidOpen(
    const lsp::DidOpenTextDocumentParams &Params) {
  lsp::Logger::info("Received didOpen Message!");
  auto Filepath = Params.textDocument.uri;
  sendInfo("LLVM Language Server Recognized that you opened " +
           Filepath.uri().str());

  // Prepare IRDocument for Queries
  lsp::Logger::info("Creating IRDocument for {}", Filepath);
  OpenDocuments[Filepath] = std::make_unique<IRDocument>(
      Filepath.file().str(),
      OptPath == "" ? std::nullopt : std::optional(OptPath.getValue()));
  if (auto &Doc = OpenDocuments[Filepath]; Doc->open()) {
    sendError(*Doc->open());
    OpenDocuments.erase(Filepath);
  }
}

void LspServer::handleRequestGetReferences(
    const lsp::ReferenceParams &Params,
    lsp::Callback<std::vector<lsp::Location>> Reply) {
  auto Filepath = Params.textDocument.uri;
  auto Line = Params.position.line;
  auto Character = Params.position.character;
  assert(Line >= 0);
  assert(Character >= 0);
  std::stringstream SS;
  std::vector<lsp::Location> Result;
  const auto &Doc = OpenDocuments[Filepath];
  if (Instruction *MaybeI = Doc->getInstructionAtLocation(Line, Character)) {
    auto TryAddReference = [&Result, &Params, &Doc](Instruction *I) {
      // FIXME: very hacky way to remove the newline from the reference...
      //   we need to have the parser set the proper end
      auto MaybeInstLocation = Doc->ParserContext.getInstructionLocation(I);
      if (!MaybeInstLocation)
        return;
      Result.emplace_back(
          lsp::Location(Params.textDocument.uri,
                        llvmFileLocRangeToLspRange(MaybeInstLocation.value())));
    };
    TryAddReference(MaybeI);
    for (User *U : MaybeI->users()) {
      if (auto *UserInst = dyn_cast<Instruction>(U)) {
        TryAddReference(UserInst);
      }
    }
  }

  Reply(std::move(Result));
}

void LspServer::handleRequestTextDocumentDocumentSymbol(
    const lsp::DocumentSymbolParams &Params,
    lsp::Callback<std::vector<lsp::DocumentSymbol>> Reply) {
  if (!OpenDocuments.contains(Params.textDocument.uri)) {
    lsp::Logger::error(
        "Document in textDocument/documentSymbol request not open: {}",
        Params.textDocument.uri.file());
    return Reply(
        make_error<lsp::LSPError>(formatv("Did not open file previously {}",
                                          Params.textDocument.uri.file()),
                                  lsp::ErrorCode::InvalidParams));
  }
  auto &Doc = OpenDocuments[Params.textDocument.uri];
  std::vector<lsp::DocumentSymbol> Result;
  for (const auto &Fn : Doc->getFunctions()) {
    lsp::DocumentSymbol Func;
    Func.name = Fn.getNameOrAsOperand();
    Func.kind = lsp::SymbolKind::Function;
    auto MaybeLoc = Doc->ParserContext.getFunctionLocation(&Fn);
    if (!MaybeLoc)
      continue;
    Func.range = llvmFileLocRangeToLspRange(*MaybeLoc);
    Func.selectionRange = Func.range;
    for (const auto &BB : Fn) {
      lsp::DocumentSymbol Block;
      Block.name = BB.getNameOrAsOperand();
      Block.kind =
          lsp::SymbolKind::Namespace; // Using namespace as there is no block
                                      // kind, and namespace is the closest
      Block.detail = "basic block";
      auto MaybeLoc = Doc->ParserContext.getBlockLocation(&BB);
      if (!MaybeLoc)
        continue;
      Block.range = llvmFileLocRangeToLspRange(*MaybeLoc);
      Block.selectionRange = Block.range;
      for (const auto &I : BB) {
        lsp::DocumentSymbol Inst;
        Inst.name = I.getNameOrAsOperand();
        Inst.kind = lsp::SymbolKind::Variable;
        {
          raw_string_ostream Ss(Inst.detail);
          I.print(Ss);
        }
        auto MaybeLoc = Doc->ParserContext.getInstructionLocation(&I);
        if (!MaybeLoc)
          continue;
        Inst.range = llvmFileLocRangeToLspRange(*MaybeLoc);
        Inst.selectionRange = Inst.range;
        Block.children.emplace_back(std::move(Inst));
      }
      Func.children.emplace_back(std::move(Block));
    }
    Result.emplace_back(std::move(Func));
  }
  Reply(std::move(Result));
}

void LspServer::handleRequestCodeAction(const lsp::CodeActionParams &Params,
                                        lsp::Callback<json::Value> Reply) {
  if (auto It = OpenDocuments.find(Params.textDocument.uri);
      It != OpenDocuments.end() &&
      It->second->getFunctionAtLocation(Params.range.start.line,
                                        Params.range.start.character))
    return Reply(json::Array{
        json::Object{{"title", "Open CFG Preview"}, {"command", "llvm.cfg"}}});
  return Reply(json::Array());
}

void LspServer::handleRequestGetCFG(const lsp::GetCfgParams &Params,
                                    lsp::Callback<lsp::CFG> Reply) {
  // TODO: have a flag to force regenerating the artifacts
  auto Filepath = Params.uri;
  auto Line = Params.position.line;
  auto Character = Params.position.character;

  for (const auto &[K, _] : OpenDocuments) {
    lsp::Logger::debug("OpenDocuments: {}", K);
  }
  if (OpenDocuments.find(Filepath) == OpenDocuments.end()) {
    lsp::Logger::error("Did not open file previously {}", Filepath);
    return Reply(make_error<lsp::LSPError>(
        formatv("Did not open file previously {}", Filepath),
        lsp::ErrorCode::InvalidParams));
  }
  IRDocument &Doc = *OpenDocuments[Filepath];
  lsp::Logger::debug("Opened doc");

  Function *F = nullptr;
  BasicBlock *BB = nullptr;
  if (BasicBlock *MaybeBB = Doc.getBlockAtLocation(Line, Character)) {
    BB = MaybeBB;
    F = BB->getParent();
  } else {
    F = Doc.getFunctionAtLocation(Line, Character);
    if (!F) {
      sendError("Not a location of a function.");
      return Reply(make_error<lsp::LSPError>("Not a location of a function",
                                             lsp::ErrorCode::InvalidRequest));
    }
    if (F->isDeclaration())
      return Reply(
          make_error<lsp::LSPError>("Cannot display CFG for declaration",
                                    lsp::ErrorCode::InvalidRequest));
    BB = &F->getEntryBlock();
  }

  lsp::Logger::debug("Found objects");

  auto PathOpt = Doc.getPathForSVGFile(F);
  if (!PathOpt)
    lsp::Logger::info("Did not find Path for SVG file for {}", Filepath);

  lsp::CFG Result;
  Result.uri = *PathOpt;
  Result.node_id = Doc.getNodeId(Doc.getBlockAtLocation(Line, Character));
  Result.function = F->getName();

  Reply(Result);

  SVGToIRMap[*PathOpt] = Filepath;
}

void LspServer::handleRequestBBLocation(const lsp::BbLocationParams &Params,
                                        lsp::Callback<lsp::BbLocation> Reply) {
  auto Filepath = Params.uri;
  auto NodeIDStr = Params.node_id;

  // We assume the query to SVGToIRMap would not fail.
  auto IR = SVGToIRMap[Filepath];
  IRDocument &Doc = *OpenDocuments[IR];
  lsp::BbLocation Result;
  Result.range = llvmFileLocRangeToLspRange(Doc.parseNodeId(NodeIDStr));
  Result.uri = IR;
  return Reply(Result);
}

void LspServer::handleRequestGetPassList(const lsp::GetPassListParams &Params,
                                         lsp::Callback<lsp::PassList> Reply) {

  auto Filepath = Params.uri;
  std::string Pipeline = Params.pipeline;

  if (OpenDocuments.find(Filepath) == OpenDocuments.end()) {
    lsp::Logger::error("Did not open file previously {}", Filepath);
    return Reply(make_error<lsp::LSPError>(
        formatv("Did not open file previously {}", Filepath),
        lsp::ErrorCode::InvalidParams));
  }
  IRDocument &Doc = *OpenDocuments[Filepath];

  lsp::Logger::info("Opened IR file to get pass list {}", Filepath);

  auto PassListResult = Doc.getPassList(Pipeline, Params.additional_opt_args);

  if (!PassListResult) {
    return Reply(PassListResult.takeError());
  }

  auto PassList = PassListResult.get();

  auto PassDescriptionsResult =
      Doc.getPassDescriptions(Pipeline, Params.additional_opt_args);

  if (!PassDescriptionsResult) {
    return Reply(PassDescriptionsResult.takeError());
  }

  auto PassDescriptions = PassDescriptionsResult.get();

  if (PassList.size() != PassDescriptions.size()) {
    lsp::Logger::error("Size mismatch between the objects!");
    return Reply(make_error<lsp::LSPError>("Size mismatch between the objects!",
                                           lsp::ErrorCode::InvalidParams));
  }

  // Build the response object
  lsp::PassList ResponseParams;
  ResponseParams.list.insert(ResponseParams.list.begin(), PassList.begin(),
                             PassList.end());
  ResponseParams.descriptions.insert(ResponseParams.descriptions.begin(),
                                     PassDescriptions.begin(),
                                     PassDescriptions.end());

  Reply(ResponseParams);
}

void LspServer::handleRequestGetIRBeforePass(
    const lsp::GetIRBeforePassParams &Params, lsp::Callback<lsp::IR> Reply) {
  auto Filepath = Params.uri;
  std::string Pipeline = Params.pipeline;

  if (OpenDocuments.find(Filepath) == OpenDocuments.end()) {
    lsp::Logger::error("Did not open file previously {}", Filepath);
    return Reply(make_error<lsp::LSPError>(
        formatv("Did not open file previously {}", Filepath),
        lsp::ErrorCode::InvalidParams));
  }
  IRDocument &Doc = *OpenDocuments[Filepath];

  unsigned PassNum = Params.passnumber;
  std::vector<StringRef> PassedAdditionalArgs;
  if (Params.additional_opt_args)
    for (const auto &Args : *Params.additional_opt_args)
      PassedAdditionalArgs.emplace_back(Args);
  auto IRFilePathResult =
      Doc.getIRBeforePassNumber(Pipeline, PassNum, PassedAdditionalArgs);

  if (!IRFilePathResult) {
    return Reply(IRFilePathResult.takeError());
  }

  auto IRFilePath = IRFilePathResult.get();

  if (auto MaybeIRUri = lsp::URIForFile::fromFile(IRFilePath.string())) {
    lsp::IR Return;
    Return.uri = *MaybeIRUri;
    Reply(Return);
  } else {
    return Reply(MaybeIRUri.takeError());
  }

  return;
}

bool LspServer::registerMessageHandlers() {
  MessageHandler.method("initialize", this,
                        &LspServer::handleRequestInitialize);

  MessageHandler.notification(
      "textDocument/didOpen", this,
      &LspServer::handleNotificationTextDocumentDidOpen);
  MessageHandler.method("textDocument/references", this,
                        &LspServer::handleRequestGetReferences);
  MessageHandler.method("textDocument/documentSymbol", this,
                        &LspServer::handleRequestTextDocumentDocumentSymbol);
  MessageHandler.method("textDocument/codeAction", this,
                        &LspServer::handleRequestCodeAction);
  // Custom messages
  MessageHandler.method("llvm/getCfg", this, &LspServer::handleRequestGetCFG);
  MessageHandler.method("llvm/bbLocation", this,
                        &LspServer::handleRequestBBLocation);
  MessageHandler.method("llvm/getPassList", this,
                        &LspServer::handleRequestGetPassList);
  MessageHandler.method("llvm/getIRBeforePass", this,
                        &LspServer::handleRequestGetIRBeforePass);

  ShowMessageSender =
      MessageHandler.outgoingNotification<lsp::ShowMessageParams>(
          "window/showMessage");

  // Return true to indicate handlers were registered successfully
  return true;
}

int main(int argc, char **argv) {
  cl::HideUnrelatedOptions(LlvmLspServerCategory);
  cl::ParseCommandLineOptions(argc, argv, "LLVM LSP Language Server");

  llvm::sys::ChangeStdinToBinary();
  lsp::JSONTransport Transport(stdin, llvm::outs());

  LspServer LS(Transport);

  lsp::Logger::setLogLevel(lsp::Logger::Level::Debug);

  auto LSResult = LS.run();
  if (!LSResult)
    lsp::Logger::error("Error while running Language Server: {}", LSResult);

  return LS.getExitCode();
}
