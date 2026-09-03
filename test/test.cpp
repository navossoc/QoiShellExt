// Test harness: loads the DLL without registering it in COM, drives one of the
// two handlers and writes the result to out.bmp for inspection.
//
//   test.exe thumb   <file.qoi> [cx] [iterations]
//   test.exe preview <file.qoi> [width] [height]
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <objbase.h>
#include <stdio.h>

typedef HRESULT(STDAPICALLTYPE* PFN_DllGetClassObject)(REFCLSID, REFIID, void**);

// {7BF3DDA8-F6F6-49E0-8F62-7B15E312302C}
static const CLSID CLSID_QoiThumbnailProvider =
{ 0x7bf3dda8, 0xf6f6, 0x49e0, { 0x8f, 0x62, 0x7b, 0x15, 0xe3, 0x12, 0x30, 0x2c } };

// {BDB2367F-2BC8-4503-A870-AF02BD359A65}
static const CLSID CLSID_QoiPreviewHandler =
{ 0xbdb2367f, 0x2bc8, 0x4503, { 0xa8, 0x70, 0xaf, 0x02, 0xbd, 0x35, 0x9a, 0x65 } };

static PFN_DllGetClassObject g_getClassObject = nullptr;

static bool LoadExtension()
{
    HMODULE dll = LoadLibraryW(L"QoiShellExt.dll");
    if (!dll)
    {
        wprintf(L"LoadLibrary failed: %lu\n", GetLastError());
        return false;
    }
    g_getClassObject = reinterpret_cast<PFN_DllGetClassObject>(
        GetProcAddress(dll, "DllGetClassObject"));
    if (!g_getClassObject)
    {
        wprintf(L"DllGetClassObject is not exported\n");
        return false;
    }
    return true;
}

static HRESULT CreateHandler(REFCLSID clsid, REFIID riid, void** ppv)
{
    IClassFactory* factory = nullptr;
    HRESULT hr = g_getClassObject(clsid, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        return hr;
    hr = factory->CreateInstance(nullptr, riid, ppv);
    factory->Release();
    return hr;
}

static bool SaveBitmap(HBITMAP bitmap, const wchar_t* path)
{
    BITMAP bm = {};
    if (!GetObject(bitmap, sizeof(bm), &bm))
        return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -bm.bmHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const DWORD size = bm.bmWidth * bm.bmHeight * 4;
    BYTE* bits = new BYTE[size];

    HDC dc = GetDC(nullptr);
    const int scanned = GetDIBits(dc, bitmap, 0, bm.bmHeight, bits, &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (scanned == 0)
    {
        delete[] bits;
        return false;
    }

    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + size;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        delete[] bits;
        return false;
    }
    DWORD written = 0;
    WriteFile(file, &fh, sizeof(fh), &written, nullptr);
    WriteFile(file, &bmi.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr);
    WriteFile(file, bits, size, &written, nullptr);
    CloseHandle(file);
    delete[] bits;
    return true;
}

// ------------------------------------------------------------------ thumbnail

static int RunThumbnail(const wchar_t* input, UINT cx, int iterations)
{
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;

    for (int i = 0; i < iterations; ++i)
    {
        if (bitmap)
        {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }

        IStream* stream = nullptr;
        HRESULT hr = SHCreateStreamOnFileEx(input, STGM_READ | STGM_SHARE_DENY_WRITE,
                                            0, FALSE, nullptr, &stream);
        if (FAILED(hr))
        {
            wprintf(L"cannot open %s: 0x%08X\n", input, hr);
            return 1;
        }

        IInitializeWithStream* init = nullptr;
        hr = CreateHandler(CLSID_QoiThumbnailProvider, IID_PPV_ARGS(&init));
        if (FAILED(hr))
        {
            wprintf(L"CreateInstance: 0x%08X\n", hr);
            return 1;
        }

        hr = init->Initialize(stream, 0);
        if (FAILED(hr))
        {
            wprintf(L"Initialize: 0x%08X\n", hr);
            return 1;
        }

        IThumbnailProvider* provider = nullptr;
        init->QueryInterface(IID_PPV_ARGS(&provider));
        hr = provider->GetThumbnail(cx, &bitmap, &alpha);

        provider->Release();
        init->Release();
        stream->Release();

        if (FAILED(hr))
        {
            wprintf(L"GetThumbnail: 0x%08X\n", hr);
            return 1;
        }
    }

    QueryPerformanceCounter(&t1);
    const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;

    BITMAP bm = {};
    GetObject(bitmap, sizeof(bm), &bm);
    wprintf(L"ok: %ldx%ld, %d bpp, alpha=%d\n", bm.bmWidth, bm.bmHeight,
            bm.bmBitsPixel, alpha);
    wprintf(L"%d iterations in %.2f ms (%.3f ms per thumbnail)\n",
            iterations, ms, ms / iterations);

    if (SaveBitmap(bitmap, L"out.bmp"))
        wprintf(L"wrote out.bmp\n");

    DeleteObject(bitmap);
    return 0;
}

// -------------------------------------------------------------------- preview

static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int RunPreview(const wchar_t* input, int width, int height)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QoiPreviewTestHost";
    RegisterClassExW(&wc);

    HWND host = CreateWindowExW(0, wc.lpszClassName, L"host", WS_OVERLAPPEDWINDOW,
                                0, 0, width, height, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!host)
    {
        wprintf(L"cannot create the host window: %lu\n", GetLastError());
        return 1;
    }

    IStream* stream = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(input, STGM_READ | STGM_SHARE_DENY_WRITE,
                                        0, FALSE, nullptr, &stream);
    if (FAILED(hr))
    {
        wprintf(L"cannot open %s: 0x%08X\n", input, hr);
        return 1;
    }

    IInitializeWithStream* init = nullptr;
    hr = CreateHandler(CLSID_QoiPreviewHandler, IID_PPV_ARGS(&init));
    if (FAILED(hr))
    {
        wprintf(L"CreateInstance: 0x%08X\n", hr);
        return 1;
    }

    hr = init->Initialize(stream, 0);
    if (FAILED(hr))
    {
        wprintf(L"Initialize: 0x%08X\n", hr);
        return 1;
    }

    IPreviewHandler* preview = nullptr;
    hr = init->QueryInterface(IID_PPV_ARGS(&preview));
    if (FAILED(hr))
    {
        wprintf(L"QueryInterface(IPreviewHandler): 0x%08X\n", hr);
        return 1;
    }

    // Same call order Explorer uses.
    RECT rc = { 0, 0, width, height };
    preview->SetWindow(host, &rc);

    IPreviewHandlerVisuals* visuals = nullptr;
    if (SUCCEEDED(preview->QueryInterface(IID_PPV_ARGS(&visuals))))
    {
        visuals->SetBackgroundColor(GetSysColor(COLOR_WINDOW));
        visuals->SetTextColor(GetSysColor(COLOR_WINDOWTEXT));
        visuals->Release();
    }

    preview->SetRect(&rc);

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    hr = preview->DoPreview();
    QueryPerformanceCounter(&t1);

    if (FAILED(hr))
    {
        wprintf(L"DoPreview: 0x%08X\n", hr);
        return 1;
    }
    wprintf(L"DoPreview: %.2f ms\n",
            (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);

    IOleWindow* oleWindow = nullptr;
    HWND pane = nullptr;
    if (SUCCEEDED(preview->QueryInterface(IID_PPV_ARGS(&oleWindow))))
    {
        oleWindow->GetWindow(&pane);
        oleWindow->Release();
    }
    if (!pane)
    {
        wprintf(L"the handler did not create a window\n");
        return 1;
    }

    // Paint into a bitmap through WM_PRINTCLIENT: no visible window needed and
    // the result is deterministic.
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(mem, bmp);
    SendMessageW(pane, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT);
    SelectObject(mem, old);

    if (SaveBitmap(bmp, L"out.bmp"))
        wprintf(L"ok: %dx%d pane, wrote out.bmp\n", width, height);

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    preview->Unload();
    preview->Release();
    init->Release();
    stream->Release();
    DestroyWindow(host);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        wprintf(L"usage: test.exe thumb   <file.qoi> [cx] [iterations]\n");
        wprintf(L"       test.exe preview <file.qoi> [width] [height]\n");
        return 1;
    }

    const wchar_t* mode = argv[1];
    const wchar_t* input = argv[2];

    // Apartment threaded: the preview handler owns a window.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!LoadExtension())
        return 1;

    int rc = 1;
    if (_wcsicmp(mode, L"thumb") == 0)
    {
        rc = RunThumbnail(input,
                          (argc > 3) ? _wtoi(argv[3]) : 256,
                          (argc > 4) ? _wtoi(argv[4]) : 1);
    }
    else if (_wcsicmp(mode, L"preview") == 0)
    {
        rc = RunPreview(input,
                        (argc > 3) ? _wtoi(argv[3]) : 400,
                        (argc > 4) ? _wtoi(argv[4]) : 400);
    }
    else
    {
        wprintf(L"unknown mode: %s\n", mode);
    }

    CoUninitialize();
    return rc;
}
