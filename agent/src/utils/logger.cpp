#include "logger.h"
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>

namespace agent {

namespace {
    std::mutex s_log_mutex;
    std::string s_log_path;
    int s_write_count = 0;
    constexpr int k_RotateCheckInterval = 100; // check every 100 writes
    constexpr uintmax_t k_MaxLogSize = 2 * 1024 * 1024; // 2 MB
} // anonymous namespace

void Logger::init(const std::string& log_path) {
    std::lock_guard lock(s_log_mutex);
    s_log_path = log_path;
}

static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &time);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// A5 fix: simple log rotation — rename current to .old, start fresh
static void rotate_if_needed() {
    if (++s_write_count < k_RotateCheckInterval) return;
    s_write_count = 0;

    try {
        auto size = std::filesystem::file_size(s_log_path);
        if (size > k_MaxLogSize) {
            std::string old_path = s_log_path + ".old";
            std::filesystem::remove(old_path);
            std::filesystem::rename(s_log_path, old_path);
        }
    } catch (...) {}
}

static void write_log(const std::string& level, const std::string& msg) {
    std::lock_guard lock(s_log_mutex);
    if (s_log_path.empty()) return;
    rotate_if_needed();
    std::ofstream f(s_log_path, std::ios::app);
    if (f.is_open()) {
        f << "[" << timestamp() << "] [" << level << "] " << msg << "\n";
    }
}

void Logger::info(const std::string& msg) { write_log("INFO", msg); }
void Logger::error(const std::string& msg) { write_log("ERROR", msg); }
void Logger::debug(const std::string& msg) { write_log("DEBUG", msg); }

} // namespace agent
