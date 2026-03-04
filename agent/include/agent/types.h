#pragma once

#include <string>
#include <cstdint>
#include <functional>

namespace agent {

enum class MessageType {
    Auth,
    AuthResult,
    CaptchaChallenge,
    CaptchaResponse,
    CaptchaResult,
    Heartbeat,
    ConfigUpdate,
    RemoteCommand,
    Unknown
};

struct AuthMessage {
    std::string agent_id;
    std::string token;
};

struct AuthResultMessage {
    bool success = false;
    std::string message;
};

struct CaptchaChallengeMessage {
    std::string id;
    std::string image_base64;
    int timeout_sec = 60;
    std::string popup_message;
};

struct CaptchaResponseMessage {
    std::string id;
    std::string answer;
    bool is_miss = false;
    int64_t response_ms = 0;
};

struct CaptchaResultMessage {
    std::string id;
    bool correct = false;
};

struct ConfigUpdateMessage {
    int timeout_sec = 60;
};

struct AgentAlertMessage {
    std::string alert_type;  // "uninstall_attempt", "close_popup", "tamper"
    std::string message;
};

struct RemoteCommandMessage {
    std::string command;  // "uninstall", "restart"
};

using OnCaptchaCallback = std::function<void(const CaptchaChallengeMessage&)>;
using OnAuthResultCallback = std::function<void(bool success, const std::string& message)>;
using OnDisconnectCallback = std::function<void()>;
using OnRemoteCommandCallback = std::function<void(const std::string& command)>;

} // namespace agent
