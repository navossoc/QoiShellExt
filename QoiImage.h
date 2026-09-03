// Shared decoding and scaling helpers used by both shell extensions.
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <vector>

namespace qoiimg
{
    // Sanity limits. Anything past these is not worth previewing.
    const ULONGLONG kMaxStreamBytes = 512ull * 1024 * 1024;
    const ULONGLONG kMaxPixels = 100ull * 1000 * 1000;

    // A decoded image, always 4 channels, RGBA, straight (un-premultiplied)
    // alpha - the order qoi.h produces.
    struct Image
    {
        std::vector<BYTE> rgba;
        int width = 0;
        int height = 0;
        bool hasAlpha = false;  // false when the file declared 3 channels

        bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
    };

    // Reads the whole stream into `out`. Uses Stat() to size the buffer up
    // front and falls back to incremental growth when the size is unknown.
    HRESULT ReadWholeStream(IStream* stream, std::vector<BYTE>& out);

    // Decodes a QOI byte buffer. Fails on a bad header or an image past
    // kMaxPixels.
    HRESULT Decode(const BYTE* data, size_t size, Image& out);

    HRESULT DecodeStream(IStream* stream, Image& out);

    // Scales `src` into a 32 bpp BGRA buffer.
    //
    // Minification uses a box filter, magnification bilinear; both weight the
    // color by alpha. Without that weighting the (arbitrary) color of fully
    // transparent pixels bleeds into the edges and shows up as a dark halo.
    //
    // `premultiply` controls the output: premultiplied for AlphaBlend, straight
    // for a thumbnail handed back as WTSAT_ARGB.
    void Resample(const Image& src, BYTE* dst, int dw, int dh, int dstStride,
                  bool premultiply);

    // Creates a top-down 32 bpp DIB section holding `src` scaled to dw x dh.
    // Returns NULL on failure.
    HBITMAP CreateScaledDib(const Image& src, int dw, int dh, bool premultiply);
}
