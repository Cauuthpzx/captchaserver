#pragma once

#include "agent/config.h"
#include <string>
#include <functional>
#include <atomic>

#include <windows.h>

namespace agent {

class ServiceManager {
public:
    // Install as Windows Service
    static bool install_service(const Config& config);
    static bool uninstall_service(const Config& config);
    static bool is_service_installed(const Config& config);

    // Run as service (called from main when started by SCM)
    static void run_as_service(const Config& config);

    // Set callback to signal main loop on service stop (K3 fix)
    static void set_stop_callback(std::function<void()> cb) { s_stop_cb = std::move(cb); }

    // Check if running as service
    static bool is_running_as_service();

    // Service control handler
    static void WINAPI service_main(DWORD argc, LPWSTR* argv);
    static void WINAPI service_ctrl_handler(DWORD ctrl);

private:
    static SERVICE_STATUS_HANDLE s_status_handle;
    static SERVICE_STATUS s_status;
    static Config s_config;
    static std::function<void()> s_stop_cb;
};

} // namespace agent
