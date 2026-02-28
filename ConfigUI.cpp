// ============================================================
//  ConfigUI.cpp  -  Minimal dark-theme Win32 config launcher
//  Mode selector + IP + Port.  Zero external dependencies.
// ============================================================

#include "ConfigUI.h"
#include <uxtheme.h>
#include <windowsx.h>
#include <cstdio>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdi32.lib")

// ─── Palette ─────────────────────────────────────────────────
namespace Pal {
    static const COLORREF BG           = RGB(13,  17,  23);
    static const COLORREF PANEL        = RGB(22,  27,  34);
    static const COLORREF BORDER       = RGB(48,  54,  61);
    static const COLORREF ACCENT       = RGB(0,  122, 255);
    static const COLORREF ACCENT_HOVER = RGB(30, 142, 255);
    static const COLORREF ACCENT_PRESS = RGB(0,   90, 200);
    static const COLORREF TXT_PRI      = RGB(230, 237, 243);
    static const COLORREF TXT_SEC      = RGB(139, 148, 158);
    static const COLORREF TXT_LABEL    = RGB(88,  166, 255);
    static const COLORREF SEP          = RGB(33,  38,  45);
}

// ─── Control IDs ─────────────────────────────────────────────
enum : UINT {
    ID_CARD_MODE   = 100,
    ID_EDIT_IP     = 101,
    ID_EDIT_PORT   = 102,
    ID_BTN_LAUNCH  = 103,
    ID_STATIC_IPLBL= 104,
};

// ─── Window size ─────────────────────────────────────────────
static const int WIN_W = 460;
static const int WIN_H = 380;

// ─── Global state ────────────────────────────────────────────
struct UI {
    bool         isSender   = false;
    bool         launched   = false;
    FuserConfig* outCfg     = nullptr;
    HWND         hwnd       = nullptr;

    // Controls
    HWND hCardMode   = nullptr;
    HWND hEditIP     = nullptr;
    HWND hEditPort   = nullptr;
    HWND hBtnLaunch  = nullptr;
    HWND hLblIP      = nullptr;

    // GDI
    HFONT   hFontTitle  = nullptr;
    HFONT   hFontSub    = nullptr;
    HFONT   hFontBody   = nullptr;
    HFONT   hFontLabel  = nullptr;
    HFONT   hFontBtn    = nullptr;
    HBRUSH  hBrushBg    = nullptr;
    HBRUSH  hBrushPanel = nullptr;
    HBRUSH  hBrushAccent= nullptr;
    HBRUSH  hBrushInput = nullptr;
} g;

// ─── GDI helpers ─────────────────────────────────────────────
static HFONT MakeFont(int pt, bool bold, const wchar_t* face = L"Segoe UI")
{
    return CreateFontW(
        -MulDiv(pt, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 72),
        0,0,0, bold ? FW_SEMIBOLD : FW_NORMAL,
        FALSE,FALSE,FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, face);
}

static void RndRect(HDC hdc, RECT r, int rad, COLORREF fill, COLORREF bord)
{
    HPEN p = CreatePen(PS_SOLID,1,bord);
    HBRUSH b = CreateSolidBrush(fill);
    auto op = (HPEN)SelectObject(hdc,p);
    auto ob = (HBRUSH)SelectObject(hdc,b);
    SetBkMode(hdc,TRANSPARENT);
    RoundRect(hdc, r.left,r.top,r.right,r.bottom, rad,rad);
    SelectObject(hdc,op); SelectObject(hdc,ob);
    DeleteObject(p); DeleteObject(b);
}

static void Txt(HDC hdc, const wchar_t* s, RECT r, HFONT f, COLORREF c,
                UINT fmt = DT_LEFT|DT_VCENTER|DT_SINGLELINE)
{
    auto of = (HFONT)SelectObject(hdc,f);
    SetTextColor(hdc,c); SetBkMode(hdc,TRANSPARENT);
    DrawTextW(hdc,s,-1,&r,fmt);
    SelectObject(hdc,of);
}

// ─── Mode card ───────────────────────────────────────────────
static void DrawModePill(HDC hdc, RECT r, const wchar_t* label,
                         const wchar_t* sub, bool selected)
{
    COLORREF fill   = selected ? Pal::ACCENT : Pal::PANEL;
    COLORREF bord   = selected ? Pal::ACCENT : Pal::BORDER;
    COLORREF txtCol = selected ? Pal::TXT_PRI : Pal::TXT_SEC;
    RndRect(hdc, r, 8, fill, bord);

    int dotX = r.left+16, dotY = (r.top+r.bottom)/2;
    HPEN penW = CreatePen(PS_SOLID,2, selected?RGB(255,255,255):Pal::BORDER);
    HBRUSH brW = CreateSolidBrush(selected?RGB(255,255,255):Pal::BORDER);
    HPEN   op=(HPEN)SelectObject(hdc,penW);
    HBRUSH ob=(HBRUSH)SelectObject(hdc,brW);
    Ellipse(hdc,dotX-6,dotY-6,dotX+6,dotY+6);
    SelectObject(hdc,op); SelectObject(hdc,ob);
    DeleteObject(penW); DeleteObject(brW);
    if (selected) {
        HBRUSH inner = CreateSolidBrush(Pal::ACCENT);
        HPEN   np    = CreatePen(PS_SOLID,1,Pal::ACCENT);
        HPEN   op2=(HPEN)SelectObject(hdc,np);
        HBRUSH ob2=(HBRUSH)SelectObject(hdc,inner);
        Ellipse(hdc,dotX-3,dotY-3,dotX+3,dotY+3);
        SelectObject(hdc,op2); SelectObject(hdc,ob2);
        DeleteObject(inner); DeleteObject(np);
    }
    RECT lbl={r.left+32, r.top, r.right-4, r.bottom - (sub?12:0)};
    Txt(hdc,label,lbl,g.hFontBody,txtCol,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    if (sub) {
        RECT sl={r.left+32,r.bottom-22,r.right-4,r.bottom-4};
        Txt(hdc,sub,sl,g.hFontSub,
            selected?RGB(180,210,255):Pal::TXT_SEC,
            DT_LEFT|DT_TOP|DT_SINGLELINE);
    }
}

static LRESULT CALLBACK ModeCardProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR, DWORD_PTR)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC hdc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        FillRect(hdc,&rc,g.hBrushBg);
        int half = rc.right/2-6;
        RECT r1={0,0,half,rc.bottom}, r2={half+12,0,rc.right,rc.bottom};
        DrawModePill(hdc,r1,L"Receiver",L"Gaming PC",!g.isSender);
        DrawModePill(hdc,r2,L"Sender",  L"ESP / 2nd PC", g.isSender);
        EndPaint(h,&ps); return 0;
    }
    if (msg==WM_ERASEBKGND) return 1;
    if (msg==WM_LBUTTONDOWN) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h,&pt);
        RECT rc; GetClientRect(h,&rc);
        bool newSender = (pt.x > rc.right/2);
        if (newSender != g.isSender) {
            g.isSender = newSender;
            InvalidateRect(h,nullptr,TRUE);
            // Update IP label and rebuild hint
            if (g.hLblIP) {
                SetWindowTextW(g.hLblIP,
                    g.isSender ? L"Receiver IP" : L"Sender IP");
            }
            // Update button text
            if (g.hBtnLaunch) {
                SetWindowTextW(g.hBtnLaunch,
                    g.isSender ? L"\u26A1  Launch Sender"
                               : L"\u25B6  Launch Receiver");
            }
            InvalidateRect(g.hwnd,nullptr,TRUE);
        }
        return 0;
    }
    if (msg==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
    return DefWindowProcW(h,msg,wp,lp);
}

// ─── Launch button subclass ───────────────────────────────────
static WNDPROC g_origBtn  = nullptr;
static bool    g_btnHover = false, g_btnPress = false;
static LRESULT CALLBACK BtnProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch(msg) {
    case WM_MOUSEMOVE:
        if (!g_btnHover) { g_btnHover=true; InvalidateRect(h,nullptr,FALSE);
            TRACKMOUSEEVENT tme{sizeof(tme),TME_LEAVE,h,0}; TrackMouseEvent(&tme); }
        break;
    case WM_MOUSELEAVE: g_btnHover=false; g_btnPress=false; InvalidateRect(h,nullptr,FALSE); break;
    case WM_LBUTTONDOWN: g_btnPress=true;  InvalidateRect(h,nullptr,FALSE); break;
    case WM_LBUTTONUP:   g_btnPress=false; InvalidateRect(h,nullptr,FALSE); break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        COLORREF fill = g_btnPress ? Pal::ACCENT_PRESS
                      : g_btnHover ? Pal::ACCENT_HOVER : Pal::ACCENT;
        RndRect(hdc,rc,10,fill,fill);
        wchar_t txt[80]{}; GetWindowTextW(h,txt,80);
        Txt(hdc,txt,rc,g.hFontBtn,RGB(255,255,255),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        EndPaint(h,&ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return CallWindowProcW(g_origBtn,h,msg,wp,lp);
}

// ─── Edit subclass (dark bg) ──────────────────────────────────
static WNDPROC g_origEdit = nullptr;
static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg==WM_ERASEBKGND) {
        HDC hdc=(HDC)wp; RECT r; GetClientRect(h,&r);
        FillRect(hdc,&r,g.hBrushInput); return 1;
    }
    return CallWindowProcW(g_origEdit,h,msg,wp,lp);
}

// ─── INI helpers ─────────────────────────────────────────────
static std::string IniRead(const std::string& ini, const char* sec,
                            const char* key, const char* def)
{
    char buf[256]{}; GetPrivateProfileStringA(sec,key,def,buf,sizeof(buf),ini.c_str());
    return buf;
}
static void IniWrite(const std::string& ini, const char* sec,
                     const char* key, const char* val)
{
    WritePrivateProfileStringA(sec,key,val,ini.c_str());
}

// ─── Collect fields + save INI ───────────────────────────────
static std::string ExeDir()
{
    char p[MAX_PATH]{}; GetModuleFileNameA(nullptr,p,MAX_PATH);
    std::string s(p); auto sl=s.find_last_of("\\/");
    return (sl!=std::string::npos) ? s.substr(0,sl+1) : "";
}

static bool Collect(FuserConfig& cfg)
{
    cfg.isSender = g.isSender;
    cfg.localIP  = "0.0.0.0";

    wchar_t ipW[128]{}; GetWindowTextW(g.hEditIP,ipW,128);
    if (ipW[0]) {
        char n[64]{};
        WideCharToMultiByte(CP_ACP,0,ipW,-1,n,64,nullptr,nullptr);
        cfg.remoteIP = n;
    } else {
        cfg.remoteIP = "0.0.0.0";
    }

    wchar_t portW[16]{}; GetWindowTextW(g.hEditPort,portW,16);
    cfg.port = portW[0] ? static_cast<uint16_t>(_wtoi(portW))
                        : FUSER_PORT;
    cfg.captureMonitor = 0;
    cfg.adapterIndex   = -1;

    // Save to INI
    std::string ini = ExeDir() + "config.ini";
    IniWrite(ini,"Fuser","Mode", cfg.isSender ? "sender" : "receiver");
    IniWrite(ini,"Fuser","LocalIP",  cfg.localIP.c_str());
    IniWrite(ini,"Fuser","RemoteIP", cfg.remoteIP.c_str());
    char portStr[8]{}; _itoa_s(cfg.port,portStr,10);
    IniWrite(ini,"Fuser","Port",portStr);
    return true;
}

// ─── Main window WndProc ─────────────────────────────────────
static const wchar_t* CARD_CLS = L"KFModeCard2";

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch(msg) {
    // ── Create ──────────────────────────────────────────────
    case WM_CREATE: {
        g.hwnd = hwnd;

        g.hFontTitle  = MakeFont(18,true);
        g.hFontSub    = MakeFont(9, false);
        g.hFontBody   = MakeFont(10,false);
        g.hFontLabel  = MakeFont(8, true);
        g.hFontBtn    = MakeFont(11,true);
        g.hBrushBg    = CreateSolidBrush(Pal::BG);
        g.hBrushPanel = CreateSolidBrush(Pal::PANEL);
        g.hBrushAccent= CreateSolidBrush(Pal::ACCENT);
        g.hBrushInput = CreateSolidBrush(Pal::PANEL);

        // Mode card
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc  = DefWindowProcW;
        wc.hInstance    = GetModuleHandleW(nullptr);
        wc.lpszClassName= CARD_CLS;
        wc.hbrBackground= g.hBrushBg;
        RegisterClassExW(&wc);

        HWND hCard = CreateWindowExW(0, CARD_CLS, L"",
            WS_CHILD|WS_VISIBLE,
            26, 112, WIN_W-52, 60,
            hwnd, (HMENU)(UINT_PTR)ID_CARD_MODE,
            GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(hCard, ModeCardProc, 1, 0);
        g.hCardMode = hCard;

        // Load previous config for pre-fill
        std::string ini = ExeDir() + "config.ini";
        std::string prevMode  = IniRead(ini,"Fuser","Mode","receiver");
        std::string prevIP    = IniRead(ini,"Fuser","RemoteIP","AUTO");
        std::string prevPort  = IniRead(ini,"Fuser","Port","9877");
        g.isSender = (prevMode == "sender");

        // IP label
        g.hLblIP = CreateWindowExW(0, L"STATIC",
            g.isSender ? L"Receiver IP" : L"Sender IP",
            WS_CHILD|WS_VISIBLE,
            26, 206, 110, 20,
            hwnd, (HMENU)(UINT_PTR)ID_STATIC_IPLBL,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g.hLblIP, WM_SETFONT, (WPARAM)g.hFontBody, TRUE);

        // IP edit
        std::wstring ipW(prevIP.begin(), prevIP.end());
        g.hEditIP = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ipW.c_str(),
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            140, 202, WIN_W-168, 28,
            hwnd, (HMENU)(UINT_PTR)ID_EDIT_IP,
            GetModuleHandleW(nullptr), nullptr);
        SetWindowTheme(g.hEditIP, L" ", L" ");
        SendMessageW(g.hEditIP, WM_SETFONT, (WPARAM)g.hFontBody, TRUE);
        if (!g_origEdit)
            g_origEdit = (WNDPROC)GetWindowLongPtrW(g.hEditIP, GWLP_WNDPROC);
        SetWindowLongPtrW(g.hEditIP, GWLP_WNDPROC, (LONG_PTR)EditProc);

        // Port label
        HWND hPortLbl = CreateWindowExW(0, L"STATIC", L"Port",
            WS_CHILD|WS_VISIBLE,
            26, 250, 110, 20,
            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hPortLbl, WM_SETFONT, (WPARAM)g.hFontBody, TRUE);

        // Port edit
        std::wstring portW(prevPort.begin(), prevPort.end());
        g.hEditPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", portW.c_str(),
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_NUMBER,
            140, 246, 120, 28,
            hwnd, (HMENU)(UINT_PTR)ID_EDIT_PORT,
            GetModuleHandleW(nullptr), nullptr);
        SetWindowTheme(g.hEditPort, L" ", L" ");
        SendMessageW(g.hEditPort, WM_SETFONT, (WPARAM)g.hFontBody, TRUE);
        SetWindowLongPtrW(g.hEditPort, GWLP_WNDPROC, (LONG_PTR)EditProc);

        // Launch button
        g.hBtnLaunch = CreateWindowExW(0, L"BUTTON",
            g.isSender ? L"\u26A1  Launch Sender" : L"\u25B6  Launch Receiver",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            26, 306, WIN_W-52, 44,
            hwnd, (HMENU)(UINT_PTR)ID_BTN_LAUNCH,
            GetModuleHandleW(nullptr), nullptr);
        g_origBtn = (WNDPROC)GetWindowLongPtrW(g.hBtnLaunch, GWLP_WNDPROC);
        SetWindowLongPtrW(g.hBtnLaunch, GWLP_WNDPROC, (LONG_PTR)BtnProc);

        return 0;
    }

    // ── Paint ───────────────────────────────────────────────
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        FillRect(hdc, &rc, g.hBrushBg);

        // Header panel
        RECT hdr={0,0,rc.right,84};
        FillRect(hdc,&hdr,g.hBrushPanel);

        // Accent strip
        RECT strip={0,0,rc.right,3};
        FillRect(hdc,&strip,g.hBrushAccent);

        // Title
        RECT tR={20,10,rc.right-20,46};
        Txt(hdc,L"Knox Fuser",tR,g.hFontTitle,Pal::TXT_PRI,
            DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        // Subtitle
        RECT sR={20,46,rc.right-20,74};
        Txt(hdc,L"Zero-Latency Network Overlay",sR,g.hFontSub,Pal::TXT_SEC,
            DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        // Separator
        HPEN sep=CreatePen(PS_SOLID,1,Pal::SEP);
        HPEN op=(HPEN)SelectObject(hdc,sep);
        MoveToEx(hdc,0,84,nullptr); LineTo(hdc,rc.right,84);
        SelectObject(hdc,op); DeleteObject(sep);

        // MODE card bg
        RECT mCard={16,94,rc.right-16,188};
        RndRect(hdc,mCard,8,Pal::PANEL,Pal::BORDER);
        RECT mLbl={30,98,200,118};
        Txt(hdc,L"MODE",mLbl,g.hFontLabel,Pal::TXT_LABEL,
            DT_LEFT|DT_TOP|DT_SINGLELINE);

        // CONNECTION card bg
        RECT cCard={16,192,rc.right-16,290};
        RndRect(hdc,cCard,8,Pal::PANEL,Pal::BORDER);
        RECT cLbl={30,196,250,216};
        Txt(hdc,L"CONNECTION",cLbl,g.hFontLabel,Pal::TXT_LABEL,
            DT_LEFT|DT_TOP|DT_SINGLELINE);

        // Static control text colour fix
        EndPaint(hwnd,&ps);
        return 0;
    }

    // ── Static controls dark bg / text ──────────────────────
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, Pal::TXT_SEC);
        SetBkColor(hdc, Pal::PANEL);
        return (LRESULT)g.hBrushPanel;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, Pal::TXT_PRI);
        SetBkColor(hdc, Pal::PANEL);
        return (LRESULT)g.hBrushInput;
    }

    // ── Commands ─────────────────────────────────────────────
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_LAUNCH) {
            if (g.outCfg && Collect(*g.outCfg)) {
                g.launched = true;
                DestroyWindow(hwnd);
            }
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─────────────────────────────────────────────────────────────
//  Public entry point
// ─────────────────────────────────────────────────────────────
bool ShowConfigUI(FuserConfig& outCfg)
{
    g = {};   // reset all state
    g.outCfg = &outCfg;

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // Register main window class
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc  = WndProc;
    wc.hInstance    = hInst;
    wc.hbrBackground= (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName= L"KnoxFuserConfig2";
    wc.hCursor      = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon        = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Centre on primary monitor
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    DWORD style   = WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    RECT  adj     = {0,0,WIN_W,WIN_H};
    AdjustWindowRect(&adj, style, FALSE);
    int ww = adj.right-adj.left, wh = adj.bottom-adj.top;
    int wx = (sx-ww)/2, wy = (sy-wh)/2;

    HWND hwnd = CreateWindowExW(
        0, L"KnoxFuserConfig2",
        L"Knox Fuser \u2014 Configuration",
        style,
        wx, wy, ww, wh,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd) return false;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return g.launched;
}
