#include "../../src/platform/platform.h"
#include <chrono>

// Only platform_millis() is implemented so far -- it's the one primitive
// the ported ui/data files actually call today (via platform.h's millis()
// shim). Mutex/HTTP/config storage land in task #4 of the Pi port (see
// project_pi_port memory) once http_mutex.cpp/storage.cpp get migrated for
// real; nothing calls them yet.

uint32_t platform_millis() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - start).count();
}
