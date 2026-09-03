// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#include "QoiImage.h"

#include <new>

#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable : 4244)  // intentional truncations in upstream qoi.h
#include "qoi.h"
#pragma warning(pop)

namespace qoiimg
{
    HRESULT ReadWholeStream(IStream* stream, std::vector<BYTE>& out)
    {
        STATSTG stat = {};
        ULONGLONG hint = 0;
        if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)))
        {
            if (stat.cbSize.QuadPart > kMaxStreamBytes)
                return E_OUTOFMEMORY;
            hint = stat.cbSize.QuadPart;
        }

        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);

        try
        {
            out.clear();
            out.reserve(hint ? static_cast<size_t>(hint) : 64 * 1024);
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }

        BYTE buffer[64 * 1024];
        for (;;)
        {
            ULONG read = 0;
            const HRESULT hr = stream->Read(buffer, sizeof(buffer), &read);
            if (FAILED(hr))
                return hr;
            if (read == 0)
                break;
            if (out.size() + read > kMaxStreamBytes)
                return E_OUTOFMEMORY;
            try
            {
                out.insert(out.end(), buffer, buffer + read);
            }
            catch (const std::bad_alloc&)
            {
                return E_OUTOFMEMORY;
            }
            if (hr == S_FALSE)
                break;
        }
        return out.empty() ? E_FAIL : S_OK;
    }

    HRESULT Decode(const BYTE* data, size_t size, Image& out)
    {
        if (!data || size < 14 || memcmp(data, "qoif", 4) != 0)
            return E_FAIL;
        if (size > static_cast<size_t>(INT_MAX))
            return E_OUTOFMEMORY;

        qoi_desc desc = {};
        BYTE* pixels = static_cast<BYTE*>(
            qoi_decode(data, static_cast<int>(size), &desc, 4));
        if (!pixels)
            return E_FAIL;

        // qoi.h hands back a malloc'd buffer; release it on every path.
        struct FreeGuard
        {
            void* p;
            ~FreeGuard() { free(p); }
        } guard{ pixels };

        const int w = static_cast<int>(desc.width);
        const int h = static_cast<int>(desc.height);
        if (w <= 0 || h <= 0 || static_cast<ULONGLONG>(w) * h > kMaxPixels)
            return E_FAIL;

        try
        {
            out.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }

        out.width = w;
        out.height = h;
        out.hasAlpha = (desc.channels == 4);
        return S_OK;
    }

    HRESULT DecodeStream(IStream* stream, Image& out)
    {
        if (!stream)
            return E_INVALIDARG;

        std::vector<BYTE> data;
        const HRESULT hr = ReadWholeStream(stream, data);
        if (FAILED(hr))
            return hr;
        return Decode(data.data(), data.size(), out);
    }

    // Area average over the source rect that maps to each destination pixel.
    static void ResampleBox(const Image& src, BYTE* dst, int dw, int dh,
                            int dstStride, bool premultiply)
    {
        const int sw = src.width;
        const int sh = src.height;
        const BYTE* base = src.rgba.data();

        for (int y = 0; y < dh; ++y)
        {
            const int y0 = static_cast<int>(static_cast<long long>(y) * sh / dh);
            int y1 = static_cast<int>(static_cast<long long>(y + 1) * sh / dh);
            if (y1 <= y0)
                y1 = y0 + 1;

            BYTE* row = dst + static_cast<size_t>(y) * dstStride;

            for (int x = 0; x < dw; ++x)
            {
                const int x0 = static_cast<int>(static_cast<long long>(x) * sw / dw);
                int x1 = static_cast<int>(static_cast<long long>(x + 1) * sw / dw);
                if (x1 <= x0)
                    x1 = x0 + 1;

                unsigned long long sr = 0, sg = 0, sb = 0, sa = 0;
                for (int sy = y0; sy < y1; ++sy)
                {
                    const BYTE* p = base + (static_cast<size_t>(sy) * sw + x0) * 4;
                    for (int sx = x0; sx < x1; ++sx, p += 4)
                    {
                        const unsigned a = p[3];
                        sr += static_cast<unsigned long long>(p[0]) * a;
                        sg += static_cast<unsigned long long>(p[1]) * a;
                        sb += static_cast<unsigned long long>(p[2]) * a;
                        sa += a;
                    }
                }

                const unsigned long long count =
                    static_cast<unsigned long long>(x1 - x0) * (y1 - y0);

                BYTE* o = row + static_cast<size_t>(x) * 4;
                if (sa == 0)
                {
                    o[0] = o[1] = o[2] = o[3] = 0;
                    continue;
                }

                const unsigned alpha = static_cast<unsigned>(sa / count);
                if (premultiply)
                {
                    // sr/count is already color*alpha averaged, i.e. exactly
                    // the premultiplied value.
                    o[0] = static_cast<BYTE>(sb / count / 255);
                    o[1] = static_cast<BYTE>(sg / count / 255);
                    o[2] = static_cast<BYTE>(sr / count / 255);
                }
                else
                {
                    o[0] = static_cast<BYTE>(sb / sa);
                    o[1] = static_cast<BYTE>(sg / sa);
                    o[2] = static_cast<BYTE>(sr / sa);
                }
                o[3] = static_cast<BYTE>(alpha);
            }
        }
    }

    static void ResampleBilinear(const Image& src, BYTE* dst, int dw, int dh,
                                 int dstStride, bool premultiply)
    {
        const int sw = src.width;
        const int sh = src.height;
        const BYTE* base = src.rgba.data();

        // Map destination pixel centers back to source pixel centers.
        const float xRatio = static_cast<float>(sw) / dw;
        const float yRatio = static_cast<float>(sh) / dh;

        for (int y = 0; y < dh; ++y)
        {
            float fy = (y + 0.5f) * yRatio - 0.5f;
            if (fy < 0) fy = 0;
            int y0 = static_cast<int>(fy);
            if (y0 > sh - 1) y0 = sh - 1;
            int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
            const float wy = fy - y0;

            BYTE* row = dst + static_cast<size_t>(y) * dstStride;

            for (int x = 0; x < dw; ++x)
            {
                float fx = (x + 0.5f) * xRatio - 0.5f;
                if (fx < 0) fx = 0;
                int x0 = static_cast<int>(fx);
                if (x0 > sw - 1) x0 = sw - 1;
                int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
                const float wx = fx - x0;

                const BYTE* p00 = base + (static_cast<size_t>(y0) * sw + x0) * 4;
                const BYTE* p01 = base + (static_cast<size_t>(y0) * sw + x1) * 4;
                const BYTE* p10 = base + (static_cast<size_t>(y1) * sw + x0) * 4;
                const BYTE* p11 = base + (static_cast<size_t>(y1) * sw + x1) * 4;

                const float w00 = (1 - wx) * (1 - wy);
                const float w01 = wx * (1 - wy);
                const float w10 = (1 - wx) * wy;
                const float w11 = wx * wy;

                // Weight color by alpha, same reason as the box filter.
                const float a00 = p00[3] * w00, a01 = p01[3] * w01;
                const float a10 = p10[3] * w10, a11 = p11[3] * w11;
                const float sa = a00 + a01 + a10 + a11;

                BYTE* o = row + static_cast<size_t>(x) * 4;
                if (sa <= 0.0f)
                {
                    o[0] = o[1] = o[2] = o[3] = 0;
                    continue;
                }

                const float sr = p00[0] * a00 + p01[0] * a01 + p10[0] * a10 + p11[0] * a11;
                const float sg = p00[1] * a00 + p01[1] * a01 + p10[1] * a10 + p11[1] * a11;
                const float sb = p00[2] * a00 + p01[2] * a01 + p10[2] * a10 + p11[2] * a11;

                const float alpha = sa;  // weights sum to 1, so this is the average
                const float scale = premultiply ? (alpha / 255.0f) : 1.0f;

                o[0] = static_cast<BYTE>(sb / sa * scale + 0.5f);
                o[1] = static_cast<BYTE>(sg / sa * scale + 0.5f);
                o[2] = static_cast<BYTE>(sr / sa * scale + 0.5f);
                o[3] = static_cast<BYTE>(alpha + 0.5f);
            }
        }
    }

    void Resample(const Image& src, BYTE* dst, int dw, int dh, int dstStride,
                  bool premultiply)
    {
        if (!src.valid() || dw <= 0 || dh <= 0)
            return;

        if (dw < src.width || dh < src.height)
            ResampleBox(src, dst, dw, dh, dstStride, premultiply);
        else
            ResampleBilinear(src, dst, dw, dh, dstStride, premultiply);
    }

    HBITMAP CreateScaledDib(const Image& src, int dw, int dh, bool premultiply)
    {
        if (!src.valid() || dw <= 0 || dh <= 0)
            return nullptr;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = dw;
        bmi.bmiHeader.biHeight = -dh;  // negative: top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits,
                                          nullptr, 0);
        if (!bitmap || !bits)
        {
            if (bitmap)
                DeleteObject(bitmap);
            return nullptr;
        }

        Resample(src, static_cast<BYTE*>(bits), dw, dh, dw * 4, premultiply);
        return bitmap;
    }
}
