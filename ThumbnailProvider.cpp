// IThumbnailProvider for .qoi files.
//
// The decode happens inside the thumbnail host process: no helper executable,
// no temp file, no disk round-trip.
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#include "ShellExt.h"
#include "QoiImage.h"

#include <shlwapi.h>
#include <thumbcache.h>
#include <new>

#ifdef QOI_ENABLE_THUMBNAIL

// Explorer never asks for more than a few hundred pixels; this only guards
// against a caller passing something absurd.
static const UINT kMaxThumbnailSize = 10000;

class QoiThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider
{
public:
    QoiThumbnailProvider() : m_cRef(1), m_stream(nullptr)
    {
        InterlockedIncrement(&g_cDllRef);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = {
            QITABENT(QoiThumbnailProvider, IInitializeWithStream),
            QITABENT(QoiThumbnailProvider, IThumbnailProvider),
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

    // IInitializeWithStream
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

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override
    {
        if (!phbmp || !pdwAlpha)
            return E_POINTER;
        *phbmp = nullptr;
        *pdwAlpha = WTSAT_UNKNOWN;

        if (!m_stream)
            return E_UNEXPECTED;
        if (cx == 0 || cx > kMaxThumbnailSize)
            return E_INVALIDARG;

        qoiimg::Image image;
        const HRESULT hr = qoiimg::DecodeStream(m_stream, image);
        if (FAILED(hr))
            return hr;

        // Never enlarge: an image that already fits in cx is returned at its
        // native size.
        const int longest = (image.width > image.height) ? image.width : image.height;
        int dw = image.width;
        int dh = image.height;
        if (static_cast<UINT>(longest) > cx)
        {
            const double scale = static_cast<double>(cx) / longest;
            dw = static_cast<int>(image.width * scale + 0.5);
            dh = static_cast<int>(image.height * scale + 0.5);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
        }

        // WTSAT_ARGB means straight alpha, which is what QOI stores
        // ("un-premultiplied", see qoi.h).
        HBITMAP bitmap = qoiimg::CreateScaledDib(image, dw, dh, false);
        if (!bitmap)
            return E_OUTOFMEMORY;

        *phbmp = bitmap;
        *pdwAlpha = image.hasAlpha ? WTSAT_ARGB : WTSAT_RGB;
        return S_OK;
    }

private:
    ~QoiThumbnailProvider()
    {
        if (m_stream)
            m_stream->Release();
        InterlockedDecrement(&g_cDllRef);
    }

    LONG m_cRef;
    IStream* m_stream;
};

IUnknown* CreateQoiThumbnailProvider()
{
    return static_cast<IInitializeWithStream*>(new (std::nothrow) QoiThumbnailProvider());
}

#endif  // QOI_ENABLE_THUMBNAIL
