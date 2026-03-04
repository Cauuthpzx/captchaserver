#pragma once

#include <string>

namespace agent {

class Logger {
public:
    static void init(const std::string& log_path);
    static void info(const std::string& msg);
    static void error(const std::string& msg);
    static void debug(const std::string& msg);
};

} // namespace agent
