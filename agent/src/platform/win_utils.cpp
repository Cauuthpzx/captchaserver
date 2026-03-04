#include "win_utils.h"
#include "utils/logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>

#include <aclapi.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace agent {

void WinUtils::hide_process() {
    // Set process priority to below normal to reduce visibility in resource monitors
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    // Hide console window if any
    HWND console = GetConsoleWindow();
    if (console) {
        ShowWindow(console, SW_HIDE);
    }
}

void WinUtils::protect_process() {
    // Deny PROCESS_TERMINATE to Everyone — makes it harder to kill via Task Manager
    // (admin/SYSTEM can still override with SeDebugPrivilege)
    HANDLE process = GetCurrentProcess();
    PACL old_dacl = nullptr;
    PACL new_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;

    if (GetSecurityInfo(process, SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd) == ERROR_SUCCESS) {

        EXPLICIT_ACCESSW deny_access = {};
        deny_access.grfAccessPermissions = PROCESS_TERMINATE | PROCESS_SUSPEND_RESUME |
            PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD;
        deny_access.grfAccessMode = DENY_ACCESS;
        deny_access.grfInheritance = NO_INHERITANCE;
        deny_access.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
        deny_access.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        deny_access.Trustee.ptstrName = const_cast<LPWSTR>(L"Everyone");

        if (SetEntriesInAclW(1, &deny_access, old_dacl, &new_dacl) == ERROR_SUCCESS) {
            SetSecurityInfo(process, SE_KERNEL_OBJECT,
                DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
            LocalFree(new_dacl);
        }
        LocalFree(sd);
    }
    Logger::info("process protection applied");
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS*) {
    // Restart self on crash, preserving command-line args (A1 fix)
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring cmd = L"\"";
    cmd += exe_path;
    cmd += L"\"";

    // Preserve original command line args
    LPWSTR original_cmd = GetCommandLineW();
    if (original_cmd) {
        // Skip past the executable name in the command line
        LPWSTR args = original_cmd;
        if (*args == L'"') {
            args++; // skip opening quote
            while (*args && *args != L'"') args++;
            if (*args) args++; // skip closing quote
        } else {
            while (*args && *args != L' ') args++;
        }
        // Skip whitespace
        while (*args == L' ') args++;
        if (*args) {
            cmd += L" ";
            cmd += args;
        }
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);

    return EXCEPTION_EXECUTE_HANDLER; // Terminate this instance
}

void WinUtils::install_crash_handler() {
    SetUnhandledExceptionFilter(crash_handler);
    Logger::info("crash handler installed");
}

std::string WinUtils::get_hostname() {
    char buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) {
        return std::string(buf, size);
    }
    return "unknown";
}

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool WinUtils::read_config_file(const std::string& path, std::string& server_host,
                                  uint16_t& server_port, std::string& agent_id,
                                  std::string& agent_token) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));

        if (key == "server_host") server_host = value;
        else if (key == "server_port") {
            char* end = nullptr;
            long port = strtol(value.c_str(), &end, 10);
            if (end != value.c_str() && port > 0 && port <= 65535) {
                server_port = static_cast<uint16_t>(port);
            }
        }
        else if (key == "agent_id") agent_id = value;
        else if (key == "agent_token") agent_token = value;
    }

    return !agent_id.empty() && !agent_token.empty();
}

HANDLE WinUtils::create_single_instance_mutex(const std::string& name) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    std::wstring wide(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), wlen);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, wide.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return nullptr; // Another instance is running
    }
    return mutex;
}

} // namespace agent
