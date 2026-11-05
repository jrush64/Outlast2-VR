// Outlast 2 VR - DXGI proxy and boot layer.
//
// Ships as a proxy dxgi.dll next to Outlast2.exe. It forwards every real dxgi export to
// System32\dxgi.dll, then patches the DXGI vtables on the way past so the proxy owns CreateSwapChain
// and Present. The boot layer itself alters nothing: it logs what the renderer is and hands the
// device and swapchain to the camera, UE3 and OpenXR layers.
//
// It answers, in one run:
//   does the proxy load at all, which factory path the game takes, what the swapchain looks
//   like (size, format, windowed, buffer count, MSAA), which D3D11 feature level and device,
//   and whether the game presents through Present or Present1.
//
// Log: %LOCALAPPDATA%\Outlast2VR\Outlast2VR_Log.txt
//
// The hook idiom follows the Batman: Arkham Knight VR proxy (same UE3 x64 D3D11 family).

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>

#include "ol2_common.h"
#include "ol2_ue3.h"
#include "ol2_xr.h"

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

namespace
{
CRITICAL_SECTION g_logLock;
bool g_logReady = false;
wchar_t g_logPath[MAX_PATH] = {};

// CreateDirectoryW only makes ONE level. Walk the path and make every component, or the log
// silently never appears on a machine where the parent folder does not already exist.
void EnsureDirectoryTree(const wchar_t* path)
{
    wchar_t work[MAX_PATH] = {};
    wcsncpy_s(work, path, _TRUNCATE);

    for (wchar_t* cursor = work; *cursor != L'\0'; ++cursor)
    {
        if (*cursor == L'\\' && cursor != work)
        {
            *cursor = L'\0';
            CreateDirectoryW(work, nullptr);
            *cursor = L'\\';
        }
    }

    CreateDirectoryW(work, nullptr);
}

void InitLog()
{
    InitializeCriticalSection(&g_logLock);

    wchar_t folder[MAX_PATH] = {};
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH);
    if (got == 0 || got >= MAX_PATH)
    {
        // No LOCALAPPDATA is not a reason to lose the log. Fall back beside the game exe.
        GetModuleFileNameW(nullptr, folder, MAX_PATH);
        wchar_t* slash = wcsrchr(folder, L'\\');
        if (slash != nullptr) *slash = L'\0';
        swprintf_s(g_logPath, L"%s\\Outlast2VR_Log.txt", folder);
    }
    else
    {
        wchar_t dir[MAX_PATH] = {};
        swprintf_s(dir, L"%s\\Outlast2VR", folder);
        EnsureDirectoryTree(dir);
        swprintf_s(g_logPath, L"%s\\Outlast2VR_Log.txt", dir);
    }

    HANDLE file = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);

    g_logReady = true;
}

void LogLineV(const char* format, va_list args)
{
    if (!g_logReady) return;

    char body[1024] = {};
    // vsnprintf truncates cleanly. sprintf_s would terminate the process on overflow, which is
    // how an Outlast 1 probe once died printing its own diagnostic line.
    vsnprintf(body, sizeof(body) - 1, format, args);

    SYSTEMTIME now = {};
    GetLocalTime(&now);

    char line[1200] = {};
    const int length = _snprintf_s(line, sizeof(line) - 1, _TRUNCATE, "[%02u:%02u:%02u.%03u] %s\r\n",
                                   now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, body);
    if (length <= 0) return;

    EnterCriticalSection(&g_logLock);
    HANDLE file = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }
    LeaveCriticalSection(&g_logLock);

    OutputDebugStringA(line);
}

void LogLine(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogLineV(format, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// settings
//
// EnableXR=0 runs the mod FLAT: the - Hinny that GOAT - engine still renders the stereo pair side by side into the
// backbuffer, so both eyes are visible on the monitor, but no OpenXR session is created and no
// headset is involved. The render bugs are all visible in that mode, so there is no reason to
// wear anything to work on them.
// ---------------------------------------------------------------------------

bool g_xrEnabled = false;

void LoadSettings()
{
    wchar_t ini[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, ini, MAX_PATH);
    wchar_t* slash = wcsrchr(ini, L'\\');
    if (slash != nullptr) *(slash + 1) = L'\0';
    wcsncat_s(ini, L"outlast2vr.ini", _TRUNCATE);

    g_xrEnabled = GetPrivateProfileIntW(L"VR", L"EnableXR", 0, ini) != 0;

    LogLine("[OL2CFG] %S", ini);
    LogLine("[OL2CFG] EnableXR=%d -> %s", g_xrEnabled ? 1 : 0,
            g_xrEnabled ? "VR: OpenXR session will be created"
                        : "FLAT: no OpenXR, no headset. Stereo pair renders side by side on the monitor.");
}

// ---------------------------------------------------------------------------
// real dxgi.dll
// ---------------------------------------------------------------------------

HMODULE g_realDxgi = nullptr;

bool EnsureRealDxgi()
{
    if (g_realDxgi != nullptr) return true;

    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        wcsncpy_s(path, L"C:\\Windows\\System32", _TRUNCATE);
    }
    wcsncat_s(path, L"\\dxgi.dll", _TRUNCATE);

    g_realDxgi = LoadLibraryW(path);
    if (g_realDxgi == nullptr)
    {
        LogLine("[OL2BOOT] FAILED to load real dxgi.dll err=%lu", GetLastError());
        return false;
    }

    LogLine("[OL2BOOT] real dxgi.dll loaded base=0x%p", g_realDxgi);
    return true;
}

template <typename T>
T ResolveReal(const char* name)
{
    if (!EnsureRealDxgi()) return nullptr;
    FARPROC proc = GetProcAddress(g_realDxgi, name);
    if (proc == nullptr) LogLine("[OL2BOOT] missing real dxgi export=%s", name);
    return reinterpret_cast<T>(proc);
}

// ---------------------------------------------------------------------------
// vtable patching
// ---------------------------------------------------------------------------

bool PatchVTable(void* object, size_t slot, void* hook, void** original)
{
    if (object == nullptr || hook == nullptr || original == nullptr) return false;

    void** vtable = *reinterpret_cast<void***>(object);
    if (vtable == nullptr) return false;
    if (vtable[slot] == hook) return false;   // already ours

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    *original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

CreateSwapChainFn g_originalCreateSwapChain = nullptr;
CreateSwapChainForHwndFn g_originalCreateSwapChainForHwnd = nullptr;
PresentFn g_originalPresent = nullptr;
Present1Fn g_originalPresent1 = nullptr;

volatile LONG g_frames = 0;
bool g_describedSwapChain = false;
unsigned g_backbufferWidth = 0;
unsigned g_backbufferHeight = 0;

const char* FormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
    default:                                return "other";
    }
}

// Fires once, on the first frame that actually reaches the screen. Everything here is a read.
void DescribeSwapChain(IDXGISwapChain* swapChain)
{
    if (g_describedSwapChain || swapChain == nullptr) return;
    g_describedSwapChain = true;

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swapChain->GetDesc(&desc)))
    {
        LogLine("[OL2SWAP] GetDesc FAILED");
        return;
    }

    g_backbufferWidth = desc.BufferDesc.Width;
    g_backbufferHeight = desc.BufferDesc.Height;

    LogLine("[OL2SWAP] %ux%u fmt=%s(%d) refresh=%u/%u buffers=%u msaa=%ux q=%u windowed=%u hwnd=0x%p flags=0x%X",
            desc.BufferDesc.Width, desc.BufferDesc.Height,
            FormatName(desc.BufferDesc.Format), static_cast<int>(desc.BufferDesc.Format),
            desc.BufferDesc.RefreshRate.Numerator, desc.BufferDesc.RefreshRate.Denominator,
            desc.BufferCount, desc.SampleDesc.Count, desc.SampleDesc.Quality,
            desc.Windowed ? 1u : 0u, desc.OutputWindow, desc.Flags);

    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device != nullptr)
    {
        const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);

        LogLine("[OL2SWAP] d3d11 device=0x%p context=0x%p featureLevel=0x%04X creationFlags=0x%X",
                device, context, static_cast<unsigned>(level), device->GetCreationFlags());

        if (context != nullptr) context->Release();
        device->Release();
    }
    else
    {
        LogLine("[OL2SWAP] swapchain is NOT a D3D11 device - unexpected, check the renderer assumption");
    }

    IDXGISwapChain1* swapChain1 = nullptr;
    const bool isSwapChain1 = SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1),
                                                                 reinterpret_cast<void**>(&swapChain1))) && swapChain1 != nullptr;
    if (isSwapChain1) swapChain1->Release();
    LogLine("[OL2SWAP] IDXGISwapChain1 available=%u", isSwapChain1 ? 1u : 0u);

    // Bring up the OpenXR session on the game's own device, matching its backbuffer format.
    if (!g_xrEnabled)
    {
        LogLine("[OL2CFG] flat mode - skipping OpenXR entirely. Set EnableXR=1 in outlast2vr.ini for the headset.");
        return;
    }

    ID3D11Device* xrDevice = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&xrDevice))) && xrDevice != nullptr)
    {
        Ol2Xr::TryCreateSession(xrDevice, static_cast<int64_t>(desc.BufferDesc.Format),
                                desc.BufferDesc.Width, desc.BufferDesc.Height);
        xrDevice->Release();
    }
}

void OnFrame(IDXGISwapChain* swapChain, const char* path)
{
    const LONG frame = InterlockedIncrement(&g_frames);

    if (frame == 1)
    {
        LogLine("[OL2BOOT] FIRST PRESENT via %s sc=0x%p", path, swapChain);
        DescribeSwapChain(swapChain);
    }
    else if (frame == 600 || frame == 3600)
    {
        LogLine("[OL2FRAME] %ld frames presented via %s", frame, path);
    }

    Ol2Camera::OnPresent(swapChain, g_backbufferWidth, g_backbufferHeight);

    // Every XR D3D11 call happens here, on the render thread. The device is SINGLETHREADED.
    if (g_xrEnabled) Ol2Xr::OnPresent(swapChain);
}

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    OnFrame(swapChain, "Present");
    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{
    OnFrame(swapChain, "Present1");
    return g_originalPresent1(swapChain, syncInterval, flags, parameters);
}

void InstallSwapChainHooks(IDXGISwapChain* swapChain, const char* where)
{
    if (swapChain == nullptr) return;

    const bool present = PatchVTable(swapChain, 8, reinterpret_cast<void*>(&PresentHook),
                                     reinterpret_cast<void**>(&g_originalPresent));

    bool present1 = false;
    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1))) && swapChain1 != nullptr)
    {
        // A UE3 fork can present through Present1 (slot 22) instead of Present.
        present1 = PatchVTable(swapChain1, 22, reinterpret_cast<void*>(&Present1Hook),
                               reinterpret_cast<void**>(&g_originalPresent1));
        swapChain1->Release();
    }

    LogLine("[OL2BOOT] swapchain hooks at %s sc=0x%p present=%u present1=%u",
            where, swapChain, present ? 1u : 0u, present1 ? 1u : 0u);

    Ol2Camera::InstallContextHooks(swapChain, where);
    Ol2Xr::SetGameSwapChain(swapChain);
}

HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain)
{
    const HRESULT hr = g_originalCreateSwapChain(factory, device, desc, swapChain);
    LogLine("[OL2BOOT] CreateSwapChain hr=0x%08X sc=0x%p", static_cast<unsigned>(hr),
            swapChain != nullptr ? *swapChain : nullptr);
    if (SUCCEEDED(hr) && swapChain != nullptr) InstallSwapChainHooks(*swapChain, "CreateSwapChain");
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrictTo, IDXGISwapChain1** swapChain)
{
    const HRESULT hr = g_originalCreateSwapChainForHwnd(factory, device, hwnd, desc, fullscreen, restrictTo, swapChain);
    LogLine("[OL2BOOT] CreateSwapChainForHwnd hr=0x%08X sc=0x%p", static_cast<unsigned>(hr),
            swapChain != nullptr ? *swapChain : nullptr);
    if (SUCCEEDED(hr) && swapChain != nullptr) InstallSwapChainHooks(*swapChain, "CreateSwapChainForHwnd");
    return hr;
}

void InstallFactoryHooks(void* factory, const char* where)
{
    if (factory == nullptr) return;

    const bool base = PatchVTable(factory, 10, reinterpret_cast<void*>(&CreateSwapChainHook),
                                  reinterpret_cast<void**>(&g_originalCreateSwapChain));

    bool hwnd = false;
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(reinterpret_cast<IUnknown*>(factory)->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))) && factory2 != nullptr)
    {
        hwnd = PatchVTable(factory2, 15, reinterpret_cast<void*>(&CreateSwapChainForHwndHook),
                           reinterpret_cast<void**>(&g_originalCreateSwapChainForHwnd));
        factory2->Release();
    }

    LogLine("[OL2BOOT] factory hooks at %s factory=0x%p createSwapChain=%u createSwapChainForHwnd=%u",
            where, factory, base ? 1u : 0u, hwnd ? 1u : 0u);
}
}

// Bridges out of the anonymous namespace, so the camera layer shares one logger and one
// vtable patcher rather than duplicating them.
void Ol2Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogLineV(format, args);
    va_end(args);
}

bool Ol2PatchVTable(void* object, size_t slot, void* hook, void** original)
{
    return PatchVTable(object, slot, hook, original);
}

bool Ol2XrEnabled() { return g_xrEnabled; }

// ---------------------------------------------------------------------------
// dxgi exports
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** factory)
{
    const auto real = ResolveReal<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory");
    if (real == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const HRESULT hr = real(riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr && *factory != nullptr) InstallFactoryHooks(*factory, "CreateDXGIFactory");
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** factory)
{
    const auto real = ResolveReal<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory1");
    if (real == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const HRESULT hr = real(riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr && *factory != nullptr) InstallFactoryHooks(*factory, "CreateDXGIFactory1");
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    const auto real = ResolveReal<HRESULT(WINAPI*)(UINT, REFIID, void**)>("CreateDXGIFactory2");
    if (real == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const HRESULT hr = real(flags, riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr && *factory != nullptr) InstallFactoryHooks(*factory, "CreateDXGIFactory2");
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI Proxy_DXGIDeclareAdapterRemovalSupport()
{
    const auto real = ResolveReal<HRESULT(WINAPI*)()>("DXGIDeclareAdapterRemovalSupport");
    if (real == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return real();
}

extern "C" __declspec(dllexport) HRESULT WINAPI Proxy_DXGIGetDebugInterface(REFIID riid, void** debugInterface)
{
    const auto real = ResolveReal<HRESULT(WINAPI*)(REFIID, void**)>("DXGIGetDebugInterface");
    if (real == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return real(riid, debugInterface);
}

extern "C" __declspec(dllexport) HRESULT WINAPI Proxy_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debugInterface)
{
    const auto real = ResolveReal<HRESULT(WINAPI*)(UINT, REFIID, void**)>("DXGIGetDebugInterface1");
    if (real == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return real(flags, riid, debugInterface);
}

// ---------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------

namespace
{
DWORD WINAPI BootThread(LPVOID parameter)
{
    const HMODULE self = static_cast<HMODULE>(parameter);

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    const HMODULE gameModule = GetModuleHandleW(nullptr);
    SIZE_T imageSize = 0;
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(gameModule);
    if (dos != nullptr && dos->e_magic == IMAGE_DOS_SIGNATURE)
    {
        IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(dos) + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) imageSize = nt->OptionalHeader.SizeOfImage;
    }

    LogLine("[OL2BOOT] proxy attached. self=0x%p exe=%S", self, exePath);
    LogLine("[OL2BOOT] game module base=0x%p imageSize=0x%IX (add this base to any RVA)", gameModule, imageSize);
    // Hook d3d11's device entry points before the game creates a device, so every device and
    // every context is ours from birth rather than only whichever one owns the swapchain.
    Ol2Camera::InstallDeviceExportHooks();

    // Engine-side stereo. Independent of the renderer hooks, so it goes in as soon as the
    // module is live rather than waiting for a swapchain.
    Ol2Ue3::Init();

    LogLine("[OL2BOOT] waiting for the game to create its DXGI factory.");

    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        InitLog();
        LoadSettings();

        HANDLE thread = CreateThread(nullptr, 0, BootThread, module, 0, nullptr);
        if (thread != nullptr) CloseHandle(thread);
    }

    return TRUE;
}
