//===-- llvm-lsp-server.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_LSP_SERVER_H
#define LLVM_TOOLS_LLVM_LSP_SERVER_H

#include <sstream>

#include "llvm/Support/JSON.h"
#include <IRDocument.h>

namespace llvm {

class LspServer {
  Logger LoggerObj;

  enum class LspServerState {
    Starting,
    Initializing,
    Ready,
    ShuttingDown, // Received 'shutdown' message
    Exitted,      // Received 'exit' message
  } State = LspServerState::Starting;

  std::string stateToString(LspServerState S) {
    switch (S) {
    case LspServerState::Starting:
      return "Starting";
    case LspServerState::Initializing:
      return "Initializing";
    case LspServerState::Ready:
      return "Ready";
    case LspServerState::ShuttingDown:
      return "ShuttingDown";
    case LspServerState::Exitted:
      return "Exitted";
    }
    return "<UNKNOWN STATE>";
  }

  void switchToState(LspServerState NewState) {
    std::stringstream SS;
    SS << "Changing State from " << stateToString(State) << " to "
       << stateToString(NewState);
    LoggerObj.log(SS.str());
    State = NewState;
  }

  enum LspErrorCode {
    RequestDuringInitialization = -32002,
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
  };

  std::unordered_map<std::string, std::unique_ptr<IRDocument>> OpenDocuments;
  std::unordered_map<std::string, std::string> SVGToIRMap;

public:
  LspServer(const std::string Logfile) : LoggerObj(Logfile) {
    LoggerObj.log("Starting LLVM LSP Server");
  }

  // Receives one message via stdin and responds to it.
  bool processRequest();

  // Sends a message to client as INFO notification
  void sendInfo(const std::string &Message);

  // The process exit code, should be success only if the State is Exitted
  int getExitCode() { return State == LspServerState::Exitted ? 0 : 1; }

private:
  // Returns the JSON String encoded in the message
  std::string readMessage();

  // Send message (response with either success or error)
  void sendMessage(const json::Value &ID, const std::string &Kind,
                   const json::Value &Payload);

  // Given a Response message as JSON value, send it over stdout.
  void sendResponse(const json::Value &ID, const json::Value &Response);
  void sendErrorResponse(const json::Value &ID, const int Code,
                         const std::string &Message);

  // Given a Notification message as JSON value, send it over stdout.
  void sendNotification(const std::string &RPCMethod,
                        const json::Value &Params);

  // Given a path into a JSON object, retrieve the sub-object.
  const json::Value *queryJSON(const json::Value *JSONObject, StringRef Query);

  // Specifically retrieve a String Object
  StringRef queryJSONForString(const json::Value *JSONObject, StringRef Query) {
    const json::Value *StrValue = queryJSON(JSONObject, Query);
    if (!StrValue)
      LoggerObj.error("Did not find valid query object");

    auto StrOpt = StrValue->getAsString();
    if (!StrOpt)
      LoggerObj.error("Did not find valid string object");

    return *StrOpt;
  }

  // Retrieve a String Object and check if it is a filepath.
  StringRef queryJSONForFilePath(const json::Value *JSONObject,
                                 StringRef Query) {
    StringRef PathValue = queryJSONForString(JSONObject, Query);

    constexpr StringLiteral FileScheme = "file://";
    if (!PathValue.starts_with(FileScheme))
      LoggerObj.error("Uri For file must start with 'file://'");

    StringRef Filepath = PathValue.drop_front(FileScheme.size());
    return Filepath;
  }

  unsigned queryJSONForInt(const json::Value *JSONObject, StringRef Query) {
    const json::Value *IntValue = queryJSON(JSONObject, Query);
    if (!IntValue)
      LoggerObj.error("Did not find valid query object");

    auto IntOpt = IntValue->getAsInteger();
    if (!IntOpt)
      LoggerObj.error("Did not find valid integer object");

    return *IntOpt;
  }

  // ---------- Functions to handle various RPC calls -----------------------

  // initialize
  void handleRequestInitialize(const json::Value *Id,
                               const json::Value *Params);
  // textDocument/didOpen
  void handleNotificationTextDocumentDidOpen(const json::Value *Id,
                                             const json::Value *Params);

  // textDocument/references
  void handleRequestGetReferences(const json::Value *Id,
                                  const json::Value *Params);

  // textDocument/codeAction
  void handleRequestCodeAction(const json::Value *Id,
                               const json::Value *Params);

  // llvm/getCfg
  void handleRequestGetCFG(const json::Value *Id, const json::Value *Params);

  // llvm/bbLocation
  void handleRequestGetBBLocation(const json::Value *Id,
                                  const json::Value *Params);

  // llvm/getPassList
  void handleRequestGetPassList(const json::Value *Id,
                                const json::Value *Params);

  // llvm/getIRAfterPass
  void handleRequestGetIRAfterPass(const json::Value *Id,
                                   const json::Value *Params);

  // textDocument/definition
  void handleRequestTextDocumentDefinition(const json::Value *Id,
                                           const json::Value *Params);

  // Identifies RPC Call and dispatches the handling to other methods
  bool handleMessage(const std::string &JsonStr);
};

} // namespace llvm

#endif // LLVM_TOOLS_LLVM_LSP_SERVER_H
