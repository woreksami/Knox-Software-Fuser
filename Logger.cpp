// ============================================================
//  Logger.cpp  –  Implementation
//  Thread-safe file logger + SEH crash dump writer
// ============================================================

#include "Logger.h"
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cassert>
#include <algorithm>
#include <shlobj.h>    // SHCreateDirectoryExA

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shell32.lib")

// ─── Internal state ──────────────────────────────────────────
namespace
{
    constexpr size_t   MAX_LOG_BYTES   = 50 * 1024 * 1024; // 50 MB per file
    constexpr size_t   LINE_BUF_SIZE   = 4096;
    constexpr size_t   RING_LINES      = 512;              // kept in memory for crash

    CRITICAL_SECTION   g_cs{};
    bool               g_csInit        = false;
    HANDLE             g_hFile         = INVALID_HANDLE_VALUE;
    size_t             g_bytesWritten  = 0;
    std::string        g_logDir;
    std::string        g_logPath;
    bool               g_initialised   = false;

    // Ring buffer of recent log lines (for crash summary)
    char      g_ring[RING_LINES][LINE_BUF_SIZE]{};
    size_t    g_ringHead = 0;
    size_t    g_ringCount= 0;
}

// ─────────────────────────────────────────────────────────────
//  Internal: timestamp string
// ─────────────────────────────────────────────────────────────
std::string Logger::TimestampNow(bool forFileName)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);

    char buf[64]{};
    if (forFileName)
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "%04d%02d%02d_%02d%02d%02d",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
    else
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    return std::string(buf);
}

std::string Logger::LevelStr(LogLevel l)
{
    switch (l)
    {
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    }
    return "?    ";
}

// ─────────────────────────────────────────────────────────────
//  Internal: open a new log file
// ─────────────────────────────────────────────────────────────
bool Logger::OpenLogFile()
{
    // Create log directory if absent
    SHCreateDirectoryExA(nullptr, g_logDir.c_str(), nullptr);

    std::string ts   = TimestampNow(true);
    g_logPath        = g_logDir + "\\KnoxFuser_" + ts + ".log";
    g_bytesWritten   = 0;

    g_hFile = CreateFileA(
        g_logPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,  // flush every write
        nullptr);

    if (g_hFile == INVALID_HANDLE_VALUE)
    {
        OutputDebugStringA("[Logger] Failed to open log file!\n");
        return false;
    }
    const char header[] =
        "\xEF\xBB\xBF"   // UTF-8 BOM
        "============================================================\n"
        "  Knox Fuser  -  Session Log\n"
        "============================================================\n";
    DWORD written = 0;
    WriteFile(g_hFile, header, static_cast<DWORD>(sizeof(header) - 1), &written, nullptr);
    g_bytesWritten = written;
    return true;
}

void Logger::CloseLogFile()
{
    if (g_hFile != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(g_hFile);
        CloseHandle(g_hFile);
        g_hFile = INVALID_HANDLE_VALUE;
    }
}

// ─────────────────────────────────────────────────────────────
//  Internal: write a pre-formatted line to disk + ring
// ─────────────────────────────────────────────────────────────
void Logger::WriteRaw(const char* line, size_t len)
{
    // Rotate if we hit the size cap
    RotateIfNeeded();

    if (g_hFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(g_hFile, line, static_cast<DWORD>(len), &written, nullptr);
        g_bytesWritten += written;
    }

    // Also surface in the VS output window / DebugView
    OutputDebugStringA(line);

    // Keep in ring buffer for crash summaries
    size_t slot = g_ringHead % RING_LINES;
    _snprintf_s(g_ring[slot], LINE_BUF_SIZE, _TRUNCATE, "%s", line);
    ++g_ringHead;
    if (g_ringCount < RING_LINES) ++g_ringCount;
}

void Logger::RotateIfNeeded()
{
    if (g_bytesWritten < MAX_LOG_BYTES) return;

    CloseLogFile();

    char notice[128]{};
    _snprintf_s(notice, sizeof(notice), _TRUNCATE,
                "[Logger] Log rotated at %zu bytes.\n", g_bytesWritten);
    OutputDebugStringA(notice);

    OpenLogFile();
}

// ─────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────
bool Logger::Init(const std::string& logDirectory)
{
    if (g_initialised) return true;

    InitializeCriticalSectionAndSpinCount(&g_cs, 1000);
    g_csInit = true;

    g_logDir = logDirectory;

    if (!OpenLogFile())
        return false;

    g_initialised = true;

    // Install crash handler
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);

    // Flush / close on normal process exit
    atexit([]{ Logger::Shutdown(); });

    char startMsg[256]{};
    _snprintf_s(startMsg, sizeof(startMsg), _TRUNCATE,
                "[Logger] Session started. Log: %s\n", g_logPath.c_str());
    WriteRaw(startMsg, strlen(startMsg));

    return true;
}

void Logger::Shutdown()
{
    if (!g_initialised) return;
    g_initialised = false;

    EnterCriticalSection(&g_cs);
    const char goodbye[] = "[Logger] Session ended cleanly.\n";
    WriteRaw(goodbye, sizeof(goodbye) - 1);
    CloseLogFile();
    LeaveCriticalSection(&g_cs);

    DeleteCriticalSection(&g_cs);
    g_csInit = false;
}

// ─────────────────────────────────────────────────────────────
//  WriteV: core formatted write
// ─────────────────────────────────────────────────────────────
void Logger::WriteV(LogLevel level, const char* fmt, va_list args)
{
    if (!g_initialised && g_hFile == INVALID_HANDLE_VALUE)
    {
        // Before Init() – fall back to debug output only
        char tmp[LINE_BUF_SIZE]{};
        vsnprintf(tmp, sizeof(tmp), fmt, args);
        OutputDebugStringA(tmp);
        return;
    }

    // Format: [2026-02-27 15:30:00.123] [INFO ] <message>\n
    char msgBuf[LINE_BUF_SIZE - 64]{};
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);

    // Ensure single trailing newline
    size_t mlen = strlen(msgBuf);
    if (mlen > 0 && msgBuf[mlen - 1] != '\n')
    {
        if (mlen + 1 < sizeof(msgBuf))
        {
            msgBuf[mlen]   = '\n';
            msgBuf[mlen+1] = '\0';
        }
    }

    std::string ts  = TimestampNow(false);
    std::string lvl = LevelStr(level);

    char line[LINE_BUF_SIZE]{};
    int lineLen = _snprintf_s(line, sizeof(line), _TRUNCATE,
                              "[%s] [%s] %s",
                              ts.c_str(), lvl.c_str(), msgBuf);
    if (lineLen < 0) lineLen = static_cast<int>(sizeof(line) - 1);

    if (g_csInit) EnterCriticalSection(&g_cs);
    WriteRaw(line, static_cast<size_t>(lineLen));

    // Additionally print to stdout (console) if attached
    fputs(line, stdout);
    fflush(stdout);

    if (g_csInit) LeaveCriticalSection(&g_cs);
}

void Logger::Write(LogLevel level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteV(level, fmt, args);
    va_end(args);
}

void Logger::Info(const char* fmt, ...)
{
    va_list a; va_start(a, fmt);
    WriteV(LogLevel::Info, fmt, a);
    va_end(a);
}

void Logger::Warn(const char* fmt, ...)
{
    va_list a; va_start(a, fmt);
    WriteV(LogLevel::Warn, fmt, a);
    va_end(a);
}

void Logger::Error(const char* fmt, ...)
{
    va_list a; va_start(a, fmt);
    WriteV(LogLevel::Error, fmt, a);
    va_end(a);
}

void Logger::Fatal(const char* fmt, ...)
{
    va_list a; va_start(a, fmt);
    WriteV(LogLevel::Fatal, fmt, a);
    va_end(a);
    // Fatal implies immediate flush; crash dump written by SEH handler
    if (g_csInit) EnterCriticalSection(&g_cs);
    if (g_hFile != INVALID_HANDLE_VALUE) FlushFileBuffers(g_hFile);
    if (g_csInit) LeaveCriticalSection(&g_cs);
}

std::string Logger::GetLogPath()  { return g_logPath; }
std::string Logger::GetCrashDir() { return g_logDir + "\\Crashes"; }

// ─────────────────────────────────────────────────────────────
//  Crash dump writer (dbghelp.dll – loaded dynamically so the
//  app doesn't hard-depend on a specific dbghelp version)
// ─────────────────────────────────────────────────────────────
bool Logger::WriteMiniDump(EXCEPTION_POINTERS* ep,
                            const std::string&  dumpPath)
{
    // Load dbghelp dynamically to avoid version lock
    HMODULE hDbg = LoadLibraryA("dbghelp.dll");
    if (!hDbg) return false;

    using MiniDumpWriteDumpFn =
        BOOL(WINAPI*)(HANDLE, DWORD, HANDLE,
                      MINIDUMP_TYPE,
                      PMINIDUMP_EXCEPTION_INFORMATION,
                      PMINIDUMP_USER_STREAM_INFORMATION,
                      PMINIDUMP_CALLBACK_INFORMATION);

    auto pfn = reinterpret_cast<MiniDumpWriteDumpFn>(
        GetProcAddress(hDbg, "MiniDumpWriteDump"));
    if (!pfn) { FreeLibrary(hDbg); return false; }

    HANDLE hDump = CreateFileA(
        dumpPath.c_str(),
        GENERIC_WRITE,
        0, nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hDump == INVALID_HANDLE_VALUE) { FreeLibrary(hDbg); return false; }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;

    MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs        |
        MiniDumpWithHandleData      |
        MiniDumpWithThreadInfo      |
        MiniDumpWithProcessThreadData);

    bool ok = pfn(GetCurrentProcess(), GetCurrentProcessId(),
                  hDump, dumpType, (ep ? &mei : nullptr),
                  nullptr, nullptr) != FALSE;

    CloseHandle(hDump);
    FreeLibrary(hDbg);
    return ok;
}

// ─────────────────────────────────────────────────────────────
//  Crash log: human-readable summary of the last N lines
// ─────────────────────────────────────────────────────────────
void Logger::WriteCrashLog(EXCEPTION_POINTERS* ep)
{
    std::string crashDir = GetCrashDir();
    SHCreateDirectoryExA(nullptr, crashDir.c_str(), nullptr);

    std::string ts       = TimestampNow(true);
    std::string logPath  = crashDir + "\\crash_" + ts + ".log";
    std::string dumpPath = crashDir + "\\crash_" + ts + ".dmp";

    // ── Write crash_XXXXXX.log ────────────────────────────────
    HANDLE hF = CreateFileA(
        logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hF != INVALID_HANDLE_VALUE)
    {
        auto wr = [&](const char* s, size_t n)
        {
            DWORD d = 0;
            WriteFile(hF, s, static_cast<DWORD>(n), &d, nullptr);
        };

        // Header
        char hdr[512]{};
        int  hdrLen = 0;

        if (ep && ep->ExceptionRecord)
        {
            hdrLen = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                "============================================================\n"
                "  KNOX FUSER  -  CRASH REPORT\n"
                "  Timestamp  : %s\n"
                "  Exception  : 0x%08X\n"
                "  Address    : 0x%p\n"
                "  Flags      : 0x%08X\n"
                "============================================================\n\n"
                "--- Last %zu log lines ---\n",
                ts.c_str(),
                ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress,
                ep->ExceptionRecord->ExceptionFlags,
                g_ringCount);
        }
        else
        {
            hdrLen = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                "============================================================\n"
                "  KNOX FUSER  -  CRASH REPORT  (no exception info)\n"
                "  Timestamp  : %s\n"
                "============================================================\n\n"
                "--- Last %zu log lines ---\n",
                ts.c_str(), g_ringCount);
        }
        if (hdrLen > 0) wr(hdr, static_cast<size_t>(hdrLen));

        // Dump ring buffer in chronological order
        size_t start = (g_ringCount >= RING_LINES)
                       ? g_ringHead % RING_LINES
                       : 0;
        for (size_t i = 0; i < g_ringCount; ++i)
        {
            size_t slot = (start + i) % RING_LINES;
            const char* ln = g_ring[slot];
            size_t l = strlen(ln);
            if (l) wr(ln, l);
        }

        const char footer[] = "\n--- End of crash log ---\n";
        wr(footer, sizeof(footer) - 1);

        CloseHandle(hF);
    }

    // ── Write crash_XXXXXX.dmp ────────────────────────────────
    WriteMiniDump(ep, dumpPath);

    // ── Show a message box so the user knows ─────────────────
    char msg[1024]{};
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
        "Knox Fuser crashed.\n\n"
        "Crash log  : %s\n"
        "Minidump   : %s\n\n"
        "Please report these files.",
        logPath.c_str(), dumpPath.c_str());
    MessageBoxA(nullptr, msg, "Knox Fuser - Crash", MB_ICONERROR | MB_OK | MB_TOPMOST);
}

// ─────────────────────────────────────────────────────────────
//  SEH unhandled exception filter
// ─────────────────────────────────────────────────────────────
LONG WINAPI Logger::UnhandledExceptionHandler(EXCEPTION_POINTERS* ep)
{
    // Re-entrancy guard
    static LONG s_entered = 0;
    if (InterlockedExchange(&s_entered, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    // Flush normal log first
    if (g_hFile != INVALID_HANDLE_VALUE)
    {
        const char dying[] = "[Logger] *** UNHANDLED EXCEPTION – writing crash dump ***\n";
        DWORD d = 0;
        WriteFile(g_hFile, dying, sizeof(dying) - 1, &d, nullptr);
        FlushFileBuffers(g_hFile);
        CloseLogFile();
    }

    WriteCrashLog(ep);

    // Let Windows default handler take over (creates WER report)
    return EXCEPTION_CONTINUE_SEARCH;
}
