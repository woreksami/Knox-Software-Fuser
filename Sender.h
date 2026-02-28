#pragma once
// ============================================================
//  Sender.h  –  SenderModule class declaration
// ============================================================
#include "NetworkFuser.h"

class SenderModule
{
public:
    explicit SenderModule(const FuserConfig& cfg);
    ~SenderModule();

    bool Init();
    void Run();    // blocking – call from dedicated thread
    void Stop();

private:
    bool InitSocket();
    bool InitDXGI();
    bool CaptureFrame();     // returns false if no new frame
    void SendFrame();

    FuserConfig             m_cfg;
    SOCKET                  m_sock;
    sockaddr_in             m_dest{};
    std::atomic<bool>       m_running;
    uint32_t                m_frameID;

    // DXGI / D3D11
    ID3D11Device*           m_d3dDevice;
    ID3D11DeviceContext*    m_d3dContext;
    IDXGIOutput1*           m_dxgiOutput1;
    IDXGIOutputDuplication* m_duplication;
    ID3D11Texture2D*        m_stagingTex;
    uint32_t                m_captureW = 0;
    uint32_t                m_captureH = 0;

    // Frame buffers
    std::vector<uint8_t>    m_fullFrameBuf;   // full desktop BGRA
    std::vector<uint8_t>    m_croppedBuf;     // cropped region
    std::vector<uint8_t>    m_packetBuf;      // single packet scratch
    BoundingBox             m_lastBB{};
};
