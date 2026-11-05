#include "ol2_xr.h"

#include "ol2_common.h"
#include "ol2_me1_shims.h"
#include "ol2_xr_types.h"
#include "ol2_ue3.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace MELEVR::Xr;

namespace
{
std::atomic_bool g_started{false};
std::atomic_bool g_ready{false};
std::atomic_bool g_shutdown{false};

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
int64_t g_gameFormat = 0;
uint32_t g_backbufferWidth = 0;
uint32_t g_backbufferHeight = 0;

HMODULE g_loader = nullptr;
PFN_xrGetInstanceProcAddr g_getProc = nullptr;
XrInstance g_instance = nullptr;
XrSystemId g_systemId = 0;
Functions g_fn = {};
XrSession g_session = nullptr;
XrSpace g_viewSpace = nullptr;
XrSpace g_localSpace = nullptr;
XrSessionState g_sessionState = 0;
bool g_sessionRunning = false;

// One swapchain per eye (true stereo projection layer).
struct EyeSwapchain
{
    XrSwapchain swapchain = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
};

EyeSwapchain g_eyes[2];  // [0]=left, [1]=right
int64_t g_swapchainFormat = 0;

std::atomic<unsigned long long> g_frameCount{0};

bool g_headRefValid = false;
float g_headRefYawDeg = 0.0f;
float g_headRefPitchDeg = 0.0f;
bool g_lastHeadPoseValid = false;
XrPosef g_lastHeadPose = {};
std::atomic<unsigned long long> g_cameraDriveFrames{0};

template <typename T>
void SafeRelease(T*& ptr) noexcept
{
    if (ptr != nullptr)
    {
        ptr->Release();
        ptr = nullptr;
    }
}

std::string FmtXr(XrResult result)
{
    return "XrResult " + std::to_string(static_cast<int>(result));
}

void CopyAscii(char* dst, size_t cap, const char* src)
{
    if (dst == nullptr || cap == 0) return;
    size_t i = 0;
    for (; src != nullptr && src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

int64_t SrgbSwapchainFormat(int64_t fmt) noexcept
{
    switch (fmt)
    {
    case 28: return 29;  // R8G8B8A8_UNORM -> _SRGB
    case 87: return 91;  // B8G8R8A8_UNORM -> _SRGB
    default: return fmt;
    }
}

bool ResolveProc(const char* name, PFN_xrVoidFunction* out)
{
    *out = nullptr;
    const XrResult r = g_getProc(g_instance, name, out);
    if (!XrSucceeded(r) || *out == nullptr)
    {
        ME1VR::Log::Line(std::string("[OL2XR] resolve ") + name + " failed: " + FmtXr(r));
        return false;
    }
    return true;
}

bool CreateInstance()
{
    g_loader = LoadLibraryW(L"openxr_loader.dll");
    if (g_loader == nullptr)
    {
        ME1VR::Log::WindowsError("[OL2XR] LoadLibraryW(openxr_loader.dll) failed", GetLastError());
        return false;
    }
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"openxr_loader.dll", &pinned);
    ME1VR::Log::Line("[OL2XR] openxr_loader.dll loaded.");

    g_getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
    if (g_getProc == nullptr)
    {
        ME1VR::Log::WindowsError("[OL2XR] xrGetInstanceProcAddr missing", GetLastError());
        return false;
    }

    PFN_xrVoidFunction createInstanceFn = nullptr;
    const XrResult resolve = g_getProc(nullptr, "xrCreateInstance", &createInstanceFn);
    if (!XrSucceeded(resolve) || createInstanceFn == nullptr)
    {
        ME1VR::Log::Line("[OL2XR] xrCreateInstance resolve failed: " + FmtXr(resolve));
        return false;
    }

    const char* extensions[] = {kD3D11ExtensionName};
    XrInstanceCreateInfo info = {};
    info.type = XR_TYPE_INSTANCE_CREATE_INFO_VALUE;
    CopyAscii(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName), "ME1VR M3 Geo11 Stereo");
    info.applicationInfo.applicationVersion = 1;
    CopyAscii(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "ME1VR");
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = MakeXrVersion(1, 0, 0);
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;

    const XrResult r = reinterpret_cast<PFN_xrCreateInstance>(createInstanceFn)(&info, &g_instance);
    ME1VR::Log::Line("[OL2XR] xrCreateInstance: " + FmtXr(r));
    return XrSucceeded(r) && g_instance != nullptr;
}

bool ResolveSessionFunctions()
{
    PFN_xrVoidFunction p = nullptr;
#define RESOLVE(name, field, type)            \
    if (!ResolveProc(name, &p)) return false; \
    g_fn.field = reinterpret_cast<type>(p)

    RESOLVE("xrDestroyInstance", destroyInstance, PFN_xrDestroyInstance);
    RESOLVE("xrPollEvent", pollEvent, PFN_xrPollEvent);
    RESOLVE("xrGetSystem", getSystem, PFN_xrGetSystem);
    RESOLVE("xrEnumerateViewConfigurationViews", enumerateViewConfigurationViews, PFN_xrEnumerateViewConfigurationViews);
    RESOLVE("xrGetD3D11GraphicsRequirementsKHR", getD3D11GraphicsRequirements, PFN_xrGetD3D11GraphicsRequirementsKHR);
    RESOLVE("xrCreateSession", createSession, PFN_xrCreateSession);
    RESOLVE("xrDestroySession", destroySession, PFN_xrDestroySession);
    RESOLVE("xrEnumerateSwapchainFormats", enumerateSwapchainFormats, PFN_xrEnumerateSwapchainFormats);
    RESOLVE("xrCreateSwapchain", createSwapchain, PFN_xrCreateSwapchain);
    RESOLVE("xrDestroySwapchain", destroySwapchain, PFN_xrDestroySwapchain);
    RESOLVE("xrEnumerateSwapchainImages", enumerateSwapchainImages, PFN_xrEnumerateSwapchainImages);
    RESOLVE("xrAcquireSwapchainImage", acquireSwapchainImage, PFN_xrAcquireSwapchainImage);
    RESOLVE("xrWaitSwapchainImage", waitSwapchainImage, PFN_xrWaitSwapchainImage);
    RESOLVE("xrReleaseSwapchainImage", releaseSwapchainImage, PFN_xrReleaseSwapchainImage);
    RESOLVE("xrBeginSession", beginSession, PFN_xrBeginSession);
    RESOLVE("xrEndSession", endSession, PFN_xrEndSession);
    RESOLVE("xrWaitFrame", waitFrame, PFN_xrWaitFrame);
    RESOLVE("xrBeginFrame", beginFrame, PFN_xrBeginFrame);
    RESOLVE("xrEndFrame", endFrame, PFN_xrEndFrame);
    RESOLVE("xrCreateReferenceSpace", createReferenceSpace, PFN_xrCreateReferenceSpace);
    RESOLVE("xrDestroySpace", destroySpace, PFN_xrDestroySpace);
    RESOLVE("xrLocateViews", locateViews, PFN_xrLocateViews);

#undef RESOLVE
    return true;
}

float WrapDeg(float degrees) noexcept
{
    while (degrees > 180.0f) degrees -= 360.0f;
    while (degrees < -180.0f) degrees += 360.0f;
    return degrees;
}

void HeadEulerDegrees(const XrQuaternionf& q, float& yawDeg, float& pitchDeg, float& rollDeg) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float fx = -2.0f * (x * z + w * y);
    const float fy = -2.0f * (y * z - w * x);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float ux = 2.0f * (x * y - w * z);
    const float uy = 1.0f - 2.0f * (x * x + z * z);
    constexpr float kRad2Deg = 57.2957795f;
    const float clampedFy = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    yawDeg = std::atan2(fx, -fz) * kRad2Deg;
    pitchDeg = std::asin(clampedFy) * kRad2Deg;
    rollDeg = std::atan2(-ux, uy) * kRad2Deg;
}

// Locate both eye views; fill out per-eye pose+fov and the averaged head pose.
bool LocateEyeViews(XrTime displayTime, XrView outViews[2]) noexcept
{
    if (g_localSpace == nullptr || outViews == nullptr)
    {
        return false;
    }

    XrViewLocateInfo locateInfo = {};
    locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
    locateInfo.displayTime = displayTime;
    locateInfo.space = g_localSpace;

    XrViewState viewState = {};
    viewState.type = XR_TYPE_VIEW_STATE_VALUE;
    outViews[0].type = XR_TYPE_VIEW_VALUE;
    outViews[1].type = XR_TYPE_VIEW_VALUE;
    uint32_t viewCount = 0;

    const XrResult r = g_fn.locateViews(g_session, &locateInfo, &viewState, 2, &viewCount, outViews);
    if (!XrSucceeded(r) || viewCount < 2 ||
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE) == 0)
    {
        static std::atomic<int> s_logs{0};
        if (s_logs.fetch_add(1, std::memory_order_relaxed) < 5)
        {
            ME1VR::Log::Line("[OL2XR] xrLocateViews has no valid head orientation: " + FmtXr(r));
        }
        g_lastHeadPoseValid = false;
        return false;
    }

    g_lastHeadPose = outViews[0].pose;
    g_lastHeadPose.position.x = (outViews[0].pose.position.x + outViews[1].pose.position.x) * 0.5f;
    g_lastHeadPose.position.y = (outViews[0].pose.position.y + outViews[1].pose.position.y) * 0.5f;
    g_lastHeadPose.position.z = (outViews[0].pose.position.z + outViews[1].pose.position.z) * 0.5f;
    g_lastHeadPoseValid = true;
    return true;
}

void DriveCameraFromHead(const XrView& centerView) noexcept
{
    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    HeadEulerDegrees(centerView.pose.orientation, yawDeg, pitchDeg, rollDeg);

    if (!g_headRefValid)
    {
        g_headRefYawDeg = yawDeg;
        g_headRefPitchDeg = pitchDeg;
        g_headRefValid = true;
        ME1VR::Log::Line("[OL2XR] head reference latched yaw=" +
                         std::to_string(static_cast<int>(yawDeg)) +
                         " pitch=" + std::to_string(static_cast<int>(pitchDeg)));
    }

    constexpr float kDegToUU = 65536.0f / 360.0f;
    const float deltaYawDeg = WrapDeg(yawDeg - g_headRefYawDeg);
    const float deltaPitchDeg = pitchDeg - g_headRefPitchDeg;
    const int32_t headYawUU = static_cast<int32_t>(-deltaYawDeg * kDegToUU);
    const int32_t headPitchUU = static_cast<int32_t>(deltaPitchDeg * kDegToUU);

    ME1VR::RenderViewHook::SetHeadLook(headYawUU, headPitchUU, true);
    const auto n = g_cameraDriveFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n == 600)
    {
        ME1VR::Log::Line("[OL2XR] render-view free head-look active frames=" + std::to_string(n) +
                         " yawDeltaDeg=" + std::to_string(deltaYawDeg) +
                         " pitchDeltaDeg=" + std::to_string(deltaPitchDeg));
    }
}

bool ChooseSwapchainFormat(int64_t* outFormat)
{
    if (outFormat == nullptr) return false;

    const int64_t desired = SrgbSwapchainFormat(g_gameFormat);
    uint32_t count = 0;
    XrResult r = g_fn.enumerateSwapchainFormats(g_session, 0, &count, nullptr);
    if (!XrSucceeded(r) || count == 0)
    {
        ME1VR::Log::Line("[OL2XR] xrEnumerateSwapchainFormats count failed: " + FmtXr(r));
        return false;
    }
    std::vector<int64_t> formats(count);
    r = g_fn.enumerateSwapchainFormats(g_session, count, &count, formats.data());
    if (!XrSucceeded(r))
    {
        ME1VR::Log::Line("[OL2XR] xrEnumerateSwapchainFormats list failed: " + FmtXr(r));
        return false;
    }
    for (int64_t fmt : formats) if (fmt == desired) { *outFormat = desired; return true; }
    for (int64_t fmt : formats) if (fmt == g_gameFormat) { *outFormat = g_gameFormat; return true; }

    ME1VR::Log::Line("[OL2XR] runtime does not support game-compatible format.");
    return false;
}

bool CreateEyeSwapchain(EyeSwapchain& eye, uint32_t width, uint32_t height, const char* tag)
{
    eye.width = width;
    eye.height = height;

    XrSwapchainCreateInfo info = {};
    info.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    info.format = g_swapchainFormat;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;

    XrResult r = g_fn.createSwapchain(g_session, &info, &eye.swapchain);
    ME1VR::Log::Line(std::string("[OL2XR] xrCreateSwapchain ") + tag + " " +
                     std::to_string(width) + "x" + std::to_string(height) + ": " + FmtXr(r));
    if (!XrSucceeded(r) || eye.swapchain == nullptr) return false;

    uint32_t imageCount = 0;
    r = g_fn.enumerateSwapchainImages(eye.swapchain, 0, &imageCount, nullptr);
    if (!XrSucceeded(r) || imageCount == 0)
    {
        ME1VR::Log::Line(std::string("[OL2XR] enumerate ") + tag + " image count failed: " + FmtXr(r));
        return false;
    }
    eye.images.resize(imageCount);
    for (auto& image : eye.images)
    {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
        image.next = nullptr;
        image.texture = nullptr;
    }
    r = g_fn.enumerateSwapchainImages(eye.swapchain, imageCount, &imageCount,
                                      reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
    ME1VR::Log::Line(std::string("[OL2XR] ") + tag + " images=" + std::to_string(imageCount) +
                     " result=" + FmtXr(r));
    return XrSucceeded(r);
}

bool CreateEyeSwapchains()
{
    if (!ChooseSwapchainFormat(&g_swapchainFormat)) return false;

    // Per-eye dimensions: ask the runtime for the recommended view-config size.
    uint32_t viewCount = 0;
    uint32_t eyeW = g_backbufferWidth;
    uint32_t eyeH = g_backbufferHeight;
    XrResult r = g_fn.enumerateViewConfigurationViews(
        g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE, 0, &viewCount, nullptr);
    if (XrSucceeded(r) && viewCount >= 2)
    {
        std::vector<XrViewConfigurationView> views(viewCount);
        for (auto& v : views) { v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE; v.next = nullptr; }
        r = g_fn.enumerateViewConfigurationViews(
            g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE,
            viewCount, &viewCount, views.data());
        if (XrSucceeded(r))
        {
            ME1VR::Log::Line("[OL2XR] runtime recommends per-eye " +
                             std::to_string(views[0].recommendedImageRectWidth) + "x" +
                             std::to_string(views[0].recommendedImageRectHeight) +
                             ", using the game backbuffer size instead");
        }
    }
    if (eyeW == 0 || eyeH == 0) { eyeW = g_backbufferWidth; eyeH = g_backbufferHeight; }

    // ME1 sized these to the runtime recommendation because its per-draw doubling rendered
    // straight into eye-sized targets. The source is the game's own backbuffer, so matching it
    // exactly turns the per-eye copy into a straight CopyResource with nothing cropped or scaled.
    eyeW = g_backbufferWidth;
    eyeH = g_backbufferHeight;
    ME1VR::Log::Line("[OL2XR] eye swapchains " + std::to_string(eyeW) + "x" + std::to_string(eyeH) +
                     " format=" + std::to_string(static_cast<long long>(g_swapchainFormat)) +
                     " (game backbuffer format=" + std::to_string(static_cast<long long>(g_gameFormat)) + ")");

    return CreateEyeSwapchain(g_eyes[0], eyeW, eyeH, "LEFT") &&
           CreateEyeSwapchain(g_eyes[1], eyeW, eyeH, "RIGHT");
}

bool CreateSessionAndSwapchains()
{
    XrSystemGetInfo sysInfo = {};
    sysInfo.type = XR_TYPE_SYSTEM_GET_INFO_VALUE;
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY_VALUE;
    XrResult r = g_fn.getSystem(g_instance, &sysInfo, &g_systemId);
    ME1VR::Log::Line("[OL2XR] xrGetSystem(HMD): " + FmtXr(r));
    if (!XrSucceeded(r)) return false;

    XrGraphicsRequirementsD3D11KHR req = {};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR_VALUE;
    r = g_fn.getD3D11GraphicsRequirements(g_instance, g_systemId, &req);
    ME1VR::Log::Line("[OL2XR] xrGetD3D11GraphicsRequirementsKHR: " + FmtXr(r));
    if (!XrSucceeded(r)) return false;

    XrGraphicsBindingD3D11KHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR_VALUE;
    binding.device = g_device;

    XrSessionCreateInfo sessionInfo = {};
    sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO_VALUE;
    sessionInfo.next = &binding;
    sessionInfo.systemId = g_systemId;
    r = g_fn.createSession(g_instance, &sessionInfo, &g_session);
    ME1VR::Log::Line("[OL2XR] xrCreateSession: " + FmtXr(r));
    if (!XrSucceeded(r) || g_session == nullptr) return false;

    XrReferenceSpaceCreateInfo spaceInfo = {};
    spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW_VALUE;
    spaceInfo.poseInReferenceSpace = IdentityPose();
    r = g_fn.createReferenceSpace(g_session, &spaceInfo, &g_viewSpace);
    if (!XrSucceeded(r) || g_viewSpace == nullptr)
    {
        ME1VR::Log::Line("[OL2XR] xrCreateReferenceSpace(VIEW) failed: " + FmtXr(r));
        return false;
    }

    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    spaceInfo.poseInReferenceSpace = IdentityPose();
    r = g_fn.createReferenceSpace(g_session, &spaceInfo, &g_localSpace);
    if (!XrSucceeded(r) || g_localSpace == nullptr)
    {
        ME1VR::Log::Line("[OL2XR] xrCreateReferenceSpace(LOCAL) failed: " + FmtXr(r));
        return false;
    }

    return CreateEyeSwapchains();
}

void PollEvents()
{
    if (g_instance == nullptr || g_fn.pollEvent == nullptr) return;

    for (;;)
    {
        XrEventDataBuffer event = {};
        event.type = XR_TYPE_EVENT_DATA_BUFFER_VALUE;
        const XrResult r = g_fn.pollEvent(g_instance, &event);
        if (r == XR_EVENT_UNAVAILABLE_VALUE) break;
        if (!XrSucceeded(r)) { ME1VR::Log::Line("[OL2XR] xrPollEvent failed: " + FmtXr(r)); break; }

        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED_VALUE)
        {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            g_sessionState = changed->state;
            ME1VR::Log::Line("[OL2XR] session state=" + std::to_string(g_sessionState));

            if (g_sessionState == XR_SESSION_STATE_READY_VALUE && !g_sessionRunning)
            {
                XrSessionBeginInfo begin = {};
                begin.type = XR_TYPE_SESSION_BEGIN_INFO_VALUE;
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
                const XrResult beginResult = g_fn.beginSession(g_session, &begin);
                ME1VR::Log::Line("[OL2XR] xrBeginSession: " + FmtXr(beginResult));
                g_sessionRunning = XrSucceeded(beginResult);
            }
            else if (g_sessionState == XR_SESSION_STATE_STOPPING_VALUE && g_sessionRunning)
            {
                const XrResult endResult = g_fn.endSession(g_session);
                ME1VR::Log::Line("[OL2XR] xrEndSession: " + FmtXr(endResult));
                g_sessionRunning = false;
            }
            else if (g_sessionState == XR_SESSION_STATE_EXITING_VALUE ||
                     g_sessionState == XR_SESSION_STATE_LOSS_PENDING_VALUE)
            {
                g_shutdown.store(true, std::memory_order_relaxed);
            }
        }
    }
}

// Copy a source D3D11 color texture into one eye's acquired swapchain image.
// Size-mismatch is handled with a region-clamped CopySubresourceRegion. Caller
// does NOT pass ownership of `source` (it is not released here).
//
// SCOPE NOTE: the per-eye SOURCE is chosen by RunFrame. When per-draw doubling
// is live and has produced eye color targets, each eye gets its own
// opposite-shear render (real parallax). Otherwise both eyes fall back to the
// game's mono backbuffer (the mono baseline behaviour: a correct mono image in a
// stereo projection layer, no parallax). In this - Hinny that GOAT - build per-draw doubling is stubbed off;
// the stereo source is the packed pair from ol2_sidecar.cpp when one is fresh.
// Two DXGI formats can be copied between when they share a typeless parent.
bool SameFormatFamily(DXGI_FORMAT a, DXGI_FORMAT b)
{
    if (a == b) return true;
    auto family = [](DXGI_FORMAT f) -> int {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return 1;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return 2;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return 3;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return 4;
        default: return 0;
        }
    };
    const int fa = family(a), fb = family(b);
    return fa != 0 && fa == fb;
}

bool CopySourceIntoEye(ID3D11Texture2D* source, EyeSwapchain& eye)
{
    if (source == nullptr || g_context == nullptr || eye.swapchain == nullptr)
    {
        return false;
    }

    XrSwapchainImageAcquireInfo acquire = {};
    acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    uint32_t imageIndex = 0;
    XrResult r = g_fn.acquireSwapchainImage(eye.swapchain, &acquire, &imageIndex);
    if (!XrSucceeded(r) || imageIndex >= eye.images.size())
    {
        return false;
    }

    XrSwapchainImageWaitInfo wait = {};
    wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wait.timeout = XR_INFINITE_DURATION_VALUE;
    r = g_fn.waitSwapchainImage(eye.swapchain, &wait);
    if (XrSucceeded(r))
    {
        ID3D11Texture2D* dst = eye.images[imageIndex].texture;
        if (dst != nullptr)
        {
            D3D11_TEXTURE2D_DESC sd = {}, dd = {};
            source->GetDesc(&sd);
            dst->GetDesc(&dd);

            static std::atomic<int> s_described{0};
            if (s_described.fetch_add(1, std::memory_order_relaxed) < 2)
            {
                ME1VR::Log::Line("[OL2COPY] src " + std::to_string(sd.Width) + "x" + std::to_string(sd.Height) +
                                 " fmt=" + std::to_string(static_cast<int>(sd.Format)) +
                                 "  dst " + std::to_string(dd.Width) + "x" + std::to_string(dd.Height) +
                                 " fmt=" + std::to_string(static_cast<int>(dd.Format)));
            }

            // D3D11 permits a copy between two formats in the SAME typeless family, and
            // R8G8B8A8_UNORM / _UNORM_SRGB are one family. The original guard demanded an exact
            // match, so an sRGB eye swapchain meant the copy was silently skipped while the
            // function still reported success: black in the headset, okL=1 okR=1 in the log.
            const bool copyable = SameFormatFamily(sd.Format, dd.Format);
            if (!copyable)
            {
                static std::atomic<int> s_warned{0};
                if (s_warned.fetch_add(1, std::memory_order_relaxed) < 1)
                {
                    ME1VR::Log::Line("[OL2COPY] INCOMPATIBLE formats src=" + std::to_string(static_cast<int>(sd.Format)) +
                                     " dst=" + std::to_string(static_cast<int>(dd.Format)) + " - nothing will be shown");
                }
            }
            else if (sd.Width == dd.Width && sd.Height == dd.Height)
            {
                g_context->CopyResource(dst, source);
            }
            else
            {
                const UINT w = sd.Width < dd.Width ? sd.Width : dd.Width;
                const UINT h = sd.Height < dd.Height ? sd.Height : dd.Height;
                D3D11_BOX box = {0, 0, 0, w, h, 1};
                g_context->CopySubresourceRegion(dst, 0, 0, 0, 0, source, 0, &box);
            }
        }
    }

    XrSwapchainImageReleaseInfo release = {};
    release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(eye.swapchain, &release);
    return true;
}

// Fetch the game backbuffer (caller releases). Mono fallback source.
ID3D11Texture2D* AcquireBackbuffer(IDXGISwapChain* gameSwapChain)
{
    if (gameSwapChain == nullptr)
    {
        return nullptr;
    }
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                        reinterpret_cast<void**>(&backBuffer))))
    {
        return nullptr;
    }
    return backBuffer;
}

// ---------------------------------------------------------------------------
// Alternate-eye rendering (historical; AER is not used by the active path)
//
// The game renders ONE view per frame. AER biases its ViewProjection by one eye's half-IPD, keep
// the result in that eye's cache texture, and alternate which eye is fresh. Both caches are
// submitted every frame, so the compositor always has a full stereo pair; one of them is a frame
// old. Each cache carries the pose and FOV it was RENDERED with, and that is what gets submitted
// for it. Pose tagging is not optional: submitting a stale image under a fresh pose is what makes
// AER read as smearing rather than depth.
// ---------------------------------------------------------------------------

ID3D11Texture2D* g_eyeCache[2] = {nullptr, nullptr};
XrPosef g_eyeCachePose[2] = {};
XrFovf g_eyeCacheFov[2] = {};
XrFovf g_lastRuntimeFov = {};
bool g_eyeCacheValid[2] = {false, false};
std::atomic<int> g_nextEye{0};
IDXGISwapChain* g_gameSwapChain = nullptr;
std::atomic<bool> g_leftBanked{false};

// How much of each eye texture actually holds picture. With the split each eye is half the
// backbuffer width, so the compositor must be told to sample only that region or it stretches
// the whole texture across the eye.
UINT g_eyeContentWidth = 0;
UINT g_eyeContentHeight = 0;
std::atomic<unsigned long long> g_aerFrames{0};

// Lazily mirror the game backbuffer's format and size. Cache textures are ours, never the
// runtime's, so a stale eye survives to the next submit.
bool EnsureEyeCache(ID3D11Texture2D* backBuffer)
{
    if (backBuffer == nullptr || g_device == nullptr) return false;
    if (g_eyeCache[0] != nullptr && g_eyeCache[1] != nullptr) return true;

    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    for (int eye = 0; eye < 2; ++eye)
    {
        if (g_eyeCache[eye] != nullptr) continue;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_eyeCache[eye])))
        {
            ME1VR::Log::Line("[OL2XR] eye cache texture creation FAILED");
            return false;
        }
    }

    ME1VR::Log::Line("[OL2XR] eye caches created " + std::to_string(desc.Width) + "x" +
                     std::to_string(desc.Height) + " fmt=" + std::to_string(static_cast<int>(desc.Format)));
    return true;
}

void RunFrame(IDXGISwapChain* gameSwapChain)
{
    XrFrameWaitInfo waitInfo = {};
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO_VALUE;
    XrFrameState frameState = {};
    frameState.type = XR_TYPE_FRAME_STATE_VALUE;

    XrResult r = g_fn.waitFrame(g_session, &waitInfo, &frameState);
    if (!XrSucceeded(r)) { ME1VR::Log::Line("[OL2XR] xrWaitFrame failed: " + FmtXr(r)); return; }

    XrFrameBeginInfo beginInfo = {};
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO_VALUE;
    r = g_fn.beginFrame(g_session, &beginInfo);
    if (!XrSucceeded(r)) { ME1VR::Log::Line("[OL2XR] xrBeginFrame failed: " + FmtXr(r)); return; }

    const XrCompositionLayerBaseHeader* layers[1] = {};
    XrCompositionLayerProjection projection = {};
    XrCompositionLayerProjectionView projViews[2] = {};

    if (frameState.shouldRender)
    {
        XrView views[2] = {};
        const bool located = LocateEyeViews(frameState.predictedDisplayTime, views);
        if (located)
        {
            DriveCameraFromHead(views[0]);
        }
        else
        {
            ME1VR::RenderViewHook::SetHeadLook(0, 0, false);
        }

        ID3D11Texture2D* stereoFrame = nullptr;
        unsigned stereoWidth = 0, stereoHeight = 0;
        unsigned long long stereoSerial = 0;
        const bool sameFrame = Ol2Camera::AcquireStereoFrame(
            &stereoFrame, &stereoWidth, &stereoHeight, &stereoSerial);
        ID3D11Texture2D* backBuffer = AcquireBackbuffer(gameSwapChain);
        bool okL = false;
        bool okR = false;

        if (backBuffer != nullptr && EnsureEyeCache(backBuffer) && g_context != nullptr)
        {
            if (sameFrame)
            {
                // PRE-COMPOSITOR SFR: the engine rendered both eye viewports into one offscreen
                // scene-color target this frame. It was captured before Outlast's broken final
                // compositor and shader-converted into the R8 backbuffer family. Take its halves.
                D3D11_TEXTURE2D_DESC cache = {};
                g_eyeCache[0]->GetDesc(&cache);
                const UINT halfW = stereoWidth / 2;
                const UINT copyH = stereoHeight < cache.Height ? stereoHeight : cache.Height;
                const UINT copyW = halfW < cache.Width ? halfW : cache.Width;
                g_eyeContentWidth = copyW;
                g_eyeContentHeight = copyH;

                D3D11_BOX leftBox = {0, 0, 0, copyW, copyH, 1};
                D3D11_BOX rightBox = {halfW, 0, 0, halfW + copyW, copyH, 1};
                g_context->CopySubresourceRegion(g_eyeCache[0], 0, 0, 0, 0,
                                                 stereoFrame, 0, &leftBox);
                g_context->CopySubresourceRegion(g_eyeCache[1], 0, 0, 0, 0,
                                                 stereoFrame, 0, &rightBox);

                if (located)
                {
                    g_eyeCachePose[0] = views[0].pose;
                    g_eyeCacheFov[0] = views[0].fov;
                    g_eyeCachePose[1] = views[1].pose;
                    g_eyeCacheFov[1] = views[1].fov;
                }
                g_eyeCacheValid[0] = true;
                g_eyeCacheValid[1] = true;
            }
            else
            {
                // A stereo frame may be absent during loading or immediately after F1. Clear the
                // half-width sub-image state before returning to the complete mono backbuffer.
                g_eyeContentWidth = 0;
                g_eyeContentHeight = 0;

                // Stable mono VR baseline: refresh BOTH eyes from the same complete game frame.
                // The previous alternate-eye path held one eye for a frame, making TAA, exposure,
                // rain, and other temporal effects pulse in both eyes. Stereo depth waits until a
                // correct same-frame right-eye render exists; image stability comes first.
                g_context->CopyResource(g_eyeCache[0], backBuffer);
                g_context->CopyResource(g_eyeCache[1], backBuffer);
                if (located)
                {
                    g_eyeCachePose[0] = views[0].pose;
                    g_eyeCacheFov[0] = views[0].fov;
                    g_eyeCachePose[1] = views[1].pose;
                    g_eyeCacheFov[1] = views[1].fov;
                }
                g_eyeCacheValid[0] = true;
                g_eyeCacheValid[1] = true;
                g_nextEye.store(0, std::memory_order_relaxed);
            }

            okL = CopySourceIntoEye(g_eyeCache[0], g_eyes[0]);
            okR = CopySourceIntoEye(g_eyeCache[1], g_eyes[1]);
        }

        SafeRelease(stereoFrame);
        SafeRelease(backBuffer);

        // SUBMIT FOV == RENDERED FOV, exactly. The image carries the GAME's frustum, not the
        // headset's, so declaring the runtime's per-eye FOV is what stretched the picture on the
        // first VR run. This rule carries over from the AER baseline and it applies to
        // same-frame identically.
        if (located) g_lastRuntimeFov = views[0].fov;

        float gameFovX = 0.0f, gameFovY = 0.0f;
        Ol2Ue3::GetEyeFov(&gameFovX, &gameFovY);
        // Stable mono's main camera projection is widened in ol2_camera to the runtime eye FOV. Declare
        // exactly that widened render here; otherwise the correct image occupies only a panel.
        if (!sameFrame)
        {
            float runtimeH = 0.0f, runtimeV = 0.0f;
            if (Ol2Xr::RuntimeEyeFov(&runtimeH, &runtimeV))
            {
                gameFovX = runtimeH;
                gameFovY = runtimeV;
            }
        }
        if (gameFovX > 0.1f && gameFovY > 0.1f)
        {
            for (int i = 0; i < 2; ++i)
            {
                g_eyeCacheFov[i].angleLeft = -gameFovX * 0.5f;
                g_eyeCacheFov[i].angleRight = gameFovX * 0.5f;
                g_eyeCacheFov[i].angleDown = -gameFovY * 0.5f;
                g_eyeCacheFov[i].angleUp = gameFovY * 0.5f;
            }
        }

        Ol2Ue3::FrameTick();
        const unsigned long long n = g_aerFrames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n % 3000) == 0)
        {
            ME1VR::Log::Line(std::string("[OL2EYES] frame ") + std::to_string(n) +
                             (sameFrame ? " PRECOMPOSITOR-SFR serial=" + std::to_string(stereoSerial)
                                        : " stable-mono") +
                             " okL=" + std::to_string(okL ? 1 : 0) +
                             " okR=" + std::to_string(okR ? 1 : 0) +
                             " gameFov=" + std::to_string(int(gameFovX * 57.2957795f)) + "x" +
                             std::to_string(int(gameFovY * 57.2957795f)));
        }

        if (located && okL && okR)
        {
            for (int i = 0; i < 2; ++i)
            {
                projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                // POSE-TAGGED: the pose this eye's image was actually rendered under.
                projViews[i].pose = g_eyeCachePose[i];
                projViews[i].fov = g_eyeCacheFov[i];
                projViews[i].subImage.swapchain = g_eyes[i].swapchain;
                projViews[i].subImage.imageRect.offset = {0, 0};
                const UINT contentW = g_eyeContentWidth ? g_eyeContentWidth : g_eyes[i].width;
                const UINT contentH = g_eyeContentHeight ? g_eyeContentHeight : g_eyes[i].height;
                projViews[i].subImage.imageRect.extent =
                    {static_cast<int32_t>(contentW), static_cast<int32_t>(contentH)};
                projViews[i].subImage.imageArrayIndex = 0;
            }
            projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            projection.space = g_localSpace;
            projection.viewCount = 2;
            projection.views = projViews;
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        }
    }

    XrFrameEndInfo endInfo = {};
    endInfo.type = XR_TYPE_FRAME_END_INFO_VALUE;
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    endInfo.layerCount = layers[0] != nullptr ? 1 : 0;
    endInfo.layers = layers[0] != nullptr ? layers : nullptr;

    r = g_fn.endFrame(g_session, &endInfo);
    if (!XrSucceeded(r))
    {
        ME1VR::Log::Line("[OL2XR] xrEndFrame failed: " + FmtXr(r));
    }
    else
    {
        const auto n = g_frameCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1)
        {
            ME1VR::Log::Line("[OL2XR] first stereo projection-layer frame submitted (eyes share content this increment).");
        }
        else if (n == 600)
        {
            ME1VR::Log::Line("[OL2XR] 600 stereo projection-layer frames submitted. seenVS=" +
                             std::to_string(ME1VR::VsStereo::TotalShadersSeen()) +
                             " worldVS=" + std::to_string(ME1VR::VsStereo::WorldShadersClassified()) +
                             " sheared=" + std::to_string(ME1VR::VsStereo::ShearedVariantsCompiled()) +
                             " | draws=" + std::to_string(ME1VR::DrawDouble::DrawsSeen()) +
                             " doubled=" + std::to_string(ME1VR::DrawDouble::WorldDrawsDoubled()) +
                             " dblFail=" + std::to_string(ME1VR::DrawDouble::DoublingFailures()));
        }
    }
}

DWORD WINAPI SessionThread(LPVOID) noexcept
{
    if (!CreateInstance() || !ResolveSessionFunctions() || !CreateSessionAndSwapchains())
    {
        ME1VR::Log::Line("[OL2XR] bring-up failed.");
        return 0;
    }
    g_ready.store(true, std::memory_order_release);

    // Session is up: turn on per-draw doubling so world draws now render the
    // second eye with opposite clip-shear. If the hooks failed to install this
    // is a harmless no-op and both eyes stay mono.
    ME1VR::DrawDouble::SetEnabled(true);
    ME1VR::Log::Line("[OL2XR] stereo OpenXR session ready (two eye swapchains + projection layer; per-draw doubling " +
                     std::string(ME1VR::DrawDouble::Installed() ? "ENABLED" : "NOT INSTALLED") + ").");
    return 0;
}
}  // namespace

namespace Ol2Xr
{
int NextEye() noexcept { return g_nextEye.load(std::memory_order_relaxed) & 1; }
bool SessionReady() noexcept { return g_ready.load(std::memory_order_relaxed); }

void TryCreateSession(ID3D11Device* device, int64_t colorFormat, uint32_t width, uint32_t height) noexcept
{
    if (device == nullptr || width == 0 || height == 0) return;

    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    g_device = device;
    g_device->AddRef();
    g_device->GetImmediateContext(&g_context);
    g_gameFormat = colorFormat;
    g_backbufferWidth = width;
    g_backbufferHeight = height;

    // Install the VS hook so world shaders are cached/classified for the shear
    // path as soon as the device is known.
    ME1VR::VsStereo::Install(device);

    // Install the per-draw doubling hooks on the immediate context. They stay
    // DISABLED (pure pass-through) until the OpenXR session is ready, so the
    // game runs untouched during bring-up.
    ME1VR::DrawDouble::Install(device, g_context);

    ME1VR::Log::Line("[OL2XR] starting OpenXR stereo session, backbuffer=" +
                     std::to_string(width) + "x" + std::to_string(height) +
                     " fmt=" + std::to_string(colorFormat));

    HANDLE thread = CreateThread(nullptr, 0, SessionThread, nullptr, 0, nullptr);
    if (thread != nullptr) CloseHandle(thread);
    else ME1VR::Log::WindowsError("[OL2XR] CreateThread failed", GetLastError());
}

void SetGameSwapChain(IDXGISwapChain* swapChain) noexcept { g_gameSwapChain = swapChain; }

bool RuntimeEyeFov(float* horizontalRadians, float* verticalRadians) noexcept
{
    if (horizontalRadians == nullptr || verticalRadians == nullptr || !g_lastHeadPoseValid) return false;
    const float h = g_lastRuntimeFov.angleRight - g_lastRuntimeFov.angleLeft;
    const float v = g_lastRuntimeFov.angleUp - g_lastRuntimeFov.angleDown;
    if (h < 0.3f || h > 3.0f || v < 0.3f || v > 3.0f) return false;
    *horizontalRadians = h;
    *verticalRadians = v;
    return true;
}

bool RuntimeEyeFovH(float* radians) noexcept
{
    float vertical = 0.0f;
    return RuntimeEyeFov(radians, &vertical);
}

bool CaptureLeftEye() noexcept
{
    if (g_gameSwapChain == nullptr || g_context == nullptr || g_device == nullptr) return false;

    ID3D11Texture2D* backBuffer = AcquireBackbuffer(g_gameSwapChain);
    if (backBuffer == nullptr) return false;

    bool ok = false;
    if (EnsureEyeCache(backBuffer))
    {
        g_context->CopyResource(g_eyeCache[0], backBuffer);
        g_leftBanked.store(true, std::memory_order_relaxed);
        ok = true;
    }
    SafeRelease(backBuffer);
    return ok;
}

void OnPresent(IDXGISwapChain* gameSwapChain) noexcept
{
    if (!g_ready.load(std::memory_order_acquire) || g_shutdown.load(std::memory_order_relaxed)) return;
    PollEvents();
    if (g_sessionRunning) RunFrame(gameSwapChain);
}

void RequestShutdown() noexcept
{
    g_shutdown.store(true, std::memory_order_relaxed);
}
}  // namespace Ol2Xr
