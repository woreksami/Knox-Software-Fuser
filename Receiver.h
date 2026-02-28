#pragma once
// ============================================================
//  Receiver.h  -  High-Compatibility DX11 Hijack
// ============================================================
#include "NetworkFuser.h"

struct IocpRecvContext;

class ReceiverModule
{
public:
    explicit ReceiverModule(const FuserConfig& cfg);
    ~ReceiverModule();

    bool Init();
    void Run();
    void Stop();

private:
    bool CreateOverlayWindow();
    bool InitDX11();
    bool InitSocket();
    void ReleaseDX11();

    void RecvThreadProc();
    void RenderThreadProc();
    void RenderFrame(const uint8_t* bgra, uint32_t fw, uint32_t fh);
    void PumpMessages();

    // Config
    FuserConfig             m_cfg;

    // Network
    SOCKET                  m_sock    = INVALID_SOCKET;
    std::atomic<bool>       m_running{false};
    std::thread             m_recvThread;

    // Frame hand-off
    std::mutex              m_frameMtx;
    std::condition_variable m_frameCv;
    std::vector<uint8_t>    m_pendingFrame;
    uint32_t                m_pendingW   = 0;
    uint32_t                m_pendingH   = 0;
    bool                    m_frameReady = false;

    // Window
    HWND  m_hwndOverlay = nullptr;
    HWND  m_hwndParent  = nullptr;
    UINT  m_overlayW    = 0;
    UINT  m_overlayH    = 0;

    // DirectX 11
    ID3D11Device*             m_d3dDevice        = nullptr;
    ID3D11DeviceContext*      m_d3dContext       = nullptr;
    IDXGISwapChain*           m_swapChain        = nullptr;
    ID3D11RenderTargetView*   m_renderTargetView = nullptr;
    ID3D11Texture2D*          m_uploadTex        = nullptr;
    ID3D11ShaderResourceView* m_shaderResView    = nullptr;
    ID3D11VertexShader*       m_vertexShader     = nullptr;
    ID3D11PixelShader*        m_pixelShader      = nullptr;
    ID3D11SamplerState*       m_sampler          = nullptr;
    ID3D11BlendState*         m_blendState       = nullptr;
    D3D11_VIEWPORT            m_viewport{};

    uint32_t m_frameWidth  = 0;
    uint32_t m_frameHeight = 0;
};
