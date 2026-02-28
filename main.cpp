// ============================================================
//  main.cpp  –  Entry point, config parsing, adapter selector
//  Zero-Latency Network Video Fuser
// ============================================================

#include "NetworkFuser.h"
#include "Logger.h"
#include "ConfigUI.h"
#include "Sender.h"
#include "Receiver.h"

// ─────────────────────────────────────────────────────────────
//  FuserUtil implementations
// ─────────────────────────────────────────────────────────────
namespace FuserUtil
{
    // Minimal INI reader – no external deps
    std::string ReadIniString(const std::string& path,
                              const std::string& section,
                              const std::string& key,
                              const std::string& defaultVal)
    {
        char buf[1024]{};
        DWORD ret = GetPrivateProfileStringA(
            section.c_str(), key.c_str(), defaultVal.c_str(),
            buf, sizeof(buf), path.c_str());
        return std::string(buf, ret);
    }

    int ReadIniInt(const std::string& path,
                   const std::string& section,
                   const std::string& key,
                   int defaultVal)
    {
        return static_cast<int>(GetPrivateProfileIntA(
            section.c_str(), key.c_str(), defaultVal, path.c_str()));
    }

    void Log(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        // Route through Logger so every existing call goes to the log file
        Logger::WriteV(LogLevel::Info, fmt, args);
        va_end(args);
    }

    void ElevateProcessPriority()
    {
        if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS))
            FuserUtil::Log("[Main] SetPriorityClass REALTIME failed: %u\n", GetLastError());
        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
            FuserUtil::Log("[Main] SetThreadPriority TIME_CRITICAL failed: %u\n", GetLastError());
    }

    void PinThreadToCore(DWORD coreIndex)
    {
        DWORD_PTR mask = static_cast<DWORD_PTR>(1) << coreIndex;
        SetThreadAffinityMask(GetCurrentThread(), mask);
    }
}


// ─────────────────────────────────────────────────────────────
//  Config loader
// ─────────────────────────────────────────────────────────────
static FuserConfig LoadConfig(const std::string& iniPath)
{
    FuserConfig cfg;

    std::string mode  = FuserUtil::ReadIniString(iniPath, "Fuser", "Mode", "receiver");
    cfg.isSender      = (mode == "sender");
    cfg.localIP       = FuserUtil::ReadIniString(iniPath, "Fuser", "LocalIP",    "0.0.0.0");
    cfg.remoteIP      = FuserUtil::ReadIniString(iniPath, "Fuser", "RemoteIP",   "192.168.50.1");
    cfg.port          = static_cast<uint16_t>(
                            FuserUtil::ReadIniInt(iniPath, "Fuser", "Port",
                                                  static_cast<int>(FUSER_PORT)));
    cfg.adapterIndex  = FuserUtil::ReadIniInt(iniPath, "Fuser", "AdapterIndex", -1);
    cfg.captureMonitor= FuserUtil::ReadIniInt(iniPath, "Fuser", "CaptureMonitor", 0);

    return cfg;
}

// ─────────────────────────────────────────────────────────────
//  Generate a default config.ini if none exists
// ─────────────────────────────────────────────────────────────
static void WriteDefaultIni(const std::string& iniPath)
{
    FILE* f = nullptr;
    if (fopen_s(&f, iniPath.c_str(), "wx") != 0 || !f)
        return;   // already exists

    fprintf(f,
        "; Knox Fuser – config.ini\n"
        "; Mode: sender  (second PC, captures + transmits overlay)\n"
        ";       receiver (main gaming PC, receives + renders overlay)\n"
        "[Fuser]\n"
        "Mode           = receiver\n"
        "LocalIP        = 0.0.0.0\n"
        "RemoteIP       = 192.168.50.2\n"
        "Port           = %u\n"
        "AdapterIndex   = -1\n"
        "CaptureMonitor = 0\n",
        static_cast<unsigned>(FUSER_PORT));
    fclose(f);

    FuserUtil::Log("[Main] Wrote default config.ini\n");
}

// ─────────────────────────────────────────────────────────────
//  WinMain
// ─────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    // ── Initialise Logger first – catches crashes from this point on ──
    {
        char exePathBuf[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exePathBuf, MAX_PATH);
        std::string exeDir(exePathBuf);
        auto sl = exeDir.find_last_of("\\/");
        if (sl != std::string::npos) exeDir = exeDir.substr(0, sl + 1);
        std::string logDir = exeDir + "Logs";
        Logger::Init(logDir);
        Logger::Info("[Main] Knox Fuser starting. Log dir: %s", logDir.c_str());
    }

    // Initialise Winsock2 early
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        Logger::Fatal("[Main] WSAStartup failed. GLE=%u", GetLastError());
        MessageBoxW(nullptr, L"WSAStartup failed.", L"Knox Fuser", MB_ICONERROR);
        return 1;
    }

    // ── Show the configuration UI ────────────────────────────
    // The UI reads the existing config.ini (if present) to pre-fill
    // fields, lets the user edit everything, then writes back on
    // "Launch".  If the user closes the window, we exit cleanly.
    FuserConfig cfg;
    if (!ShowConfigUI(cfg))
    {
        // User dismissed the window without launching
        WSACleanup();
        return 0;
    }

    // ── Allocate a debug console (after the UI closes) ───────
    AllocConsole();
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);

    Logger::Info("[Main] --- Configuration ---");
    Logger::Info("[Main] Mode     : %s", cfg.isSender ? "SENDER" : "RECEIVER");
    Logger::Info("[Main] LocalIP  : %s", cfg.localIP.c_str());
    Logger::Info("[Main] RemoteIP : %s", cfg.remoteIP.c_str());
    Logger::Info("[Main] Port     : %u", cfg.port);
    Logger::Info("[Main] Monitor  : %d", cfg.captureMonitor);
    Logger::Info("[Main] Log file : %s", Logger::GetLogPath().c_str());

    // ── Branch: Sender ───────────────────────────────────────
    if (cfg.isSender)
    {
        FuserUtil::ElevateProcessPriority();

        SenderModule sender(cfg);
        if (!sender.Init())
        {
            Logger::Fatal("[Main] Sender init failed!");
            WSACleanup();
            return 1;
        }

        // Run on a dedicated high-priority thread
        std::thread senderThread([&sender]
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            FuserUtil::PinThreadToCore(0);
            sender.Run();
        });

        FuserUtil::Log("[Main] Sender running. Close the console to stop.\n");

        // Keep main thread pumping messages until console is closed
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        sender.Stop();
        if (senderThread.joinable())
            senderThread.join();
    }
    // ── Branch: Receiver ─────────────────────────────────────
    else
    {
        ReceiverModule receiver(cfg);
        if (!receiver.Init())
        {
            Logger::Fatal("[Main] Receiver init failed!");
            WSACleanup();
            return 1;
        }

        receiver.Run();   // blocking – exits when overlay window is destroyed
    }

    WSACleanup();
    Logger::Info("[Main] Exiting cleanly.");
    Logger::Shutdown();
    return 0;
}
