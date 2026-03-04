#pragma once

#include <string>
#include <atomic>
#include <thread>

#include <windows.h>

namespace agent {

// Mutual watchdog: monitors the main process and restarts if killed.
// The main process also monitors the watchdog.
class Watchdog {
public:
    Watchdog();
    ~Watchdog();

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    void start();
    void stop();

    // Create a watchdog child process, returns process handle
    static HANDLE spawn_watchdog(const std::string& exe_path, DWORD parent_pid);

    // Entry point when running as watchdog
    static void run_as_watchdog(DWORD target_pid, const std::string& exe_path);

private:
    void monitor_loop();

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    HANDLE m_watchdog_process = nullptr;
};

} // namespace agent
