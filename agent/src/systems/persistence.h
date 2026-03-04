#pragma once

#include "agent/config.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>

#include <windows.h>

namespace agent {

using AlertCallback = std::function<void(const std::string& alert_type, const std::string& message)>;

// Persistence: ensures the agent survives reboots and removal attempts
class Persistence {
public:
    // Add to startup via registry
    static bool add_to_startup(const Config& config);
    static bool remove_from_startup(const Config& config);

    // Copy self to a persistent location
    static std::string install_to_appdata(const Config& config);

    // Self-repair: check if files are intact and restore if needed
    static void verify_integrity();

    // Monitor for uninstall/tamper attempts (runs in background thread)
    static void start_monitoring(const Config& config, AlertCallback on_alert);
    static void stop_monitoring();

private:
    static std::string get_install_dir(const Config& config);

    static std::thread s_monitor_thread;
    static std::atomic<bool> s_monitoring;
    static HANDLE s_wake_event;
};

} // namespace agent
