#pragma once
// ============================================================
//  Logger.h  –  Thread-safe file logger + crash dump handler
//  Zero external dependencies. Uses only Windows API + CRT.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdarg>
#include <string>

// ─── Log levels ──────────────────────────────────────────────
enum class LogLevel : uint8_t
{
    Info  = 0,
    Warn  = 1,
    Error = 2,
    Fatal = 3,
};

// ─── Logger ──────────────────────────────────────────────────
class Logger
{
public:
    // Call once at startup. Creates the log file and installs
    // the unhandled-exception / crash-dump handler.
    static bool Init(const std::string& logDirectory);

    // Flush and close the log file. Called automatically on
    // normal exit via atexit(); call explicitly for early exit.
    static void Shutdown();

    // Core write (fmt is printf-style)
    static void Write(LogLevel level, const char* fmt, ...);
    static void WriteV(LogLevel level, const char* fmt, va_list args);

    // Convenience wrappers
    static void Info (const char* fmt, ...);
    static void Warn (const char* fmt, ...);
    static void Error(const char* fmt, ...);
    static void Fatal(const char* fmt, ...);  // writes + triggers crash dump

    // Returns absolute path to the current log file
    static std::string GetLogPath();
    static std::string GetCrashDir();

    // Called by the SEH filter; public so it can be called manually in tests
    static LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* ep);

private:
    Logger()  = delete;
    ~Logger() = delete;

    static void        RotateIfNeeded();
    static bool        OpenLogFile();
    static void        CloseLogFile();
    static void        WriteRaw(const char* line, size_t len);
    static void        WriteCrashLog(EXCEPTION_POINTERS* ep);
    static bool        WriteMiniDump(EXCEPTION_POINTERS* ep,
                                     const std::string& dumpPath);
    static std::string TimestampNow(bool forFileName);
    static std::string LevelStr(LogLevel l);
};
