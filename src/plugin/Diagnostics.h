#pragma once

// Opt-in render log, compiled out unless ABGLOW_DIAGNOSTICS is defined. It
// exists because the mock host in tests/ is written from the same assumptions
// as the plug-in, so it cannot catch a misunderstanding of what After Effects
// actually asks for. This records that, from inside a real session.
namespace abglow {
namespace diag {

#ifdef ABGLOW_DIAGNOSTICS

bool Enabled();
void Log(const char* format, ...);
void Frame();

#else

inline bool Enabled() { return false; }
inline void Log(const char*, ...) {}
inline void Frame() {}

#endif

}  // namespace diag
}  // namespace abglow
