#pragma once

#include "agent/config.h"
#include "agent/types.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

#include <windows.h>
#include <winhttp.h>

namespace agent {

class WSClient {
public:
    explicit WSClient(const Config& config);
    ~WSClient();

    WSClient(const WSClient&) = delete;
    WSClient& operator=(const WSClient&) = delete;

    void set_on_captcha(OnCaptchaCallback cb) { m_on_captcha = std::move(cb); }
    void set_on_auth_result(OnAuthResultCallback cb) { m_on_auth_result = std::move(cb); }
    void set_on_disconnect(OnDisconnectCallback cb) { m_on_disconnect = std::move(cb); }
    void set_on_remote_command(OnRemoteCommandCallback cb) { m_on_remote_command = std::move(cb); }

    void start();
    void stop();
    bool is_connected() const { return m_connected.load(); }

    void send_captcha_response(const CaptchaResponseMessage& msg);
    void send_alert(const std::string& alert_type, const std::string& message);

private:
    void connection_loop();
    bool connect_once();
    void read_loop();
    void heartbeat_loop();
    bool send_raw(const std::string& data);
    void handle_message(const std::string& data);
    void cleanup_connection();

    Config m_config;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_is_websocket{false};

    HINTERNET m_session = nullptr;
    HINTERNET m_connection = nullptr;
    HINTERNET m_request = nullptr;

    std::mutex m_send_mutex;
    std::thread m_conn_thread;
    std::thread m_heartbeat_thread;

    OnCaptchaCallback m_on_captcha;
    OnAuthResultCallback m_on_auth_result;
    OnDisconnectCallback m_on_disconnect;
    OnRemoteCommandCallback m_on_remote_command;
};

} // namespace agent
