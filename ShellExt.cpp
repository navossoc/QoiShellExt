// DLL entry points: class factory, COM registration and unregistration.
//
// The two handlers register independently. DllRegisterServer installs
// everything compiled into the DLL; DllInstall installs one of them, which is
// how regsvr32 lets each user pick:
//
//   regsvr32 /n /i:thumbnail QoiShellExt.dll
//   regsvr32 /n /i:preview   QoiShellExt.dll
//   regsvr32 /u /n /i:preview QoiShellExt.dll
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#include "ShellExt.h"

#include <shlwapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <new>

HINSTANCE g_hInst = nullptr;
LONG g_cDllRef = 0;

#ifdef QOI_ENABLE_THUMBNAIL
// {7BF3DDA8-F6F6-49E0-8F62-7B15E312302C}
const CLSID CLSID_QoiThumbnailProvider =
{ 0x7bf3dda8, 0xf6f6, 0x49e0, { 0x8f, 0x62, 0x7b, 0x15, 0xe3, 0x12, 0x30, 0x2c } };
const wchar_t* const kThumbnailClsidText = L"{7BF3DDA8-F6F6-49E0-8F62-7B15E312302C}";
const wchar_t* const kThumbnailName = L"QOI Thumbnail Provider";

static const wchar_t* const kThumbnailShellex =
    L".qoi\\shellex\\{E357FCCD-A995-4576-B01F-234630154E96}";
#endif

#ifdef QOI_ENABLE_PREVIEW
// {BDB2367F-2BC8-4503-A870-AF02BD359A65}
const CLSID CLSID_QoiPreviewHandler =
{ 0xbdb2367f, 0x2bc8, 0x4503, { 0xa8, 0x70, 0xaf, 0x02, 0xbd, 0x35, 0x9a, 0x65 } };
const wchar_t* const kPreviewClsidText = L"{BDB2367F-2BC8-4503-A870-AF02BD359A65}";
const wchar_t* const kPreviewName = L"QOI Preview Handler";

static const wchar_t* const kPreviewShellex =
    L".qoi\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";

// prevhost.exe, the surrogate every preview handler runs inside.
static const wchar_t* const kPreviewHostAppId =
    L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}";

static const wchar_t* const kPreviewHandlerListKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";
#endif

static const wchar_t* const kApprovedKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";

// ------------------------------------------------------------- class factory

class ClassFactory : public IClassFactory
{
public:
    typedef IUnknown* (*Creator)();

    explicit ClassFactory(Creator creator) : m_cRef(1), m_creator(creator)
    {
        InterlockedIncrement(&g_cDllRef);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = {
            QITABENT(ClassFactory, IClassFactory),
            { nullptr, 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0)
            delete this;
        return cRef;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
    {
        if (pUnkOuter)
            return CLASS_E_NOAGGREGATION;

        IUnknown* instance = m_creator();
        if (!instance)
            return E_OUTOFMEMORY;

        const HRESULT hr = instance->QueryInterface(riid, ppv);
        instance->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL fLock) override
    {
        if (fLock)
            InterlockedIncrement(&g_cDllRef);
        else
            InterlockedDecrement(&g_cDllRef);
        return S_OK;
    }

private:
    ~ClassFactory() { InterlockedDecrement(&g_cDllRef); }

    LONG m_cRef;
    Creator m_creator;
};

// ------------------------------------------------------------ registry helpers

static LONG SetValue(HKEY root, const wchar_t* subKey, const wchar_t* name,
                     const wchar_t* value)
{
    return RegSetKeyValueW(root, subKey, name, REG_SZ, value,
                           static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
}

static void DeleteClsid(const wchar_t* clsidText)
{
    wchar_t key[128];
    StringCchPrintfW(key, ARRAYSIZE(key), L"CLSID\\%s", clsidText);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, key);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, key);
}

// Only clears the association if it still points at us; otherwise another
// handler has already taken the extension over.
static void DeleteShellexIfOurs(const wchar_t* shellexKey, const wchar_t* clsidText)
{
    wchar_t current[128] = {};
    DWORD cb = sizeof(current);
    if (RegGetValueW(HKEY_CLASSES_ROOT, shellexKey, nullptr, RRF_RT_REG_SZ,
                     nullptr, current, &cb) == ERROR_SUCCESS &&
        _wcsicmp(current, clsidText) == 0)
    {
        RegDeleteTreeW(HKEY_CLASSES_ROOT, shellexKey);
        RegDeleteKeyW(HKEY_CLASSES_ROOT, shellexKey);
    }
}

static void DeleteListValue(const wchar_t* listKey, const wchar_t* valueName)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, listKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        RegDeleteValueW(key, valueName);
        RegCloseKey(key);
    }
}

static HRESULT GetModulePath(wchar_t (&path)[MAX_PATH])
{
    if (GetModuleFileNameW(g_hInst, path, MAX_PATH) == 0)
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

// ------------------------------------------------------- per-handler registration

#ifdef QOI_ENABLE_THUMBNAIL
static HRESULT RegisterThumbnail()
{
    wchar_t modulePath[MAX_PATH] = {};
    HRESULT hr = GetModulePath(modulePath);
    if (FAILED(hr))
        return hr;

    wchar_t clsidKey[128];
    wchar_t inprocKey[160];
    StringCchPrintfW(clsidKey, ARRAYSIZE(clsidKey), L"CLSID\\%s", kThumbnailClsidText);
    StringCchPrintfW(inprocKey, ARRAYSIZE(inprocKey), L"CLSID\\%s\\InprocServer32",
                     kThumbnailClsidText);

    LONG rc = SetValue(HKEY_CLASSES_ROOT, clsidKey, nullptr, kThumbnailName);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);
    rc = SetValue(HKEY_CLASSES_ROOT, inprocKey, nullptr, modulePath);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);
    // "Both": the thumbnail host calls from MTA threads, and "Apartment" would
    // force needless marshalling.
    rc = SetValue(HKEY_CLASSES_ROOT, inprocKey, L"ThreadingModel", L"Both");
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);
    rc = SetValue(HKEY_CLASSES_ROOT, kThumbnailShellex, nullptr, kThumbnailClsidText);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);

    SetValue(HKEY_LOCAL_MACHINE, kApprovedKey, kThumbnailClsidText, kThumbnailName);
    return S_OK;
}

static void UnregisterThumbnail()
{
    DeleteShellexIfOurs(kThumbnailShellex, kThumbnailClsidText);
    DeleteClsid(kThumbnailClsidText);
    DeleteListValue(kApprovedKey, kThumbnailClsidText);
}
#endif  // QOI_ENABLE_THUMBNAIL

#ifdef QOI_ENABLE_PREVIEW
static HRESULT RegisterPreview()
{
    wchar_t modulePath[MAX_PATH] = {};
    HRESULT hr = GetModulePath(modulePath);
    if (FAILED(hr))
        return hr;

    wchar_t clsidKey[128];
    wchar_t inprocKey[160];
    StringCchPrintfW(clsidKey, ARRAYSIZE(clsidKey), L"CLSID\\%s", kPreviewClsidText);
    StringCchPrintfW(inprocKey, ARRAYSIZE(inprocKey), L"CLSID\\%s\\InprocServer32",
                     kPreviewClsidText);

    LONG rc = SetValue(HKEY_CLASSES_ROOT, clsidKey, nullptr, kPreviewName);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);
    SetValue(HKEY_CLASSES_ROOT, clsidKey, L"DisplayName", kPreviewName);
    // Without the AppID the handler would load inside Explorer itself.
    rc = SetValue(HKEY_CLASSES_ROOT, clsidKey, L"AppID", kPreviewHostAppId);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);
    rc = SetValue(HKEY_CLASSES_ROOT, inprocKey, nullptr, modulePath);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);
    // A preview handler owns a window, so it is bound to an apartment.
    rc = SetValue(HKEY_CLASSES_ROOT, inprocKey, L"ThreadingModel", L"Apartment");
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);
    rc = SetValue(HKEY_CLASSES_ROOT, kPreviewShellex, nullptr, kPreviewClsidText);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);

    // The shell only honours preview handlers listed here.
    rc = SetValue(HKEY_LOCAL_MACHINE, kPreviewHandlerListKey, kPreviewClsidText,
                  kPreviewName);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);

    SetValue(HKEY_LOCAL_MACHINE, kApprovedKey, kPreviewClsidText, kPreviewName);
    return S_OK;
}

static void UnregisterPreview()
{
    DeleteShellexIfOurs(kPreviewShellex, kPreviewClsidText);
    DeleteClsid(kPreviewClsidText);
    DeleteListValue(kPreviewHandlerListKey, kPreviewClsidText);
    DeleteListValue(kApprovedKey, kPreviewClsidText);
}
#endif  // QOI_ENABLE_PREVIEW

// Harmless to write more than once, and it belongs to the file type rather
// than to either handler.
static void RegisterFileType()
{
    SetValue(HKEY_CLASSES_ROOT, L".qoi", L"PerceivedType", L"image");
}

// ---------------------------------------------------------------- entry points

STDAPI DllRegisterServer()
{
    HRESULT hr = S_OK;

#ifdef QOI_ENABLE_THUMBNAIL
    hr = RegisterThumbnail();
    if (FAILED(hr))
        return hr;
#endif
#ifdef QOI_ENABLE_PREVIEW
    hr = RegisterPreview();
    if (FAILED(hr))
        return hr;
#endif

    RegisterFileType();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return hr;
}

STDAPI DllUnregisterServer()
{
#ifdef QOI_ENABLE_THUMBNAIL
    UnregisterThumbnail();
#endif
#ifdef QOI_ENABLE_PREVIEW
    UnregisterPreview();
#endif

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

// regsvr32 /n /i:<what>, with <what> being "thumbnail", "preview" or "all".
// An empty command line means "all", matching DllRegisterServer.
STDAPI DllInstall(BOOL bInstall, PCWSTR pszCmdLine)
{
    const wchar_t* what = (pszCmdLine && *pszCmdLine) ? pszCmdLine : L"all";

    const bool all = (_wcsicmp(what, L"all") == 0);
    const bool thumbnail = all || (_wcsicmp(what, L"thumbnail") == 0);
    const bool preview = all || (_wcsicmp(what, L"preview") == 0);

    if (!thumbnail && !preview)
        return E_INVALIDARG;  // unknown keyword

#ifndef QOI_ENABLE_THUMBNAIL
    if (thumbnail && !all)
        return E_NOTIMPL;  // this build carries no thumbnail provider
#endif
#ifndef QOI_ENABLE_PREVIEW
    if (preview && !all)
        return E_NOTIMPL;  // this build carries no preview handler
#endif

    HRESULT hr = S_OK;

    if (bInstall)
    {
#ifdef QOI_ENABLE_THUMBNAIL
        if (thumbnail)
        {
            hr = RegisterThumbnail();
            if (FAILED(hr))
                return hr;
        }
#endif
#ifdef QOI_ENABLE_PREVIEW
        if (preview)
        {
            hr = RegisterPreview();
            if (FAILED(hr))
                return hr;
        }
#endif
        RegisterFileType();
    }
    else
    {
#ifdef QOI_ENABLE_THUMBNAIL
        if (thumbnail)
            UnregisterThumbnail();
#endif
#ifdef QOI_ENABLE_PREVIEW
        if (preview)
            UnregisterPreview();
#endif
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return hr;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    ClassFactory::Creator creator = nullptr;

#ifdef QOI_ENABLE_THUMBNAIL
    if (IsEqualCLSID(rclsid, CLSID_QoiThumbnailProvider))
        creator = CreateQoiThumbnailProvider;
#endif
#ifdef QOI_ENABLE_PREVIEW
    if (IsEqualCLSID(rclsid, CLSID_QoiPreviewHandler))
        creator = CreateQoiPreviewHandler;
#endif

    if (!creator)
        return CLASS_E_CLASSNOTAVAILABLE;

    ClassFactory* factory = new (std::nothrow) ClassFactory(creator);
    if (!factory)
        return E_OUTOFMEMORY;

    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_cDllRef == 0) ? S_OK : S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hInst = hModule;
        // The preview handler creates windows, so thread notifications are of
        // no use to us either way.
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
