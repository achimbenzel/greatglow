#include "Diagnostics.h"

#ifdef ABGLOW_DIAGNOSTICS

#include "AbGlow.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace abglow {
namespace diag {
namespace {

// Bounded so a long session cannot fill the disk.
constexpr long kMaxLines = 20000;

std::mutex& Lock() {
    static std::mutex mutex;
    return mutex;
}

std::string LogPath() {
    const char* temp = std::getenv("TEMP");
    if (temp == nullptr) temp = std::getenv("TMP");
    if (temp == nullptr) temp = ".";
    return std::string(temp) + "/ProfoundGlow-diag.log";
}

long& LineCount() {
    static long lines = 0;
    return lines;
}

std::FILE* Open() {
    static std::FILE* file = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        file = std::fopen(LogPath().c_str(), "w");
        if (file != nullptr) {
            std::fprintf(file, "%s %s (%s) diagnostics\n", AB_GLOW_NAME, AB_GLOW_VERSION_STRING, AB_GLOW_BUILD_ID);
            std::fflush(file);
        }
    }
    return file;
}

}  // namespace

bool Enabled() { return true; }

void Log(const char* format, ...) {
    std::lock_guard<std::mutex> guard(Lock());
    std::FILE* file = Open();
    if (file == nullptr) return;
    if (LineCount() >= kMaxLines) return;
    ++LineCount();
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}

void Frame() { Log("--"); }

}  // namespace diag
}  // namespace abglow

#endif
