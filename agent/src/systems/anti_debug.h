#pragma once

#include <atomic>
#include <thread>

#include <windows.h>

namespace agent {

class AntiDebug {
public:
    AntiDebug();
    ~AntiDebug();

    AntiDebug(const AntiDebug&) = delete;
    AntiDebug& operator=(const AntiDebug&) = delete;

    void start();
    void stop();
    bool is_debugger_detected() const { return m_detected.load(); }

private:
    void check_loop();
    bool check_debugger() const;
    bool check_remote_debugger() const;
    bool check_hardware_breakpoints() const;
    bool check_timing() const;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_detected{false};
    std::thread m_thread;
};

} // namespace agent
