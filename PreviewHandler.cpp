// IPreviewHandler for .qoi files - the Explorer preview pane.
//
// The preview is a plain child window owned by this DLL, painted inside the
// prevhost.exe surrogate: no helper process to launch and reparent, and
// resizing is an ordinary WM_SIZE.
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#include "ShellExt.h"
#include "QoiImage.h"

#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <new>

#ifdef QOI_ENABLE_PREVIEW

static const wchar_t* const kWindowClass = L"QoiPreviewHandlerWindow";

// Checkerboard drawn behind images with transparency, so a mostly transparent
// file does not look like an empty pane.
static const int kCheckerSize = 8;

class QoiPreviewHandler : public IInitializeWithStream,
                          public IPreviewHandler,
                          public IPreviewHandlerVisuals,
                          public IOleWindow,
                          public IObjectWithSite
{
public:
    QoiPreviewHandler() :
        m_cRef(1),
        m_stream(nullptr),
        m_site(nullptr),
        m_hwndParent(nullptr),
        m_hwnd(nullptr),
        m_rcParent(),
        m_background(GetSysColor(COLOR_WINDOW)),
        m_textColor(GetSysColor(COLOR_WINDOWTEXT)),
        m_font(nullptr),
        m_cache(nullptr),
        m_cacheW(0),
        m_cacheH(0),
        m_decoded(false),
        m_decodeFailed(false)
    {
        InterlockedIncrement(&g_cDllRef);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = {
            QITABENT(QoiPreviewHandler, IInitializeWithStream),
            QITABENT(QoiPreviewHandler, IPreviewHandler),
            QITABENT(QoiPreviewHandler, IPreviewHandlerVisuals),
            QITABENT(QoiPreviewHandler, IOleWindow),
            QITABENT(QoiPreviewHandler, IObjectWithSite),
            { nullptr, 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0)
            delete this;
        return cRef;
    }

    // -------------------------------------------------- IInitializeWithStream

    IFACEMETHODIMP Initialize(IStream* stream, DWORD /*grfMode*/) override
    {
        if (!stream)
            return E_INVALIDARG;
        if (m_stream)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        m_stream = stream;
        m_stream->AddRef();
        return S_OK;
    }

    // -------------------------------------------------------- IPreviewHandler

    IFACEMETHODIMP SetWindow(HWND hwnd, const RECT* prc) override
    {
        if (!hwnd || !prc)
            return E_INVALIDARG;

        m_hwndParent = hwnd;
        m_rcParent = *prc;

        if (m_hwnd)
        {
            SetParent(m_hwnd, m_hwndParent);
            MoveToParentRect();
        }
        return S_OK;
    }

    IFACEMETHODIMP SetRect(const RECT* prc) override
    {
        if (!prc)
            return E_INVALIDARG;

        m_rcParent = *prc;
        if (m_hwnd)
            MoveToParentRect();
        return S_OK;
    }

    IFACEMETHODIMP DoPreview() override
    {
        if (!m_stream)
            return E_UNEXPECTED;
        if (!m_hwndParent)
            return E_UNEXPECTED;

        DecodeOnce();

        if (!m_hwnd)
        {
            RegisterWindowClass();
            m_hwnd = CreateWindowExW(
                0, kWindowClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                m_rcParent.left, m_rcParent.top,
                m_rcParent.right - m_rcParent.left,
                m_rcParent.bottom - m_rcParent.top,
                m_hwndParent, nullptr, g_hInst, this);
            if (!m_hwnd)
                return HRESULT_FROM_WIN32(GetLastError());
        }
        else
        {
            MoveToParentRect();
        }

        InvalidateCache();
        ShowWindow(m_hwnd, SW_SHOW);
        InvalidateRect(m_hwnd, nullptr, TRUE);
        return S_OK;
    }

    IFACEMETHODIMP Unload() override
    {
        if (m_hwnd)
        {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
        InvalidateCache();
        m_image = qoiimg::Image();
        m_decoded = false;
        m_decodeFailed = false;
        if (m_stream)
        {
            m_stream->Release();
            m_stream = nullptr;
        }
        return S_OK;
    }

    IFACEMETHODIMP SetFocus() override
    {
        if (!m_hwnd)
            return S_FALSE;
        ::SetFocus(m_hwnd);
        return S_OK;
    }

    IFACEMETHODIMP QueryFocus(HWND* phwnd) override
    {
        if (!phwnd)
            return E_INVALIDARG;
        *phwnd = ::GetFocus();
        return *phwnd ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    IFACEMETHODIMP TranslateAccelerator(MSG* pmsg) override
    {
        // The frame owns the shortcuts; we have no input of our own.
        if (!m_site)
            return S_FALSE;

        IPreviewHandlerFrame* frame = nullptr;
        HRESULT hr = m_site->QueryInterface(IID_PPV_ARGS(&frame));
        if (SUCCEEDED(hr))
        {
            hr = frame->TranslateAccelerator(pmsg);
            frame->Release();
        }
        return SUCCEEDED(hr) ? hr : S_FALSE;
    }

    // ------------------------------------------------- IPreviewHandlerVisuals

    IFACEMETHODIMP SetBackgroundColor(COLORREF color) override
    {
        m_background = color & 0x00FFFFFF;
        Repaint();
        return S_OK;
    }

    IFACEMETHODIMP SetFont(const LOGFONTW* plf) override
    {
        if (!plf)
            return E_INVALIDARG;
        if (m_font)
        {
            DeleteObject(m_font);
            m_font = nullptr;
        }
        m_font = CreateFontIndirectW(plf);
        Repaint();
        return S_OK;
    }

    IFACEMETHODIMP SetTextColor(COLORREF color) override
    {
        m_textColor = color & 0x00FFFFFF;
        Repaint();
        return S_OK;
    }

    // -------------------------------------------------------------- IOleWindow

    IFACEMETHODIMP GetWindow(HWND* phwnd) override
    {
        if (!phwnd)
            return E_INVALIDARG;
        *phwnd = m_hwnd;
        return m_hwnd ? S_OK : E_FAIL;
    }

    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

    // ---------------------------------------------------------- IObjectWithSite

    IFACEMETHODIMP SetSite(IUnknown* site) override
    {
        if (m_site)
        {
            m_site->Release();
            m_site = nullptr;
        }
        m_site = site;
        if (m_site)
            m_site->AddRef();
        return S_OK;
    }

    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_INVALIDARG;
        *ppv = nullptr;
        if (!m_site)
            return E_FAIL;
        return m_site->QueryInterface(riid, ppv);
    }

private:
    ~QoiPreviewHandler()
    {
        Unload();
        if (m_site)
            m_site->Release();
        if (m_font)
            DeleteObject(m_font);
        InterlockedDecrement(&g_cDllRef);
    }

    void MoveToParentRect()
    {
        SetWindowPos(m_hwnd, nullptr,
                     m_rcParent.left, m_rcParent.top,
                     m_rcParent.right - m_rcParent.left,
                     m_rcParent.bottom - m_rcParent.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void Repaint()
    {
        if (m_hwnd)
            InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    void InvalidateCache()
    {
        if (m_cache)
        {
            DeleteObject(m_cache);
            m_cache = nullptr;
        }
        m_cacheW = m_cacheH = 0;
    }

    void DecodeOnce()
    {
        if (m_decoded || m_decodeFailed)
            return;
        m_decodeFailed = FAILED(qoiimg::DecodeStream(m_stream, m_image));
        m_decoded = !m_decodeFailed;
    }

    // ------------------------------------------------------------- rendering

    void Render(HDC hdc, const RECT& client)
    {
        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;
        if (cw <= 0 || ch <= 0)
            return;

        HBRUSH bg = CreateSolidBrush(m_background);
        FillRect(hdc, &client, bg);
        DeleteObject(bg);

        if (!m_image.valid())
        {
            DrawMessage(hdc, client, L"Unable to decode this QOI file.");
            return;
        }

        // Scale to fit, preserving aspect ratio. Unlike the thumbnail, the
        // preview pane does enlarge - that is what the user expects when a
        // small image is selected.
        const double scale = min(static_cast<double>(cw) / m_image.width,
                                 static_cast<double>(ch) / m_image.height);
        int dw = static_cast<int>(m_image.width * scale + 0.5);
        int dh = static_cast<int>(m_image.height * scale + 0.5);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;

        const int x = client.left + (cw - dw) / 2;
        const int y = client.top + (ch - dh) / 2;

        if (!m_cache || m_cacheW != dw || m_cacheH != dh)
        {
            InvalidateCache();
            // Premultiplied, because AlphaBlend with AC_SRC_ALPHA requires it.
            m_cache = qoiimg::CreateScaledDib(m_image, dw, dh, true);
            m_cacheW = dw;
            m_cacheH = dh;
        }
        if (!m_cache)
            return;

        RECT dest = { x, y, x + dw, y + dh };
        if (m_image.hasAlpha)
            DrawCheckerboard(hdc, dest);

        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(mem, m_cache);

        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(hdc, x, y, dw, dh, mem, 0, 0, dw, dh, blend);

        SelectObject(mem, old);
        DeleteDC(mem);
    }

    void DrawCheckerboard(HDC hdc, const RECT& rc)
    {
        // Derived from the pane background so it stays subtle in either theme.
        const int r = GetRValue(m_background);
        const int g = GetGValue(m_background);
        const int b = GetBValue(m_background);
        const int delta = (r + g + b > 3 * 128) ? -16 : 16;
        const COLORREF light = m_background;
        const COLORREF dark = RGB(Clamp(r + delta), Clamp(g + delta), Clamp(b + delta));

        HBRUSH lightBrush = CreateSolidBrush(light);
        HBRUSH darkBrush = CreateSolidBrush(dark);

        FillRect(hdc, &rc, lightBrush);

        int row = 0;
        for (int yy = rc.top; yy < rc.bottom; yy += kCheckerSize, ++row)
        {
            int col = 0;
            for (int xx = rc.left; xx < rc.right; xx += kCheckerSize, ++col)
            {
                if (((row + col) & 1) == 0)
                    continue;
                RECT cell = { xx, yy,
                              min(xx + kCheckerSize, rc.right),
                              min(yy + kCheckerSize, rc.bottom) };
                FillRect(hdc, &cell, darkBrush);
            }
        }

        DeleteObject(lightBrush);
        DeleteObject(darkBrush);
    }

    static int Clamp(int v)
    {
        return (v < 0) ? 0 : ((v > 255) ? 255 : v);
    }

    void DrawMessage(HDC hdc, const RECT& client, const wchar_t* text)
    {
        HGDIOBJ oldFont = nullptr;
        if (m_font)
            oldFont = SelectObject(hdc, m_font);

        const int oldMode = SetBkMode(hdc, TRANSPARENT);
        const COLORREF oldColor = ::SetTextColor(hdc, m_textColor);

        RECT rc = client;
        DrawTextW(hdc, text, -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        ::SetTextColor(hdc, oldColor);
        SetBkMode(hdc, oldMode);
        if (oldFont)
            SelectObject(hdc, oldFont);
    }

    // Paints through an off-screen bitmap; the pane is resized continuously
    // while the user drags the splitter and direct painting flickers.
    void PaintDoubleBuffered(HDC hdc, const RECT& client)
    {
        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;
        if (cw <= 0 || ch <= 0)
            return;

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
        if (!mem || !bmp)
        {
            if (bmp)
                DeleteObject(bmp);
            if (mem)
                DeleteDC(mem);
            Render(hdc, client);
            return;
        }

        HGDIOBJ old = SelectObject(mem, bmp);
        RECT local = { 0, 0, cw, ch };
        Render(mem, local);
        BitBlt(hdc, client.left, client.top, cw, ch, mem, 0, 0, SRCCOPY);

        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    static void RegisterWindowClass()
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        // Already registered by another instance in this process is fine.
        RegisterClassExW(&wc);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        QoiPreviewHandler* self = reinterpret_cast<QoiPreviewHandler*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_NCCREATE:
        {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_ERASEBKGND:
            // Render() fills the whole client area.
            return 1;

        case WM_PAINT:
            if (self)
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT client;
                GetClientRect(hwnd, &client);
                self->PaintDoubleBuffered(hdc, client);
                EndPaint(hwnd, &ps);
                return 0;
            }
            break;

        case WM_PRINTCLIENT:
            // Lets PrintWindow() capture the pane, which the test harness uses.
            if (self)
            {
                RECT client;
                GetClientRect(hwnd, &client);
                self->Render(reinterpret_cast<HDC>(wParam), client);
                return 0;
            }
            break;

        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LONG m_cRef;
    IStream* m_stream;
    IUnknown* m_site;

    HWND m_hwndParent;
    HWND m_hwnd;
    RECT m_rcParent;

    COLORREF m_background;
    COLORREF m_textColor;
    HFONT m_font;

    qoiimg::Image m_image;
    HBITMAP m_cache;   // m_image scaled to the current pane size, premultiplied
    int m_cacheW;
    int m_cacheH;
    bool m_decoded;
    bool m_decodeFailed;
};

IUnknown* CreateQoiPreviewHandler()
{
    return static_cast<IInitializeWithStream*>(new (std::nothrow) QoiPreviewHandler());
}

#endif  // QOI_ENABLE_PREVIEW
