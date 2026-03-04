#pragma once

#include <string>
#include <atomic>
#include <thread>

#include <windows.h>

namespace agent {

// Watchdog uses Windows Task Scheduler to ensure the app restarts if killed.
// Same mechanism used by Chrome, Spotify, Adobe updaters, etc.
class Watchdog {
public:
    Watchdog();
    ~Watchdog();

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    void start();
    void stop();

    // Legacy entry point — now a no-op
    static void run_as_watchdog(DWORD target_pid, const std::string& exe_path);

private:
    static bool create_scheduled_task(const std::string& task_name, const std::string& exe_path);
    static bool delete_scheduled_task(const std::string& task_name);

    std::string m_task_name;
};

} // namespace agent
