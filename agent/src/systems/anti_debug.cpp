#include "anti_debug.h"
#include "utils/logger.h"

namespace agent {

AntiDebug::AntiDebug() = default;

AntiDebug::~AntiDebug() {
    stop();
}

void AntiDebug::start() {
    // No-op — all anti-debug APIs removed to avoid AV behavioral detection
}

void AntiDebug::stop() {
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

bool AntiDebug::check_debugger() const { return false; }
bool AntiDebug::check_remote_debugger() const { return false; }
bool AntiDebug::check_hardware_breakpoints() const { return false; }
bool AntiDebug::check_timing() const { return false; }

} // namespace agent
