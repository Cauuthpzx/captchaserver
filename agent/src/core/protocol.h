#pragma once

#include "agent/types.h"
#include <string>

namespace agent {

// JSON serialization/deserialization for WebSocket messages
class Protocol {
public:
    static std::string serialize_auth(const AuthMessage& msg);
    static std::string serialize_captcha_response(const CaptchaResponseMessage& msg);
    static std::string serialize_heartbeat();
    static std::string serialize_alert(const AgentAlertMessage& msg);

    static MessageType parse_type(const std::string& json);
    static AuthResultMessage parse_auth_result(const std::string& json);
    static CaptchaChallengeMessage parse_captcha_challenge(const std::string& json);
    static CaptchaResultMessage parse_captcha_result(const std::string& json);
    static ConfigUpdateMessage parse_config_update(const std::string& json);
    static RemoteCommandMessage parse_remote_command(const std::string& json);

private:
    static std::string find_string(const std::string& json, const std::string& key);
    static int find_int(const std::string& json, const std::string& key, int default_val = 0);
    static bool find_bool(const std::string& json, const std::string& key, bool default_val = false);
};

} // namespace agent
