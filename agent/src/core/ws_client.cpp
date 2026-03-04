#include "ws_client.h"
#include "protocol.h"
#include "utils/logger.h"
#include "utils/xorstr.h"
#include <sstream>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")

namespace agent {

WSClient::WSClient(const Config& config) : m_config(config) {}

WSClient::~WSClient() {
    stop();
}

void WSClient::start() {
    if (m_running.load()) return;
    m_running.store(true);
    m_conn_thread = std::thread(&WSClient::connection_loop, this);
}

void WSClient::stop() {
    m_running.store(false);
    m_connected.store(false);
    cleanup_connection();
    if (m_heartbeat_thread.joinable()) m_heartbeat_thread.join();
    if (m_conn_thread.joinable()) m_conn_thread.join();
}

void WSClient::connection_loop() {
    int backoff_sec = m_config.reconnect_min_sec;

    while (m_running.load()) {
        Logger::info("attempting connection to " + m_config.server_host + ":" + std::to_string(m_config.server_port));

        ULONGLONG connect_start = GetTickCount64();

        if (connect_once()) {
            backoff_sec = m_config.reconnect_min_sec;
            m_connected.store(true);

            // Start heartbeat
            m_heartbeat_thread = std::thread(&WSClient::heartbeat_loop, this);

            // Block on read loop
            read_loop();

            // Disconnected
            m_connected.store(false);
            if (m_heartbeat_thread.joinable()) m_heartbeat_thread.join();

            if (m_on_disconnect) m_on_disconnect();
            Logger::info("disconnected from server");

            // If connection lasted < 5s, don't reset backoff (server cycling)
            ULONGLONG elapsed = GetTickCount64() - connect_start;
            if (elapsed >= 5000) {
                backoff_sec = m_config.reconnect_min_sec;
            }
        } else {
            Logger::error("connection failed, retry in " + std::to_string(backoff_sec) + "s");
        }

        cleanup_connection();

        if (!m_running.load()) break;

        // Exponential backoff
        for (int i = 0; i < backoff_sec * 10 && m_running.load(); ++i) {
            Sleep(100);
        }
        backoff_sec = (std::min)(backoff_sec * 2, m_config.reconnect_max_sec);
    }
}

bool WSClient::connect_once() {
    // Convert host to wide string using proper API
    int wlen = MultiByteToWideChar(CP_UTF8, 0, m_config.server_host.c_str(), -1, nullptr, 0);
    std::wstring wide_host(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, m_config.server_host.c_str(), -1, wide_host.data(), wlen);

    auto ua = xorwstr(L"Mozilla/5.0 (Windows NT 10.0)").decrypt();
    m_session = WinHttpOpen(ua.c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!m_session) {
        Logger::error("WinHttpOpen failed: " + std::to_string(GetLastError()));
        return false;
    }

    m_connection = WinHttpConnect(m_session, wide_host.c_str(), m_config.server_port, 0);
    if (!m_connection) {
        Logger::error("WinHttpConnect failed: " + std::to_string(GetLastError()));
        return false;
    }

    DWORD flags = m_config.use_tls ? WINHTTP_FLAG_SECURE : 0;
    m_request = WinHttpOpenRequest(m_connection, L"GET", L"/ws",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!m_request) {
        Logger::error("WinHttpOpenRequest failed: " + std::to_string(GetLastError()));
        return false;
    }

    // Set WebSocket upgrade
    if (!WinHttpSetOption(m_request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        Logger::error("WinHttpSetOption upgrade failed: " + std::to_string(GetLastError()));
        return false;
    }

    // Set timeouts: connect 30s, receive 90s (detect dead connections)
    DWORD connect_timeout = 30000;
    DWORD receive_timeout = 90000;
    WinHttpSetOption(m_request, WINHTTP_OPTION_CONNECT_TIMEOUT, &connect_timeout, sizeof(connect_timeout));
    WinHttpSetOption(m_request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &receive_timeout, sizeof(receive_timeout));

    if (!WinHttpSendRequest(m_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        Logger::error("WinHttpSendRequest failed: " + std::to_string(GetLastError()));
        return false;
    }

    if (!WinHttpReceiveResponse(m_request, nullptr)) {
        Logger::error("WinHttpReceiveResponse failed: " + std::to_string(GetLastError()));
        return false;
    }

    // Complete the WebSocket upgrade
    HINTERNET ws_handle = WinHttpWebSocketCompleteUpgrade(m_request, 0);
    if (!ws_handle) {
        Logger::error("WebSocket upgrade failed: " + std::to_string(GetLastError()));
        return false;
    }

    // Close the old request handle, use WebSocket handle
    WinHttpCloseHandle(m_request);
    m_request = ws_handle;
    m_is_websocket.store(true);

    // Set receive timeout on WebSocket handle too
    WinHttpSetOption(m_request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &receive_timeout, sizeof(receive_timeout));

    // Send auth message
    AuthMessage auth;
    auth.agent_id = m_config.agent_id;
    auth.token = m_config.agent_token;
    auth.hwid = m_config.hwid;
    std::string auth_json = Protocol::serialize_auth(auth);

    if (!send_raw(auth_json)) {
        Logger::error("failed to send auth");
        return false;
    }

    // Read auth response
    BYTE buffer[4096];
    DWORD bytes_read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE buf_type;
    DWORD err = WinHttpWebSocketReceive(m_request, buffer, sizeof(buffer) - 1, &bytes_read, &buf_type);
    if (err != NO_ERROR) {
        Logger::error("auth response read failed: " + std::to_string(err));
        return false;
    }
    buffer[bytes_read] = '\0';
    std::string response(reinterpret_cast<char*>(buffer), bytes_read);

    auto auth_result = Protocol::parse_auth_result(response);
    if (!auth_result.success) {
        Logger::error("auth failed: " + auth_result.message);
        if (m_on_auth_result) m_on_auth_result(false, auth_result.message);
        return false;
    }

    Logger::info("authenticated successfully");
    if (m_on_auth_result) m_on_auth_result(true, "");
    return true;
}

void WSClient::read_loop() {
    constexpr DWORD k_BufSize = 8192;
    BYTE buffer[k_BufSize];
    std::string accumulated;

    while (m_running.load() && m_connected.load()) {
        DWORD bytes_read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE buf_type;

        DWORD err = WinHttpWebSocketReceive(m_request, buffer, k_BufSize - 1, &bytes_read, &buf_type);
        if (err != NO_ERROR) {
            if (m_running.load()) {
                Logger::error("ws receive error: " + std::to_string(err));
            }
            return;
        }

        if (buf_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            Logger::info("server closed connection");
            return;
        }

        accumulated.append(reinterpret_cast<char*>(buffer), bytes_read);

        if (buf_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            buf_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            handle_message(accumulated);
            accumulated.clear();
        }
        // If FRAGMENT, continue accumulating
    }
}

void WSClient::heartbeat_loop() {
    while (m_running.load() && m_connected.load()) {
        for (int i = 0; i < m_config.heartbeat_interval_sec * 10; ++i) {
            if (!m_running.load() || !m_connected.load()) return;
            Sleep(100);
        }
        if (m_connected.load()) {
            if (!send_raw(Protocol::serialize_heartbeat())) {
                Logger::error("heartbeat send failed, triggering reconnect");
                m_connected.store(false);
                return;
            }
        }
    }
}

bool WSClient::send_raw(const std::string& data) {
    std::lock_guard lock(m_send_mutex);
    if (!m_request) return false;
    DWORD err = WinHttpWebSocketSend(m_request,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(data.c_str()),
        static_cast<DWORD>(data.size()));
    return err == NO_ERROR;
}

void WSClient::handle_message(const std::string& data) {
    auto msg_type = Protocol::parse_type(data);

    switch (msg_type) {
    case MessageType::CaptchaChallenge: {
        auto challenge = Protocol::parse_captcha_challenge(data);
        Logger::info("received captcha challenge id=" + challenge.id);
        if (m_on_captcha) m_on_captcha(challenge);
        break;
    }
    case MessageType::CaptchaResult: {
        auto result = Protocol::parse_captcha_result(data);
        Logger::info("captcha result id=" + result.id + " correct=" + (result.correct ? "true" : "false"));
        break;
    }
    case MessageType::ConfigUpdate: {
        auto config = Protocol::parse_config_update(data);
        Logger::info("config update: timeout=" + std::to_string(config.timeout_sec));
        break;
    }
    case MessageType::RemoteCommand: {
        auto cmd = Protocol::parse_remote_command(data);
        Logger::info("remote command: " + cmd.command);
        if (m_on_remote_command) m_on_remote_command(cmd.command);
        break;
    }
    default:
        break;
    }
}

void WSClient::send_captcha_response(const CaptchaResponseMessage& msg) {
    std::string json = Protocol::serialize_captcha_response(msg);
    if (!send_raw(json)) {
        Logger::error("failed to send captcha response");
    }
}

void WSClient::send_alert(const std::string& alert_type, const std::string& message) {
    AgentAlertMessage alert;
    alert.alert_type = alert_type;
    alert.message = message;
    std::string json = Protocol::serialize_alert(alert);
    if (!send_raw(json)) {
        Logger::error("failed to send alert: " + alert_type);
    }
}

void WSClient::cleanup_connection() {
    // Lock send_mutex to prevent send_raw from using closed handles (T4 fix)
    std::lock_guard lock(m_send_mutex);

    if (m_request) {
        // Only attempt graceful WebSocket close if handle is actually a WebSocket (M3 fix)
        if (m_is_websocket.load()) {
            WinHttpWebSocketClose(m_request, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        }
        WinHttpCloseHandle(m_request);
        m_request = nullptr;
        m_is_websocket.store(false);
    }
    if (m_connection) {
        WinHttpCloseHandle(m_connection);
        m_connection = nullptr;
    }
    if (m_session) {
        WinHttpCloseHandle(m_session);
        m_session = nullptr;
    }
}

} // namespace agent
