// ============================================================
//  Sender.cpp  –  DXGI Desktop Duplication capture + UDP send
//  Zero-Latency Network Video Fuser
// ============================================================

#include "NetworkFuser.h"
#include "Sender.h"

// ─────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────
namespace
{
    bool PerformDiscovery(SOCKET s, sockaddr_in& outDest, uint16_t port) {
        // Listen for "KNOX_DISCOVERY_v1" on Port 9878
        SOCKET discSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (discSock == INVALID_SOCKET) return false;

        BOOL reuse = TRUE;
        setsockopt(discSock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(port + 1); // 9878
        local.sin_addr.s_addr = INADDR_ANY;
        if (bind(discSock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
            closesocket(discSock); return false;
        }

        // Set timeout
        DWORD timeout = 800; // 0.8s
        setsockopt(discSock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

        char buf[256];
        sockaddr_in from{};
        int fromLen = sizeof(from);
        
        // Scan for up to 2.5s to find a Cable IP (prefers 10.* or 169.*)
        bool foundAny = false;
        sockaddr_in backupWiFi{};
        bool foundWiFi = false;

        auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 2500) {
            int nr = recvfrom(discSock, buf, sizeof(buf)-1, 0, (sockaddr*)&from, &fromLen);
            if (nr > 0) {
                buf[nr] = '\0';
                if (strstr(buf, "KNOX_DISCOVERY")) {
                    char hitIP[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &from.sin_addr, hitIP, INET_ADDRSTRLEN);
                    
                    // Priority check
                    bool isCable = (strncmp(hitIP, "10.", 3) == 0 || strncmp(hitIP, "169.254.", 8) == 0);
                    if (isCable) {
                        outDest.sin_addr = from.sin_addr;
                        closesocket(discSock);
                        return true;
                    } else if (strncmp(hitIP, "192.168.", 8) == 0) {
                        backupWiFi = from;
                        foundWiFi = true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        closesocket(discSock);
        if (foundWiFi) {
            FuserUtil::Log("[Sender] Warning: Using WiFi IP (%d.%d.%d.%d) because no cable was found.\n", 
                backupWiFi.sin_addr.S_un.S_un_b.s_b1, backupWiFi.sin_addr.S_un.S_un_b.s_b2,
                backupWiFi.sin_addr.S_un.S_un_b.s_b3, backupWiFi.sin_addr.S_un.S_un_b.s_b4);
            outDest.sin_addr = backupWiFi.sin_addr;
            return true;
        }
        return false;
    }

    // Scan a BGRA buffer and find the tightest bounding box of
    // non-black (alpha > 0 OR any colour channel > threshold) pixels.
    BoundingBox ComputeBoundingBox(const uint8_t* bgra,
                                   uint32_t       width,
                                   uint32_t       height,
                                   uint8_t        threshold = 2)
    {
        uint32_t xMin = width,  yMin = height;
        uint32_t xMax = 0,      yMax = 0;
        bool     found = false;

        const uint32_t stride = width * 4;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = bgra + y * stride;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t b = row[x * 4 + 0];
                const uint8_t g = row[x * 4 + 1];
                const uint8_t r = row[x * 4 + 2];
                const uint8_t a = row[x * 4 + 3];
                if (b > threshold || g > threshold || r > threshold || a > threshold)
                {
                    if (x < xMin) xMin = x;
                    if (x > xMax) xMax = x;
                    if (y < yMin) yMin = y;
                    if (y > yMax) yMax = y;
                    found = true;
                }
            }
        }

        if (!found)
            return BoundingBox{0, 0, 0, 0};

        return BoundingBox{
            xMin,
            yMin,
            xMax - xMin + 1,
            yMax - yMin + 1
        };
    }

    // Copy a sub-rectangle from a full-width BGRA buffer into dst
    void CropBGRA(const uint8_t* src, uint32_t srcWidth,
                  uint8_t*       dst,
                  const BoundingBox& bb)
    {
        const uint32_t srcStride = srcWidth * 4;
        const uint32_t dstStride = bb.w    * 4;

        for (uint32_t row = 0; row < bb.h; ++row)
        {
            const uint8_t* srcRow = src + (bb.y + row) * srcStride + bb.x * 4;
            uint8_t*       dstRow = dst + row * dstStride;
            std::memcpy(dstRow, srcRow, dstStride);
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  SenderModule implementation
// ─────────────────────────────────────────────────────────────
SenderModule::SenderModule(const FuserConfig& cfg)
    : m_cfg(cfg)
    , m_sock(INVALID_SOCKET)
    , m_running(false)
    , m_frameID(0)
    , m_d3dDevice(nullptr)
    , m_d3dContext(nullptr)
    , m_dxgiOutput1(nullptr)
    , m_duplication(nullptr)
    , m_stagingTex(nullptr)
{}

SenderModule::~SenderModule()
{
    Stop();
}

// ─── Public: initialise networking + DXGI ───────────────────
bool SenderModule::Init()
{
    if (!InitSocket())   return false;
    if (!InitDXGI())     return false;
    return true;
}

void SenderModule::Run()
{
    m_running = true;

    // Discovery phase if no IP target
    // Let config handle the IP (Targeting configurable address)
    if (m_cfg.remoteIP.empty() || m_cfg.remoteIP == "0.0.0.0" || m_cfg.remoteIP == "AUTO") {
        m_dest.sin_family = AF_INET;
        m_dest.sin_port = htons(m_cfg.port);
        m_dest.sin_addr.s_addr = INADDR_BROADCAST;
        FuserUtil::Log("[Sender] Discovery Mode: Waiting for Receiver broadcast...\n");
    } else {
        m_dest.sin_family = AF_INET;
        m_dest.sin_port = htons(m_cfg.port);
        inet_pton(AF_INET, m_cfg.remoteIP.c_str(), &m_dest.sin_addr);
        FuserUtil::Log("[Sender] TARGETING CONFIGURED IP -> %s:%u\n", m_cfg.remoteIP.c_str(), m_cfg.port);
    }

    if (!m_running) return;

    FuserUtil::Log("[Sender] Starting capture loop -> %u\n", m_cfg.port);

    // Allocate scratch buffers
    m_fullFrameBuf.resize(SENDER_CAPTURE_RES_W * SENDER_CAPTURE_RES_H * 4);
    m_croppedBuf.resize(  SENDER_CAPTURE_RES_W * SENDER_CAPTURE_RES_H * 4);
    m_packetBuf.resize(MAX_UDP_PAYLOAD);

    uint32_t sentCount = 0;
    uint32_t failCount = 0;

    while (m_running)
    {
        if (!CaptureFrame())
        {
            // Aggressive retry
            continue;
        }
        failCount = 0;

        // Force a heartbeat pixel in top-left
        if (m_fullFrameBuf.size() >= 4) {
            m_fullFrameBuf[0] = 2; m_fullFrameBuf[1] = 2; m_fullFrameBuf[2] = 2; m_fullFrameBuf[3] = 255;
        }

        SendFrame();

        sentCount++;
        if (sentCount % 100 == 0) {
            FuserUtil::Log("[Sender] Still sending... (Last Frame sent to %s)\n", 
                m_cfg.remoteIP.empty() ? "BROADCAST" : m_cfg.remoteIP.c_str());
            
            // Heartbeat: Prove the line is open
            const char* beep = "BEEP";
            int rc = sendto(m_sock, beep, 4, 0, (sockaddr*)&m_dest, sizeof(m_dest));
            if (rc == SOCKET_ERROR) {
                FuserUtil::Log("[Sender] ERROR: Network blocked outgoing heartbeat! Code: %d\n", WSAGetLastError());
            }
        }
    }
}

void SenderModule::Stop()
{
    m_running = false;

    if (m_duplication)  { m_duplication->ReleaseFrame(); m_duplication->Release();  m_duplication  = nullptr; }
    if (m_stagingTex)   { m_stagingTex->Release();   m_stagingTex   = nullptr; }
    if (m_dxgiOutput1)  { m_dxgiOutput1->Release();  m_dxgiOutput1  = nullptr; }
    if (m_d3dContext)   { m_d3dContext->Release();   m_d3dContext   = nullptr; }
    if (m_d3dDevice)    { m_d3dDevice->Release();    m_d3dDevice    = nullptr; }

    if (m_sock != INVALID_SOCKET)
    {
        // Cancel any pending discovery recv
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
}

// ─── Private: socket init ────────────────────────────────────
bool SenderModule::InitSocket()
{
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) return false;

    // Bind to any port locally
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    
    // Bind to ANY interface for generic routing
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        FuserUtil::Log("[Sender] ERROR: Failed to bind socket. Code: %d\n", WSAGetLastError());
    } else {
        FuserUtil::Log("[Sender] Bound outgoing traffic to ANY interface.\n");
    }

    // Enable broadcasting
    BOOL broadcast = TRUE;
    setsockopt(m_sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

    // Prepare destination template
    m_dest.sin_family = AF_INET;
    m_dest.sin_port   = htons(m_cfg.port);

    FuserUtil::Log("[Sender] Socket armed (Global Broadcast Mode enabled)\n");

    return true;
}



// ─── Private: DXGI Desktop Duplication ──────────────────────
bool SenderModule::InitDXGI()
{
    // Create D3D11 device on the default adapter
    D3D_FEATURE_LEVEL featureLevel;
    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        nullptr, 0,
        D3D11_SDK_VERSION,
        &m_d3dDevice,
        &featureLevel,
        &m_d3dContext
    );
    if (FAILED(hr))
    {
        FuserUtil::Log("[Sender] D3D11CreateDevice failed: 0x%08X\n", hr);
        return false;
    }

    // Walk the DXGI adapter/output chain to find the requested monitor
    IDXGIDevice*  dxgiDevice  = nullptr;
    IDXGIAdapter* dxgiAdapter = nullptr;
    m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice),
                                reinterpret_cast<void**>(&dxgiDevice));
    dxgiDevice->GetParent(__uuidof(IDXGIAdapter),
                          reinterpret_cast<void**>(&dxgiAdapter));
    dxgiDevice->Release();

    IDXGIOutput* output = nullptr;
    int target = m_cfg.captureMonitor;
    for (int i = 0; ; ++i)
    {
        IDXGIOutput* candidate = nullptr;
        if (dxgiAdapter->EnumOutputs(i, &candidate) == DXGI_ERROR_NOT_FOUND)
            break;
        if (i == target)
        {
            output = candidate;
            break;
        }
        candidate->Release();
    }
    dxgiAdapter->Release();

    if (!output)
    {
        FuserUtil::Log("[Sender] Monitor index %d not found.\n", target);
        return false;
    }

    hr = output->QueryInterface(__uuidof(IDXGIOutput1),
                                reinterpret_cast<void**>(&m_dxgiOutput1));
    output->Release();
    if (FAILED(hr))
    {
        FuserUtil::Log("[Sender] QueryInterface IDXGIOutput1 failed: 0x%08X\n", hr);
        return false;
    }

    hr = m_dxgiOutput1->DuplicateOutput(m_d3dDevice, &m_duplication);
    if (FAILED(hr))
    {
        FuserUtil::Log("[Sender] DuplicateOutput failed: 0x%08X\n", hr);
        return false;
    }

    DXGI_OUTDUPL_DESC duplDesc{};
    m_duplication->GetDesc(&duplDesc);
    m_captureW = duplDesc.ModeDesc.Width;
    m_captureH = duplDesc.ModeDesc.Height;

    FuserUtil::Log("[Sender] Capture surface: %u x %u\n", m_captureW, m_captureH);

    // Pre-create a CPU-accessible staging texture sized to the desktop
    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width              = m_captureW;
    stagingDesc.Height             = m_captureH;
    stagingDesc.MipLevels          = 1;
    stagingDesc.ArraySize          = 1;
    stagingDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;  // BGRA
    stagingDesc.SampleDesc.Count   = 1;
    stagingDesc.Usage              = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags          = 0;

    hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTex);
    if (FAILED(hr))
    {
        FuserUtil::Log("[Sender] CreateTexture2D (staging) failed: 0x%08X\n", hr);
        return false;
    }

    m_fullFrameBuf.resize(m_captureW * m_captureH * 4);
    m_croppedBuf  .resize(m_captureW * m_captureH * 4);

    return true;
}

// ─── Private: one capture + send cycle ──────────────────────
bool SenderModule::CaptureFrame()
{
    IDXGIResource*           deskRes    = nullptr;
    DXGI_OUTDUPL_FRAME_INFO  frameInfo  = {};

    // AcquireNextFrame with a 0ms timeout for non-blocking poll;
    // we rely on the DXGI present signal so 5ms is safe.
    HRESULT hr = m_duplication->AcquireNextFrame(5, &frameInfo, &deskRes);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return false;  // no new frame yet
    if (FAILED(hr))
    {
        FuserUtil::Log("[Sender] AcquireNextFrame failed: 0x%08X – reinitialising\n", hr);
        // Attempt duplication recovery (monitor mode change, etc.)
        m_duplication->Release();
        m_duplication = nullptr;
        HRESULT hr2 = m_dxgiOutput1->DuplicateOutput(m_d3dDevice, &m_duplication);
        if (FAILED(hr2))
            FuserUtil::Log("[Sender] DuplicateOutput recovery failed: 0x%08X\n", hr2);
        return false;
    }

    // Get the desktop texture
    ID3D11Texture2D* desktopTex = nullptr;
    hr = deskRes->QueryInterface(__uuidof(ID3D11Texture2D),
                                 reinterpret_cast<void**>(&desktopTex));
    deskRes->Release();
    if (FAILED(hr))
    {
        m_duplication->ReleaseFrame();
        return false;
    }

    // Copy GPU → CPU-accessible staging texture
    m_d3dContext->CopyResource(m_stagingTex, desktopTex);
    desktopTex->Release();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = m_d3dContext->Map(m_stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        m_duplication->ReleaseFrame();
        return false;
    }

    // Copy with stride correction (GPU pitch may be wider than width*4)
    const uint32_t destStride = m_captureW * 4;
    for (uint32_t row = 0; row < m_captureH; ++row)
    {
        std::memcpy(
            m_fullFrameBuf.data() + row * destStride,
            reinterpret_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch,
            destStride);
    }

    m_d3dContext->Unmap(m_stagingTex, 0);
    m_duplication->ReleaseFrame();

    // Compute the tight bounding box of visible (non-black) pixels
    m_lastBB = ComputeBoundingBox(m_fullFrameBuf.data(), m_captureW, m_captureH);
    if (m_lastBB.w == 0 || m_lastBB.h == 0)
        return false;   // fully black frame – nothing to send

    // Crop
    CropBGRA(m_fullFrameBuf.data(), m_captureW, m_croppedBuf.data(), m_lastBB);

    return true;
}

void SenderModule::SendFrame()
{
    const uint32_t frameBytes = m_lastBB.w * m_lastBB.h * 4;

    // ── Packet 0: metadata + first pixel slice ───────────────
    // Layout of packet 0 payload (after header):
    //   FrameMetaPayload (20 bytes) | pixel_data[0..N]
    const uint32_t pixelBytesInPkt0 = MAX_PIXEL_PAYLOAD - FRAME_META_SIZE;

    // Pre-calculate total packets needed
    const uint32_t remainingAfterPkt0 =
        (frameBytes > pixelBytesInPkt0) ? (frameBytes - pixelBytesInPkt0) : 0;
    const uint32_t extraPackets =
        (remainingAfterPkt0 + MAX_PIXEL_PAYLOAD - 1) / MAX_PIXEL_PAYLOAD;
    const uint16_t totalPackets = static_cast<uint16_t>(1 + extraPackets);

    const uint32_t thisFrameID = ++m_frameID;
    const uint8_t* pixelPtr    = m_croppedBuf.data();

    // Build packet 0
    {
        FuserPacketHeader hdr;
        hdr.FrameID      = thisFrameID;
        hdr.PacketIndex  = 0;
        hdr.TotalPackets = totalPackets;

        FrameMetaPayload meta;
        meta.width    = m_lastBB.w;
        meta.height   = m_lastBB.h;
        meta.originX  = m_lastBB.x;
        meta.originY  = m_lastBB.y;
        meta.rawBytes = frameBytes;

        uint8_t* p = m_packetBuf.data();
        std::memcpy(p, &hdr,  HEADER_SIZE);       p += HEADER_SIZE;
        std::memcpy(p, &meta, FRAME_META_SIZE);   p += FRAME_META_SIZE;

        const uint32_t pixToCopy = (frameBytes < pixelBytesInPkt0)
                                   ? frameBytes : pixelBytesInPkt0;
        std::memcpy(p, pixelPtr, pixToCopy);
        pixelPtr += pixToCopy;

        int sendLen = static_cast<int>(HEADER_SIZE + FRAME_META_SIZE + pixToCopy);
        int rc = sendto(m_sock, reinterpret_cast<const char*>(m_packetBuf.data()),
               sendLen, 0,
               reinterpret_cast<const sockaddr*>(&m_dest), sizeof(m_dest));
               
        if (rc == SOCKET_ERROR) {
            static int errCount = 0;
            if (errCount++ % 500 == 0) FuserUtil::Log("[Sender] ERROR: sendto explicitly blocked by Windows. Code: %d\n", WSAGetLastError());
        }
    }

    // ── Packets 1..N: remaining pixel slices ─────────────────
    uint32_t remaining = remainingAfterPkt0;
    uint16_t pktIdx    = 1;

    while (remaining > 0)
    {
        FuserPacketHeader hdr;
        hdr.FrameID      = thisFrameID;
        hdr.PacketIndex  = pktIdx;
        hdr.TotalPackets = totalPackets;

        uint32_t slice = (remaining > MAX_PIXEL_PAYLOAD) ? MAX_PIXEL_PAYLOAD : remaining;

        uint8_t* p = m_packetBuf.data();
        std::memcpy(p, &hdr, HEADER_SIZE); p += HEADER_SIZE;
        std::memcpy(p, pixelPtr, slice);
        pixelPtr  += slice;
        remaining -= slice;

        int sendLen = static_cast<int>(HEADER_SIZE + slice);
        sendto(m_sock, reinterpret_cast<const char*>(m_packetBuf.data()),
               sendLen, 0,
               reinterpret_cast<const sockaddr*>(&m_dest), sizeof(m_dest));
        ++pktIdx;
    }
}
