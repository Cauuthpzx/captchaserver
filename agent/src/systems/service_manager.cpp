#include "service_manager.h"
#include "utils/logger.h"
#include <string>

namespace agent {

SERVICE_STATUS_HANDLE ServiceManager::s_status_handle = nullptr;
SERVICE_STATUS ServiceManager::s_status = {};
Config ServiceManager::s_config;
std::function<void()> ServiceManager::s_stop_cb;

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), len);
    return ws;
}

bool ServiceManager::install_service(const Config& config) {
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        Logger::error("cannot open SCM: " + std::to_string(GetLastError()));
        return false;
    }

    std::wstring svc_name = to_wide(config.service_name);
    std::wstring svc_display = to_wide(config.service_display);

    // Command line: exe --service
    std::wstring cmd = std::wstring(exe_path) + L" --service";

    SC_HANDLE svc = CreateServiceW(scm,
        svc_name.c_str(), svc_display.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        cmd.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_EXISTS) {
            Logger::info("service already installed");
            return true;
        }
        Logger::error("CreateService failed: " + std::to_string(err));
        return false;
    }

    // Set failure actions: restart on failure
    SC_ACTION actions[3] = {
        {SC_ACTION_RESTART, 60000},   // restart after 60s
        {SC_ACTION_RESTART, 120000},  // restart after 2min
        {SC_ACTION_RESTART, 300000},  // restart after 5min
    };
    SERVICE_FAILURE_ACTIONSW fa = {};
    fa.dwResetPeriod = 86400; // 1 day
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    // Set description
    SERVICE_DESCRIPTIONW desc;
    desc.lpDescription = const_cast<LPWSTR>(L"Manages input method language profiles and keyboard layout switching");
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Start the service
    StartServiceW(svc, 0, nullptr);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    Logger::info("service installed and started");
    return true;
}

bool ServiceManager::uninstall_service(const Config& config) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return false;

    std::wstring svc_name = to_wide(config.service_name);
    SC_HANDLE svc = OpenServiceW(scm, svc_name.c_str(), SERVICE_ALL_ACCESS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status;
    ControlService(svc, SERVICE_CONTROL_STOP, &status);
    Sleep(1000);
    BOOL result = DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result != FALSE;
}

bool ServiceManager::is_service_installed(const Config& config) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    std::wstring svc_name = to_wide(config.service_name);
    SC_HANDLE svc = OpenServiceW(scm, svc_name.c_str(), SERVICE_QUERY_STATUS);
    bool installed = (svc != nullptr);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return installed;
}

bool ServiceManager::is_running_as_service() {
    // Check if stdout is attached to console
    // Services don't have a console
    DWORD proc_id = 0;
    HWND console = GetConsoleWindow();
    if (!console) return true; // No console = likely service
    GetWindowThreadProcessId(console, &proc_id);
    return proc_id != GetCurrentProcessId();
}

void WINAPI ServiceManager::service_main(DWORD, LPWSTR*) {
    std::wstring svc_name = to_wide(s_config.service_name);

    s_status_handle = RegisterServiceCtrlHandlerW(svc_name.c_str(), service_ctrl_handler);
    if (!s_status_handle) return;

    s_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    s_status.dwCurrentState = SERVICE_RUNNING;
    s_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(s_status_handle, &s_status);
}

void WINAPI ServiceManager::service_ctrl_handler(DWORD ctrl) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        s_status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(s_status_handle, &s_status);
        // Signal main loop to stop (K3 fix)
        if (s_stop_cb) {
            s_stop_cb();
        }
        break;
    }
}

void ServiceManager::run_as_service(const Config& config) {
    s_config = config;
    std::wstring svc_name = to_wide(config.service_name);

    SERVICE_TABLE_ENTRYW dispatch_table[] = {
        { const_cast<LPWSTR>(svc_name.c_str()), service_main },
        { nullptr, nullptr }
    };

    StartServiceCtrlDispatcherW(dispatch_table);
}

} // namespace agent
