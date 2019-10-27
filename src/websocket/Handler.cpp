#include "websocket/Handler.hpp"
#include "websocket/StateMachine.hpp"
#include "websocket/Response.hpp"
#include "common.hpp"
#include "config.hpp"
#include "connection.hpp"
#include "topic_manager.hpp"
#include "jwt/json/json.hpp"
#include "server.hpp"
#include "redis.hpp"

namespace eventhub {
namespace websocket {

void Handler::process(std::shared_ptr<Connection>& conn, Worker* wrk, char* buf, size_t n_bytes) {
  auto& fsm = conn->get_ws_fsm();
  fsm.process(buf, n_bytes);

  switch (fsm.get_state()) {
    case StateMachine::state::WS_CONTROL_READY:
      _handleControlFrame(conn, wrk, fsm);
      fsm.clearControlPayload();
      break;

    case StateMachine::state::WS_DATA_READY:
      _handleDataFrame(conn, wrk, fsm);
      fsm.clearPayload();
      break;
  }
}

void Handler::_handleDataFrame(std::shared_ptr<Connection>& conn, Worker* wrk, StateMachine& fsm) {
  auto& payload = fsm.get_payload();

  LOG(INFO) << "Data: " << payload;

  // Parse command and arguments.
  // Format is <COMMAND><SPACE><ARGUMENTS><NEWLINE>
  if (payload.length() > 1 && payload.substr(payload.length() - 1, payload.length()).compare("\n") == 0) {
    std::string command;
    std::string arg;
    auto argPos = payload.find_first_of(' ');

    if (argPos != std::string::npos) {
      command = payload.substr(0, argPos);
      arg     = payload.substr(argPos + 1, payload.length() - command.length() - 2);
    } else {
      command = payload.substr(0, payload.length() - 1);
    }

    _handleClientCommand(conn, wrk, command, arg);
  }
}

void Handler::_handleControlFrame(std::shared_ptr<Connection>& conn, Worker* wrk, StateMachine& fsm) {
  DLOG(INFO) << "Control Type: " << fsm.get_control_frame_type() << " payload: " << fsm.get_control_payload();

  switch (fsm.get_control_frame_type()) {
    case response::opcodes::CLOSE_FRAME: // Close
      DLOG(INFO) << "Recv close frame: " << fsm.get_payload() << " " << fsm.get_control_payload();
      conn->shutdown();
    break;

    case response::opcodes::PING_FRAME: // Ping
      DLOG(INFO) << "Sent PONG to " << conn->getIP();
      response::sendData(conn, fsm.get_control_payload(), response::opcodes::PONG_FRAME, 1);
    break;

    case response::opcodes::PONG_FRAME: // Pong
      DLOG(INFO) << "Got PONG from" << conn->getIP();
    break;
  }
}

void sendErrorMsg(std::shared_ptr<Connection>& conn, const std::string& errMsg, bool disconnect) {
  nlohmann::json j;
  j["error"] = errMsg;

  try {
    response::sendData(conn, j.dump(0));
  } catch(...) {}

  if (disconnect) {
    response::sendData(conn, "", response::opcodes::CLOSE_FRAME, 1);
    conn->shutdown();
  }

}

void Handler::_handleClientCommand(std::shared_ptr<Connection>& conn, Worker* wrk, const std::string& command, const std::string& arg) {
  LOG(INFO) << "Command: '" << command << "'";
  LOG(INFO) << "Arg: '" << arg << "'";

  auto& accessController = conn->get_access_controller();

  if (command.compare("AUTH") == 0) {
    auto authSuccess = accessController.authenticate(arg, Config.getJWTSecret());
    if (!authSuccess) {
      sendErrorMsg(conn, "Authentication failed.", true);
    }

    return;
  }

  if (!accessController.isAuthenticated()) {
    sendErrorMsg(conn, "Authenticate first.", true);
    return;
  }

  // Subscribe to a topic.
  if (command.compare("SUB") == 0) {
    if (!TopicManager::isValidTopicFilter(arg)) {
      sendErrorMsg(conn, arg + " is a invalid topic.", false);
      return;
    }

    if (!accessController.allowSubscribe(arg)) {
      // TODO: Disconnect.
      sendErrorMsg(conn, "Insufficient access", false);
      return;
    }

    wrk->subscribeConnection(conn, arg);
  }

  // Unsubscribe from a channel.
  else if (command.compare("UNSUB") == 0) {

  }

  // Unsubscribe from all channels.
  else if (command.compare("UNSUBALL") == 0) {
  }

  // Publish to a channel.
  else if (command.compare("PUB") == 0) {
    auto p = arg.find_first_of(' ');

    if (p == string::npos || p == arg.length()-1) {
      sendErrorMsg(conn, "Payload cannot be blank", false);
      return;
    }

    auto topicName = arg.substr(0, p);
    auto payload = arg.substr(p+1, string::npos);

    if (!accessController.allowPublish(topicName)) {
      sendErrorMsg(conn, "Insufficient access to '" + topicName + "'", false);
      return;
    }

    if (!TopicManager::isValidTopicFilter(topicName)) {
      sendErrorMsg(conn, topicName + " is a invalid topic.", false);
      return;
    }


    try {
      auto cacheId = wrk->getServer()->getRedis().CacheMessage(topicName, payload);
      if (cacheId.length() == 0) {
        LOG(ERROR) << "Failed to cache message to Redis.";
        sendErrorMsg(conn, "Failed to cache message to Redis. Discarding.", false);
        return;
      }

      wrk->getServer()->getRedis().PublishMessage(topicName, cacheId, payload);
    } catch(std::exception &e) {
      LOG(ERROR) << "Redis error while publishing message: " << e.what();
      sendErrorMsg(conn, "Redis error while publishing message.", false);
      return;
    }
  }

  // List all subscribed channels.
  else if (command.compare("LIST") == 0) {
  } else {
    sendErrorMsg(conn, "Unknown command", false);
  }
}

}
} // namespace eventhub
