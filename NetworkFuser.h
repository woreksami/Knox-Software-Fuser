#pragma once

// ============================================================
//  NetworkFuser.h  –  Shared types, constants & declarations
//  Zero-Latency Network Video Fuser
//  Compiler: MSVC C++17  /MT (static CRT)
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <cstring>
#include <cassert>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <memory>
#include <chrono>

// ─── Link libraries (static) ────────────────────────────────
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ─── Build-time constants ────────────────────────────────────
static constexpr uint16_t FUSER_PORT          = 9877;          // UDP port
static constexpr uint32_t MAX_UDP_PAYLOAD     = 1400;          // bytes per packet (avoids IP fragmentation)
static constexpr uint32_t HEADER_SIZE         = 8;             // bytes: FrameID(4) + PktIdx(2) + TotalPkts(2)
static constexpr uint32_t MAX_PIXEL_PAYLOAD   = MAX_UDP_PAYLOAD - HEADER_SIZE;  // 1392 bytes
static constexpr uint32_t IOCP_RECV_BUFFERS   = 256;           // pending WSARecvFrom calls
static constexpr uint32_t REASSEMBLY_SLOTS    = 8;             // ring-buffer depth for frame reassembly
static constexpr uint32_t FRAME_TIMEOUT_MS    = 5;             // drop incomplete frame after N ms
static constexpr uint32_t MAX_FRAME_BYTES     = 7680 * 4320 * 4; // worst-case 8K BGRA
static constexpr uint32_t SENDER_CAPTURE_RES_W = 3840;
static constexpr uint32_t SENDER_CAPTURE_RES_H = 2160;

// ─── 8-byte packet header (packed, no padding) ──────────────
#pragma pack(push, 1)
struct FuserPacketHeader
{
    uint32_t FrameID;       // monotonically increasing frame counter
    uint16_t PacketIndex;   // 0-based index of this slice within the frame
    uint16_t TotalPackets;  // total slices that make up the complete frame
};
static_assert(sizeof(FuserPacketHeader) == HEADER_SIZE, "Header size mismatch");
#pragma pack(pop)

// ─── Bounding box for cropped transmission ──────────────────
struct BoundingBox
{
    uint32_t x, y, w, h;   // pixel-space; w==0 means no non-black pixels found
};

// ─── Config loaded from config.ini ──────────────────────────
struct FuserConfig
{
    bool     isSender       = false;
    std::string localIP     = "0.0.0.0";   // sender: bind IP; receiver: ignored
    std::string remoteIP    = "0.0.0.0";   // sender: destination IP
    uint16_t port           = FUSER_PORT;
    int      adapterIndex   = -1;          // -1 = auto-select
    int      captureMonitor = 0;           // DXGI output index
};

// ─── Reassembly slot (per-frame) ────────────────────────────
struct FrameSlot
{
    uint32_t              frameID       = 0;
    uint32_t              totalPackets  = 0;
    uint32_t              receivedCount = 0;
    uint32_t              totalBytes    = 0;
    bool                  complete      = false;
    ULONGLONG             firstPacketMs = 0;
    std::vector<uint8_t>  pixelData;                  // assembled BGRA buffer
    std::vector<bool>     received;                   // per-packet receipt flags
    uint32_t              width         = 0;
    uint32_t              height        = 0;
};

// ─── Frame metadata prepended before pixel slices ───────────
// Sent as packet 0 payload extension after the FuserPacketHeader
#pragma pack(push, 1)
struct FrameMetaPayload
{
    uint32_t width;
    uint32_t height;
    uint32_t originX;
    uint32_t originY;
    uint32_t rawBytes;   // total BGRA bytes in this frame
};
#pragma pack(pop)
static constexpr uint32_t FRAME_META_SIZE = sizeof(FrameMetaPayload); // 20 bytes

// ─── Logger (included here so every module can call FuserUtil::Log) ─
#include "Logger.h"

// ─── Forward declarations ────────────────────────────────────
class SenderModule;
class ReceiverModule;
class MemoryReassembly;

// ─── Utility helpers ─────────────────────────────────────────
namespace FuserUtil
{
    // Read a key from a simple INI file (no external deps)
    std::string ReadIniString(const std::string& path,
                              const std::string& section,
                              const std::string& key,
                              const std::string& defaultVal = "");

    int ReadIniInt(const std::string& path,
                   const std::string& section,
                   const std::string& key,
                   int defaultVal = 0);

    // High-precision timestamp in milliseconds
    inline ULONGLONG NowMs()
    {
        return static_cast<ULONGLONG>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Log to debugger output + stdout
    void Log(const char* fmt, ...);

    // Raise the calling process and the current thread to real-time priority
    void ElevateProcessPriority();

    // Pin calling thread to a single logical core (optional – reduces cache misses)
    void PinThreadToCore(DWORD coreIndex);
}
