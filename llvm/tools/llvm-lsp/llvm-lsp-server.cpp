//===-- llvm-lsp-server.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <iostream>

#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"

#include "IRDocument.h"
#include "llvm-lsp-server.h"
#include "llvm/ADT/StringRef.h"
#include <string>

using namespace llvm;

static cl::OptionCategory LlvmLspServerCategory("llvm-lsp-server options");
static cl::opt<std::string> LogFilePath("log-file",
                                        cl::desc("Path to log file"),
                                        cl::init("/tmp/llvm-lsp-server.log"),
                                        cl::cat(LlvmLspServerCategory));

bool LspServer::processRequest() {
  std::string Msg = readMessage();
  LoggerObj.log("Received Message from Client: " + Msg);
  // TODO: Should we really quit on empty message?
  if (Msg.empty())
    return false;
  return handleMessage(Msg);
}

void LspServer::sendInfo(const std::string &Message) {
  json::Object NotificationParams{{"type", 3}, // Info
                                  {"message", Message}};
  sendNotification(std::string("window/showMessage"),
                   json::Value(std::move(NotificationParams)));
}

std::string LspServer::readMessage() {
  std::string Line;
  size_t ContentLength = 0;
  // Read headers
  while (std::getline(std::cin, Line) && !Line.empty()) {
    LoggerObj.log("Received Message from Client: " + Line);
    if (!Line.empty() && Line.back() == '\r')
      Line.pop_back();
    if (Line.empty())
      break; // End of headers

    if (Line.find("Content-Length:") == 0)
      ContentLength = std::stoi(Line.substr(15));
    if (Line.find("Content-Type") == 0)
      continue; // TODO: Handle Content-Type header
  }
  // Read body
  std::string JsonStr(ContentLength, '\0');
  std::cin.read(&JsonStr[0], ContentLength);
  return JsonStr;
}

void LspServer::sendMessage(const json::Value &ID, const std::string &Kind,
                            const json::Value &Payload) {
  json::Object ResponseObj{{"jsonrpc", "2.0"}, {"id", ID}, {Kind, Payload}};
  std::string Output;
  raw_string_ostream OutStr(Output);
  OutStr << json::Value(std::move(ResponseObj));
  std::cout << "Content-Length: " << Output.size() << "\r\n\r\n" << Output;
  std::cout.flush();
}

void LspServer::sendResponse(const json::Value &ID,
                             const json::Value &Response) {
  sendMessage(ID, "result", Response);
}

void LspServer::sendErrorResponse(const json::Value &ID, const int Code,
                                  const std::string &Message) {
  sendMessage(ID, "error", json::Object{{"code", Code}, {"message", Message}});
}

void LspServer::sendNotification(const std::string &RPCMethod,
                                 const json::Value &Params) {
  json::Object NotificationObj{
      {"jsonrpc", "2.0"}, {"method", RPCMethod}, {"params", Params}};
  std::string Output;
  raw_string_ostream OutStr(Output);
  OutStr << json::Value(std::move(NotificationObj));
  std::cout << "Content-Length: " << Output.size() << "\r\n\r\n" << Output;
  std::cout.flush();
}

const json::Value *LspServer::queryJSON(const json::Value *JSONObject,
                                        StringRef Query) {
  SmallVector<std::string, 8> QueryComponents;
  auto SplitQuery = [&QueryComponents](std::string Query) {
    std::istringstream SS(Query);
    std::string Token;
    while (std::getline(SS, Token, '.')) {
      QueryComponents.push_back(Token);
    }
  };
  SplitQuery(Query.str());
  const json::Value *Current = JSONObject;
  for (const auto &Key : QueryComponents) {
    if (const auto *Obj = Current->getAsObject()) {
      auto It = Obj->find(Key);
      if (It == Obj->end())
        return nullptr;
      Current = &It->getSecond();
    } else {
      return nullptr;
    }
  }
  return Current;
}

void LspServer::handleRequestInitialize(const json::Value *Id,
                                        const json::Value *Params) {
  LoggerObj.log("Received Initialize Message!");
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
        {"hoverProvider", true},
        {"codeActionProvider", true},
        {"definitionProvider", true},
      }
    }
  };
  // clang-format on
  sendResponse(*Id, json::Value(std::move(ResponseParams)));
}

void LspServer::handleNotificationTextDocumentDidOpen(
    const json::Value *Id, const json::Value *Params) {
  LoggerObj.log("Received didOpen Message!");
  StringRef Filepath = queryJSONForFilePath(Params, "textDocument.uri");
  sendInfo("LLVM Language Server Recognized that you opened " + Filepath.str());

  // Prepare IRDocument for Queries
  LoggerObj.log("Creating IRDocument for " + Filepath.str());
  OpenDocuments[Filepath.str()] =
      std::make_unique<IRDocument>(Filepath.str(), LoggerObj);
}

static json::Object fileLocToJSON(FileLoc FL) {
  return json::Object{{"line", FL.Line}, {"character", FL.Col}};
}

static json::Object fileLocRangeToJSON(FileLocRange FLR) {
  return json::Object{{"start", fileLocToJSON(FLR.Start)},
                      {"end", fileLocToJSON(FLR.End)}};
}

void LspServer::handleRequestGetReferences(const json::Value *Id,
                                           const json::Value *Params) {
  auto Filepath = queryJSONForFilePath(Params, "textDocument.uri");
  auto Line = queryJSON(Params, "position.line")->getAsInteger();
  auto Character = queryJSON(Params, "position.character")->getAsInteger();
  assert(Line && *Line >= 0);
  assert(Character && *Character >= 0);
  std::stringstream SS;
  // SS << "Requested references for token: " << Filepath.str() << ":" << *Line
  //    << ":" << *Character;
  // sendInfo(SS.str());
  json::Array Result;
  const auto &Doc = OpenDocuments[Filepath.str()];
  if (Instruction *MaybeI = Doc->getInstructionAtLocation(*Line, *Character)) {
    auto AddReference = [&Result, &Filepath, &Doc](Instruction *I) {
      // FIXME: very hacky way to remove the newline from the reference...
      //   we need to have the parser set the proper end
      auto End = Doc->ParserContext.getInstructionLocation(I).value().End;
      End.Line--;
      End.Col = 10000;
      Result.push_back(json::Object{
          {"uri", Filepath},
          {"range",
           json::Object{{"start", fileLocToJSON(Doc->ParserContext
                                                    .getInstructionLocation(I)
                                                    .value()
                                                    .Start)},
                        {"end", fileLocToJSON(/*I->SrcLoc->*/ End)}}},
      });
    };
    AddReference(MaybeI);
    for (User *U : MaybeI->users()) {
      if (auto *UserInst = dyn_cast<Instruction>(U)) {
        if (Doc->ParserContext.getInstructionLocation(UserInst))
          AddReference(UserInst);
      }
    }
  }

  sendResponse(*Id, std::move(Result));
}

void LspServer::handleRequestCodeAction(const json::Value *Id,
                                        const json::Value *Params) {
  sendResponse(*Id, json::Array{json::Object{{"title", "Open CFG Preview"},
                                             {"command", "llvm.cfg"}}});
}

void LspServer::handleRequestGetCFG(const json::Value *Id,
                                    const json::Value *Params) {
  // TODO: have a flag to force regenerating the artifacts
  StringRef Filepath = queryJSONForFilePath(Params, "uri");
  auto Line = queryJSON(Params, "position.line")->getAsInteger();
  auto Character = queryJSON(Params, "position.character")->getAsInteger();

  if (OpenDocuments.find(Filepath.str()) == OpenDocuments.end())
    LoggerObj.error("Did not open file previously " + Filepath.str());
  IRDocument &Doc = *OpenDocuments[Filepath.str()];

  Function *F = nullptr;
  BasicBlock *BB = nullptr;
  if (Instruction *MaybeI =
          OpenDocuments[Filepath.str()]->getInstructionAtLocation(*Line,
                                                                  *Character)) {
    BB = MaybeI->getParent();
    F = BB->getParent();
  } else {
    F = Doc.getFirstFunction();
    BB = &F->getEntryBlock();
  }

  auto PathOpt = Doc.getPathForSVGFile(F);
  if (!PathOpt)
    LoggerObj.log("Did not find Path for SVG file for " + Filepath.str());

  // clang-format off
  json::Object ResponseParams{
  {"result",
    json::Object{
        // TODO: unify handling of uri, filepath, optionals.... {"uri", "file://" + *PathOpt},
        // the protocol should exclusively use uris
        {"uri", "file://" + *PathOpt},
        {"node_id", Doc.getNodeId(Doc.getBlockAtLocation(*Line, *Character))},
        {"function", F->getName()},
      }
    }
  };
  // clang-format on

  sendResponse(*Id, json::Value(std::move(ResponseParams)));

  SVGToIRMap[*PathOpt] = Filepath.str();
}

// TODO: factor out the filepath -> uri operation
void LspServer::handleRequestGetBBLocation(const json::Value *Id,
                                           const json::Value *Params) {
  auto Filepath = queryJSONForFilePath(Params, "uri");
  auto NodeIDStr = queryJSON(Params, "node_id")->getAsString();
  assert(NodeIDStr);

  // sendInfo("LLVM Language Server Recognized request to get Basicblock "
  //          "corresponding to SVG file " +
  //          Filepath.str() + ", Node ID: " + NodeIDStr->str());

  // We assume the query to SVGToIRMap would not fail.
  auto IR = SVGToIRMap[Filepath.str()];
  IRDocument &Doc = *OpenDocuments[IR];
  sendResponse(
      *Id,
      json::Object{
          {"result", json::Object{{"range", fileLocRangeToJSON(Doc.parseNodeId(
                                                NodeIDStr->str()))},
                                  {"uri", "file://" + IR}}}});
}

void LspServer::handleRequestTextDocumentDefinition(const json::Value *Id,
                                                    const json::Value *Params) {
  StringRef Filepath = queryJSONForFilePath(Params, "textDocument.uri");
  unsigned Line = queryJSONForInt(Params, "position.line");
  unsigned Col = queryJSONForInt(Params, "position.character");

  LoggerObj.log("Recognized request : " + Filepath.str() + ", Line: " +
                std::to_string(Line) + ", Col: " + std::to_string(Col));

  if (OpenDocuments.find(Filepath.str()) == OpenDocuments.end())
    LoggerObj.error("Did not open file previously " + Filepath.str());
  IRDocument &Doc = *OpenDocuments[Filepath.str()];

  const Function *F = Doc.getFunctionAtLocation(Line, Col);
  if (!F)
    sendInfo("You clicked on a region that is not inside any function!");
  else
    sendInfo("You clicked on Function : " + F->getName().str());

  // clang-format off
  // Sending path to same file
  json::Object ResponseParams{
    {"uri", "file://" + Filepath.str()},
    {"range",
      json::Object{
        {"start", json::Object{{"line", 0}, {"character", 0}}},
        {"end", json::Object{{"line", 5}, {"character", 0}}}
        }  
      }
  };
  // clang-format on
  sendResponse(*Id, json::Value(std::move(ResponseParams)));
}

void LspServer::handleRequestGetPassList(const json::Value *Id,
                                         const json::Value *Params) {

  StringRef Filepath = queryJSONForFilePath(Params, "uri");

  if (OpenDocuments.find(Filepath.str()) == OpenDocuments.end())
    LoggerObj.error("Did not open file previously " + Filepath.str());
  IRDocument &Doc = *OpenDocuments[Filepath.str()];

  LoggerObj.log("Opened IR file to get pass list " + Filepath.str());

  auto PassList = Doc.getPassList();

  auto PassDescriptions = Doc.getPassDescriptions();

  json::Array NameArray, DescArray;
  for (unsigned I = 0; I < PassList.size(); I++)
    NameArray.push_back(PassList[I]);
  for (unsigned I = 0; I < PassDescriptions.size(); I++)
    DescArray.push_back(PassDescriptions[I]);

  if (PassList.size() != PassDescriptions.size())
    LoggerObj.error("Size mismatch between the objects!");

  // Build the response object
  json::Object ResponseParams;
  ResponseParams["list"] = json::Value(std::move(NameArray));
  ResponseParams["descriptions"] = json::Value(std::move(DescArray));
  ResponseParams["status"] = "success";

  // clang-format on
  sendResponse(*Id, json::Value(std::move(ResponseParams)));
}

void LspServer::handleRequestGetIRAfterPass(const json::Value *Id,
                                            const json::Value *Params) {
  StringRef Filepath = queryJSONForFilePath(Params, "uri");

  if (OpenDocuments.find(Filepath.str()) == OpenDocuments.end())
    LoggerObj.error("Did not open file previously " + Filepath.str());
  IRDocument &Doc = *OpenDocuments[Filepath.str()];

  unsigned PassNum = queryJSONForInt(Params, "passnumber");
  std::string IRFilePath = Doc.getIRAfterPassNumber(PassNum);

  json::Object ResponseParams{{"uri", "file://" + IRFilePath}};

  sendResponse(*Id, json::Value(std::move(ResponseParams)));
}

bool LspServer::handleMessage(const std::string &JsonStr) {
  auto Val = json::parse(JsonStr);
  assert(Val && "Error Parsing JSON String!");

  const json::Object *Obj = Val->getAsObject();
  assert(Obj && "Expected valid JSON Object!");

  const std::string Method = Obj->getString("method")->str();
  const json::Value *Id = Obj->get("id");
  const json::Value *Params = Obj->get("params");

  // TODO: some methods can only be sent once, enforce it
  //   these include: initialize, initialized, shutdown, exit
  switch (State) {

  case LspServerState::Starting: {
    if (Method == "initialize") {
      assert(Id && "Expected valid ID field!");
      assert(Params && "Expected valid Params field!");
      switchToState(LspServerState::Initializing);
      handleRequestInitialize(Id, Params);
      return true;
    }
    // For requests, reply with error code
    if (Id) {
      sendErrorResponse(*Id, LspErrorCode::RequestDuringInitialization, "");
      return true;
    }
    // For notifications, only process 'exit'
    if (Method == "exit") {
      switchToState(LspServerState::Exitted);
      return false;
    }
    // Ignore the rest
    return true;
  }

  case LspServerState::Initializing: {
    if (Method == "initialized") {
      switchToState(LspServerState::Ready);
      return true;
    }
    // TODO: shouldn't be getting anything, for now just ignore it if we do
    return true;
  }

  case LspServerState::Ready: {
    if (Method == "shutdown") {
      switchToState(LspServerState::ShuttingDown);
      sendErrorResponse(*Id, LspErrorCode::InvalidRequest, "");
      return true;
    }

    // Ignored for now
    if (Method == "textDocument/hover")
      return true;
    if (Method == "$/cancelRequest")
      return true;
    if (Method == "$/setTrace")
      return true;
    if (Method == "textDocument/didClose")
      return true;

    if (Method == "textDocument/didOpen") {
      handleNotificationTextDocumentDidOpen(Id, Params);
      return true;
    }
    if (Method == "textDocument/references") {
      handleRequestGetReferences(Id, Params);
      return true;
    }
    if (Method == "textDocument/codeAction") {
      handleRequestCodeAction(Id, Params);
      return true;
    }
    if (Method == "llvm/getCfg") {
      handleRequestGetCFG(Id, Params);
      return true;
    }
    if (Method == "llvm/bbLocation") {
      handleRequestGetBBLocation(Id, Params);
      return true;
    }
    if (Method == "llvm/getPassList") {
      sendInfo("Fetching Pass List");
      LoggerObj.log("Received Message to send Pass List");
      handleRequestGetPassList(Id, Params);
      return true;
    }
    if (Method == "llvm/getIRAfterPass") {
      sendInfo("Getting IR given Pass Number");
      LoggerObj.log("Received Message to retrieve IR from Pass Number");
      handleRequestGetIRAfterPass(Id, Params);
      return true;
    }
    if (Method == "textDocument/definition") {
      handleRequestTextDocumentDefinition(Id, Params);
      return true;
    }
    // TODO: handle other LSP methods
    sendInfo("[WIP] Unhandled RPC call : " + Method);
    return true;
  }

  case LspServerState::ShuttingDown: {
    if (Method == "exit") {
      switchToState(LspServerState::Exitted);
      // TODO: not sure what's wrong but restarting the lsp from vscode never
      // gets here... figure out why
      LoggerObj.log("Bye!");
      return false;
    }
    // TODO: shouldn't be getting anything, for now just quit if we do
    return false;
  }

  case LspServerState::Exitted: {
    // TODO: shouldn't be getting anything, for now just quit if we do
    return false;
  }
  }
  // silence warning
  return true;
}

int main(int argc, char **argv) {
  cl::HideUnrelatedOptions(LlvmLspServerCategory);
  cl::ParseCommandLineOptions(argc, argv, "LLVM LSP Language Server");

  LspServer LS(LogFilePath);

  while (LS.processRequest())
    ;

  return LS.getExitCode();
}
