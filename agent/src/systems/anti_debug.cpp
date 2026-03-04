#include "anti_debug.h"
#include "utils/logger.h"

namespace agent {

AntiDebug::AntiDebug() = default;

AntiDebug::~AntiDebug() {
    stop();
}

void AntiDebug::start() {
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&AntiDebug::check_loop, this);
}

void AntiDebug::stop() {
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

void AntiDebug::check_loop() {
    while (m_running.load()) {
        if (check_debugger() || check_remote_debugger() || check_hardware_breakpoints()) {
            m_detected.store(true);
            Logger::info("debugger detected — entering stealth mode");
            // Don't exit — just note it and continue running
            // Could optionally hide the popup or change behavior
        } else {
            m_detected.store(false);
        }

        // Check every 5 seconds
        for (int i = 0; i < 50 && m_running.load(); ++i) {
            Sleep(100);
        }
    }
}

bool AntiDebug::check_debugger() const {
    return IsDebuggerPresent() != FALSE;
}

bool AntiDebug::check_remote_debugger() const {
    BOOL present = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &present);
    return present != FALSE;
}

bool AntiDebug::check_hardware_breakpoints() const {
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE thread = GetCurrentThread();
    if (GetThreadContext(thread, &ctx)) {
        return (ctx.Dr0 != 0 || ctx.Dr1 != 0 || ctx.Dr2 != 0 || ctx.Dr3 != 0);
    }
    return false;
}

bool AntiDebug::check_timing() const {
    ULONGLONG start = GetTickCount64();
    // Dummy work
    volatile int sum = 0;
    for (int i = 0; i < 1000; ++i) sum += i;
    ULONGLONG elapsed = GetTickCount64() - start;
    // Normal execution: < 5ms. Debugging: much longer
    return elapsed > 50;
}

} // namespace agent
