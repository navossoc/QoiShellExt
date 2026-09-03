// Shared declarations: the two CLSIDs this DLL implements and the DLL-wide
// reference count that keeps it loaded.
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#pragma once

// Which handlers are compiled in. A plain build has both; define exactly one of
// these to get a DLL that only carries that handler. Choosing what to *install*
// does not need a separate build - see DllInstall in ShellExt.cpp.
#if !defined(QOI_ENABLE_THUMBNAIL) && !defined(QOI_ENABLE_PREVIEW)
#define QOI_ENABLE_THUMBNAIL
#define QOI_ENABLE_PREVIEW
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

#ifdef QOI_ENABLE_THUMBNAIL
// {7BF3DDA8-F6F6-49E0-8F62-7B15E312302C}
extern const CLSID CLSID_QoiThumbnailProvider;
extern const wchar_t* const kThumbnailClsidText;
extern const wchar_t* const kThumbnailName;
IUnknown* CreateQoiThumbnailProvider();
#endif

#ifdef QOI_ENABLE_PREVIEW
// {BDB2367F-2BC8-4503-A870-AF02BD359A65}
extern const CLSID CLSID_QoiPreviewHandler;
extern const wchar_t* const kPreviewClsidText;
extern const wchar_t* const kPreviewName;
IUnknown* CreateQoiPreviewHandler();
#endif

extern HINSTANCE g_hInst;
extern LONG g_cDllRef;
