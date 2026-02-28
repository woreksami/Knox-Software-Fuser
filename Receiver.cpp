// ============================================================
//  Receiver.cpp  -  D3D11 Robust Hijack Implementation
//  Zero-Latency Network Video Fuser
// ============================================================

#include "NetworkFuser.h"
#include "Receiver.h"
#include "MemoryReassembly.h"
#include <d3d11.h>
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ─── IOCP per-buffer context ──────────────────────────────────
struct IocpRecvContext {
    OVERLAPPED   overlapped{};
    WSABUF       wsaBuf{};
    SOCKADDR_IN  fromAddr{};
    INT          fromLen = sizeof(SOCKADDR_IN);
    uint8_t      data[MAX_UDP_PAYLOAD + 64]{};
};

// ─── Hijack Logic ─────────────────────────────────────────────
namespace HijackHelper
{
    struct EnumData { const wchar_t* cls; const wchar_t* proc; HWND result; };
    static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lp) {
        auto* d = reinterpret_cast<EnumData*>(lp);
        wchar_t cls[256]{}; GetClassNameW(hwnd, cls, 256);
        if (d->cls && wcscmp(cls, d->cls) != 0) return TRUE;
        if (d->proc) {
            DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
            HANDLE hP = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!hP) return TRUE;
            wchar_t path[MAX_PATH]{}; DWORD sz = MAX_PATH;
            QueryFullProcessImageNameW(hP, 0, path, &sz); CloseHandle(hP);
            const wchar_t* nm = wcsrchr(path, L'\\');
            nm = nm ? nm+1 : path;
            if (_wcsicmp(nm, d->proc) != 0) return TRUE;
        }
        if (IsWindowVisible(hwnd)) { d->result = hwnd; return FALSE; }
        return TRUE;
    }

    HWND FindBestOverlay() {
        struct { const wchar_t* c; const wchar_t* p; } cands[] = {
            { L"CEF.NVSPCAPS", L"NVIDIA Share.exe" }, // NVIDIA Overlay
            { L"Chrome_WidgetWin_1", L"Medal.exe" },   // Medal.tv
            { L"Chrome_WidgetWin_1", L"Discord.exe" }  // Discord
        };
        for (auto& c : cands) {
            EnumData d{ c.c, c.p, nullptr };
            EnumWindows(EnumWindowsProc, (LPARAM)&d);
            if (d.result) return d.result;
        }
        return nullptr;
    }
}

// ─── HLSL Shaders ─────────────────────────────────────────────
static const char g_vsSource[] =
    "void vs_main(uint id : SV_VertexID, out float4 pos : SV_POSITION, out float2 uv : TEXCOORD0) {\n"
    "    uv = float2((id & 1) ? 1.0f : 0.0f, (id & 2) ? 1.0f : 0.0f);\n"
    "    pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);\n"
    "}\n";

static const char g_psSource[] =
    "Texture2D gTex : register(t0); SamplerState gSmp : register(s0);\n"
    "float4 ps_main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    float4 c = gTex.Sample(gSmp, uv);\n"
    "    if(c.a < 0.01f) discard;\n" // Performance: discard transparent pixels
    "    return c;\n"
    "}\n";

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

// ─── ReceiverModule Implementation ────────────────────────────
ReceiverModule::ReceiverModule(const FuserConfig& cfg) : m_cfg(cfg) {}
ReceiverModule::~ReceiverModule() { Stop(); }

bool ReceiverModule::Init() {
    FuserUtil::ElevateProcessPriority();

    FuserUtil::Log("[Main] Auto-Discovery Enabled (Receiver Mode)\n");

    if (!CreateOverlayWindow()) return false;
    if (!InitDX11()) return false;
    if (!InitSocket()) return false;

    return true;
}


bool ReceiverModule::CreateOverlayWindow() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KnoxFuserOverlay";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    m_hwndParent = HijackHelper::FindBestOverlay();
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    
    // We create as a standard popup FIRST (no layered bits) to ensure DX11 init succeeds
    m_hwndOverlay = CreateWindowExW(
        WS_EX_TOPMOST, L"KnoxFuserOverlay", L"", WS_POPUP,
        0, 0, sw, sh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!m_hwndOverlay) return false;
    m_overlayW = sw; m_overlayH = sh;
    return true;
}

bool ReceiverModule::InitDX11() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwndOverlay;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &m_swapChain, &m_d3dDevice, &fl, &m_d3dContext);
    
    if (FAILED(hr)) {
        FuserUtil::Log("[Receiver] DX11 Init failed: 0x%08X\n", hr);
        return false;
    }

    ID3D11Texture2D* block = nullptr;
    m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&block);
    m_d3dDevice->CreateRenderTargetView(block, nullptr, &m_renderTargetView);
    block->Release();

    // Compile Shaders
    HMODULE hComp = LoadLibraryA("d3dcompiler_47.dll");
    if (!hComp) return false;
    auto pfnCompile = (D3DCompileFn)GetProcAddress(hComp, "D3DCompile");
    
    ID3DBlob *vsB = nullptr, *psB = nullptr;
    pfnCompile(g_vsSource, strlen(g_vsSource), nullptr, nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vsB, nullptr);
    pfnCompile(g_psSource, strlen(g_psSource), nullptr, nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &psB, nullptr);
    
    m_d3dDevice->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_vertexShader);
    m_d3dDevice->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_pixelShader);
    vsB->Release(); psB->Release();
    FreeLibrary(hComp);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    m_d3dDevice->CreateSamplerState(&smp, &m_sampler);

    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_d3dDevice->CreateBlendState(&bld, &m_blendState);

    m_viewport = { 0.0f, 0.0f, (float)m_overlayW, (float)m_overlayH, 0.0f, 1.0f };

    // AFTER DX11 is initialized on the window, WE HIJACK!
    if (m_hwndParent) {
        FuserUtil::Log("[Receiver] Performing stealth hijack on parent 0x%p\n", m_hwndParent);
        SetParent(m_hwndOverlay, m_hwndParent);
    }
    
    // Set styles for transparency and click-through
    SetWindowLong(m_hwndOverlay, GWL_EXSTYLE, WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE);
    SetLayeredWindowAttributes(m_hwndOverlay, RGB(0,0,0), 0, LWA_COLORKEY);
    
    ShowWindow(m_hwndOverlay, SW_SHOW);
    FuserUtil::Log("[Receiver] Overlay ready and hijacked.\n");
    return true;
}

void ReceiverModule::RenderFrame(const uint8_t* bgra, uint32_t fw, uint32_t fh) {
    if (!bgra || fw == 0 || fh == 0) return;

    if (fw != m_frameWidth || fh != m_frameHeight) {
        if (m_shaderResView) m_shaderResView->Release();
        if (m_uploadTex) m_uploadTex->Release();
        
        D3D11_TEXTURE2D_DESC td{};
        td.Width = fw; td.Height = fh; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        
        m_d3dDevice->CreateTexture2D(&td, nullptr, &m_uploadTex);
        m_d3dDevice->CreateShaderResourceView(m_uploadTex, nullptr, &m_shaderResView);
        m_frameWidth = fw; m_frameHeight = fh;
    }

    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m_d3dContext->Map(m_uploadTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        for (uint32_t y = 0; y < fh; y++)
            memcpy((uint8_t*)map.pData + y * map.RowPitch, bgra + y * fw * 4, fw * 4);
        m_d3dContext->Unmap(m_uploadTex, 0);
    }

    float black[4] = {0,0,0,0};
    m_d3dContext->ClearRenderTargetView(m_renderTargetView, black);
    m_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_d3dContext->VSSetShader(m_vertexShader, nullptr, 0);
    m_d3dContext->PSSetShader(m_pixelShader, nullptr, 0);
    m_d3dContext->PSSetShaderResources(0, 1, &m_shaderResView);
    m_d3dContext->PSSetSamplers(0, 1, &m_sampler);
    m_d3dContext->OMSetRenderTargets(1, &m_renderTargetView, nullptr);
    m_d3dContext->OMSetBlendState(m_blendState, nullptr, 0xFFFFFFFF);
    m_d3dContext->RSSetViewports(1, &m_viewport);
    m_d3dContext->Draw(4, 0);
    m_swapChain->Present(0, 0);
}

void ReceiverModule::Stop() {
    m_running = false;
    if (m_recvThread.joinable()) m_recvThread.join();
    ReleaseDX11();
}

void ReceiverModule::ReleaseDX11() {
#define REL(p) if(p){p->Release();p=nullptr;}
    REL(m_renderTargetView); REL(m_swapChain); REL(m_d3dContext); REL(m_d3dDevice);
    REL(m_uploadTex); REL(m_shaderResView); REL(m_vertexShader); REL(m_pixelShader);
    REL(m_sampler); REL(m_blendState);
}

bool ReceiverModule::InitSocket() {
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    
    BOOL reuse = TRUE;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in local{}; 
    local.sin_family = AF_INET; 
    local.sin_addr.s_addr = INADDR_ANY; // Catch-all: Listen on all interfaces
    local.sin_port = htons(m_cfg.port); 
    
    if (bind(m_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        FuserUtil::Log("[Socket] FATAL: Bind failed (Error: %d)\n", WSAGetLastError());
        return false;
    }

    int rb = 16*1024*1024; setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, (char*)&rb, sizeof(rb));
    u_long mode = 1; ioctlsocket(m_sock, FIONBIO, &mode);

    FuserUtil::Log("[Receiver] Catch-All listening on Port %u (ANY INTERFACE)\n", m_cfg.port);
    
    // SELF-TEST: Send a tiny packet to ourselves to prove the socket works
    sockaddr_in self{}; self.sin_family = AF_INET; self.sin_port = htons(m_cfg.port);
    inet_pton(AF_INET, "127.0.0.1", &self.sin_addr);
    const char* testMsg = "SELF_TEST";
    sendto(m_sock, testMsg, 9, 0, (sockaddr*)&self, sizeof(self));
    
    return true;
}

void ReceiverModule::RecvThreadProc() {
    FuserUtil::Log("[Socket] Simple High-Speed Listener started.\n");

    MemoryReassembly reasm;
    uint32_t frameCount = 0;
    uint32_t pktCount = 0;
    
    std::vector<uint8_t> buffer(2000);
    FuserUtil::Log("[Socket] Low-Level listener is ARMED. Watching for raw UDP...\n");

    while (m_running) {
        sockaddr_in from{};
        int fromLen = sizeof(from); // CRITICAL: Reset every loop
        int nr = recvfrom(m_sock, (char*)buffer.data(), (int)buffer.size(), 0, (sockaddr*)&from, &fromLen);
        
        if (nr > 0) {
            // Check for self-test
            if (nr == 9 && memcmp(buffer.data(), "SELF_TEST", 9) == 0) {
                FuserUtil::Log("[Socket] SUCCESS: Receiver socket self-tested OK!\n");
                continue;
            }

            // LOG ABSOLUTELY EVERYTHING FOR DIAGNOSTICS
            char senderIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, senderIP, INET_ADDRSTRLEN);
            pktCount++;
            
            if (pktCount % 10 == 0) {
                FuserUtil::Log("[Socket] Raw Packet #%u: %d bytes from %s\n", pktCount, nr, senderIP);
            }

            if (FrameSlot* s = reasm.ConsumePacket(buffer.data(), nr)) {
                {
                    std::lock_guard<std::mutex> l(m_frameMtx);
                    m_pendingFrame = std::move(s->pixelData);
                    m_pendingW = s->width; m_pendingH = s->height; m_frameReady = true;
                }
                m_frameCv.notify_one(); 
                reasm.ReleaseSlot(s);

                frameCount++;
                if (frameCount % 100 == 0) {
                    FuserUtil::Log("[Receiver] RENDERED %u full frames.\n", frameCount);
                }
            }
        } else {
            // No data, sleep tiny bit to save CPU
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        reasm.PurgeExpired();
    }
}




void ReceiverModule::RenderThreadProc() {
    while (m_running) {
        std::vector<uint8_t> p; uint32_t w, h;
        {
            std::unique_lock<std::mutex> l(m_frameMtx);
            m_frameCv.wait_for(l, std::chrono::milliseconds(8), [this]{ return m_frameReady || !m_running; });
            if (!m_running || !m_frameReady) { PumpMessages(); continue; }
            p = std::move(m_pendingFrame); w = m_pendingW; h = m_pendingH; m_frameReady = false;
        }
        RenderFrame(p.data(), w, h);
        PumpMessages();
    }
}

void ReceiverModule::PumpMessages() {
    MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

void ReceiverModule::Run() {
    m_running = true;

    // Launch discovery thread (broadcasts every 1.5s)
    std::thread discoveryThread([this] {
        SOCKET ds = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (ds == INVALID_SOCKET) return;

        BOOL broadcast = TRUE;
        setsockopt(ds, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(9877 + 1); // 9878
        target.sin_addr.s_addr = INADDR_BROADCAST; // 255.255.255.255
        const char* msg = "KNOX_DISCOVERY_v1";

        FuserUtil::Log("[Main] Discovery signal ACTIVE (sending heartbeat every 1.5s)\n");

        while (m_running) {
            sendto(ds, msg, (int)strlen(msg), 0, (sockaddr*)&target, sizeof(target));
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
        closesocket(ds);
    });
    discoveryThread.detach();

    m_recvThread = std::thread([this]{ 
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); 
        RecvThreadProc(); 
    });

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    RenderThreadProc();
}
