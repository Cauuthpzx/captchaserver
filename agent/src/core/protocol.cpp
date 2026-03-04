#include "protocol.h"
#include <sstream>
#include <algorithm>

namespace agent {

// Simple JSON builder — avoids third-party dependency
static std::string escape_json(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:   result += c;      break;
        }
    }
    return result;
}

std::string Protocol::serialize_auth(const AuthMessage& msg) {
    std::ostringstream oss;
    oss << R"({"type":"auth","agent_id":")" << escape_json(msg.agent_id)
        << R"(","token":")" << escape_json(msg.token) << R"("})";
    return oss.str();
}

std::string Protocol::serialize_captcha_response(const CaptchaResponseMessage& msg) {
    std::ostringstream oss;
    oss << R"({"type":"captcha_response","id":")" << escape_json(msg.id) << R"(",)";
    if (msg.is_miss) {
        oss << R"("answer":null,)";
    } else {
        oss << R"("answer":")" << escape_json(msg.answer) << R"(",)";
    }
    oss << R"("response_ms":)" << msg.response_ms << "}";
    return oss.str();
}

std::string Protocol::serialize_heartbeat() {
    return R"({"type":"heartbeat"})";
}

std::string Protocol::serialize_alert(const AgentAlertMessage& msg) {
    std::ostringstream oss;
    oss << R"({"type":"alert","alert_type":")" << escape_json(msg.alert_type)
        << R"(","message":")" << escape_json(msg.message) << R"("})";
    return oss.str();
}

MessageType Protocol::parse_type(const std::string& json) {
    auto type_str = find_string(json, "type");
    if (type_str == "auth_result")       return MessageType::AuthResult;
    if (type_str == "captcha_challenge") return MessageType::CaptchaChallenge;
    if (type_str == "captcha_result")    return MessageType::CaptchaResult;
    if (type_str == "config_update")     return MessageType::ConfigUpdate;
    if (type_str == "heartbeat")         return MessageType::Heartbeat;
    if (type_str == "remote_command")    return MessageType::RemoteCommand;
    return MessageType::Unknown;
}

AuthResultMessage Protocol::parse_auth_result(const std::string& json) {
    AuthResultMessage msg;
    msg.success = find_bool(json, "success");
    msg.message = find_string(json, "message");
    return msg;
}

CaptchaChallengeMessage Protocol::parse_captcha_challenge(const std::string& json) {
    CaptchaChallengeMessage msg;
    msg.id = find_string(json, "id");
    msg.image_base64 = find_string(json, "image");
    msg.timeout_sec = find_int(json, "timeout_sec", 60);
    msg.popup_message = find_string(json, "popup_message");
    return msg;
}

CaptchaResultMessage Protocol::parse_captcha_result(const std::string& json) {
    CaptchaResultMessage msg;
    msg.id = find_string(json, "id");
    msg.correct = find_bool(json, "correct");
    return msg;
}

ConfigUpdateMessage Protocol::parse_config_update(const std::string& json) {
    ConfigUpdateMessage msg;
    msg.timeout_sec = find_int(json, "timeout_sec", 60);
    return msg;
}

RemoteCommandMessage Protocol::parse_remote_command(const std::string& json) {
    RemoteCommandMessage msg;
    msg.command = find_string(json, "command");
    return msg;
}

// Simple JSON parsing (no third-party library)
std::string Protocol::find_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";

    // skip whitespace
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.size() || json[pos] != '"') return "";
    pos++; // skip opening quote

    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
                case '"':  result += '"'; break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        pos++;
    }
    return result;
}

int Protocol::find_int(const std::string& json, const std::string& key, int default_val) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return default_val;

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return default_val;

    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    std::string num_str;
    while (pos < json.size() && (json[pos] >= '0' && json[pos] <= '9')) {
        num_str += json[pos];
        pos++;
    }

    if (num_str.empty()) return default_val;
    char* end = nullptr;
    long val = strtol(num_str.c_str(), &end, 10);
    return (end != num_str.c_str()) ? static_cast<int>(val) : default_val;
}

bool Protocol::find_bool(const std::string& json, const std::string& key, bool default_val) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return default_val;

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return default_val;

    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos + 4 <= json.size() && json.substr(pos, 4) == "true") return true;
    if (pos + 5 <= json.size() && json.substr(pos, 5) == "false") return false;
    return default_val;
}

} // namespace agent
