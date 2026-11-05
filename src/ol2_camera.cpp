// Outlast 2 VR - camera layer.
//
// Finds the scene ViewProjection matrix as it is uploaded to the GPU, then verifies the mod controls it.
//
// The VP signature is lifted from the Arkham Knight VR proxy, where it is proven on a UE3
// x64 D3D11 title, and it matches on CONTENT rather than on a hardcoded slot or offset. Arkham
// Knight and BioShock Infinite both carry the VP at VS register b1 byte 0, so that is the
// expectation here, but this probe does not assume it: it scans every 16-byte register boundary
// of every VS-bound constant buffer and REPORTS which slot and offset actually won.
//
// ---------------------------------------------------------------------------
// Why this file hooks devices and not just the swapchain's context
//
// An early build hooked the immediate context of the swapchain's device and
// measured, over 3000 presented frames: vsCbs=0, drawIndexed=0, drawInstanced=0, draw=1. A
// renderer cannot produce frames without setting VS constants or issuing draws, so the game is
// simply not drawing through that context object. The signature was never the problem; it never
// ran once.
//
// Two things can produce that, and this build covers both rather than picking one:
//   1. More than one D3D11 device exists and the hook landed on the wrong one. So D3D11CreateDevice and
//      D3D11CreateDeviceAndSwapChain are inline-hooked, and EVERY device gets hooked at birth.
//   2. The game renders on a deferred context. Deferred and immediate contexts are different
//      implementation classes with DIFFERENT vtables, so patching the immediate one misses them
//      entirely. CreateDeferredContext is hooked on every device, and every context ever seen
//      is patched, keyed by its own vtable with its own set of original pointers.
//
// It also re-checks every frame that the patches are still installed, so "something overwrote the hook"
// is distinguishable from "wrong object" instead of being guessed at.
//
// The device was created D3D11_CREATE_DEVICE_SINGLETHREADED (measured), so it has no internal
// locking. Everything here runs on the thread the game itself calls from.
//
// Hotkeys:
//   F9  dump the live VP and derived FOV once
//   F10 ownership proof: oscillate the camera sideways. The world must visibly sway.
//   F11 static half-IPD sideways offset, the shape the stereo path will use
//
// A constant offset is INVISIBLE on success, which is why the proof is a sway and not a shift.

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <mutex>
#include <unordered_map>

#include "MinHook.h"
#include "ol2_common.h"
#include "ol2_sidecar.h"
#include "ol2_ue3.h"
#include "ol2_xr.h"

namespace
{
constexpr UINT kVpBytes = 64;         // 4 rows x float4
constexpr float kRadToDeg = 57.2957795f;

// Backbuffer width, so a viewport can be classified as the left or right half of the split.
unsigned g_backbufferWidthForViewport = 0;
unsigned g_backbufferHeightForViewport = 0;

// ---------------------------------------------------------------------------
// UE3 view-projection identification
//
// Row-vector convention: clip = worldPos * M, so
//   column 0 of the 3x3 part = viewRight   * P00
//   column 1                 = viewUp      * P11
//   column 3 (w)             = viewForward           (w_clip == z_view, so unit length)
//   m[15] == 0                                       (perspective, not ortho)
// Together those reject LocalToWorld, shadow and UI ortho matrices for free.
// ---------------------------------------------------------------------------

struct VpFacts
{
    float p00 = 0.0f;             // 1 / tan(hFov/2)
    float p11 = 0.0f;             // 1 / tan(vFov/2)
    float fwd[3] = {0, 0, 0};
    float right[3] = {0, 0, 0};
    float up[3] = {0, 0, 0};
};

bool Finite16(const float* m)
{
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

bool LooksLikeViewProjection(const float* m, VpFacts& out)
{
    if (!Finite16(m)) return false;
    if (std::fabs(m[15]) > 0.001f) return false;

    const float fx = m[3], fy = m[7], fz = m[11];
    const float fwdLen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fwdLen < 0.95f || fwdLen > 1.05f) return false;

    const float rx = m[0], ry = m[4], rz = m[8];
    const float p00 = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (p00 < 0.4f || p00 > 5.0f) return false;           // roughly 22 to 135 degrees horizontal

    const float ux = m[1], uy = m[5], uz = m[9];
    const float p11 = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (p11 < 0.4f || p11 > 6.0f) return false;

    const float dotRF = (rx * fx + ry * fy + rz * fz) / p00;
    const float dotUF = (ux * fx + uy * fy + uz * fz) / p11;
    const float dotRU = (rx * ux + ry * uy + rz * uz) / (p00 * p11);
    if (std::fabs(dotRF) > 0.03f || std::fabs(dotUF) > 0.03f || std::fabs(dotRU) > 0.03f) return false;

    out.p00 = p00;
    out.p11 = p11;
    out.fwd[0] = fx;         out.fwd[1] = fy;         out.fwd[2] = fz;
    out.right[0] = rx / p00; out.right[1] = ry / p00; out.right[2] = rz / p00;
    out.up[0] = ux / p11;    out.up[1] = uy / p11;    out.up[2] = uz / p11;
    return true;
}

// row3' = row3 - d.x*row0 - d.y*row1 - d.z*row2   (exact world-space camera translation)
void TranslateCamera(float* m, const float d[3])
{
    for (int c = 0; c < 4; ++c)
        m[12 + c] -= d[0] * m[0 + c] + d[1] * m[4 + c] + d[2] * m[8 + c];
}

// ---------------------------------------------------------------------------
// head tracking
//
// Outlast 2 has no CalcSceneView hook to write a head rotation into, so the headset orientation
// is applied to the ViewProjection itself.
//
// clip = worldRel * M. Rotating the camera by R means expressing the world in the rotated frame,
// clip = (worldRel * R) * M = worldRel * (R * M), so the fix is a premultiply of M's upper 3x3
// rows by R. Doing it to ALL FOUR columns keeps the depth column consistent with the rotated
// basis, which rebuilding right/up/forward by hand would not.
// ---------------------------------------------------------------------------

volatile LONG g_headValid = 0;
float g_headYawRad = 0.0f;
float g_headPitchRad = 0.0f;
float g_headYawSign = -1.0f;      // flipped with F7 if the world turns the wrong way
bool g_headEnabled = true;

// UE3 world frame: Z up, X forward, Y right.
void PremultiplyRotation(float* m, float yawRad, float pitchRad)
{
    const float cy = std::cos(yawRad), sy = std::sin(yawRad);
    const float cp = std::cos(pitchRad), sp = std::sin(pitchRad);

    // Yaw about Z, then pitch about Y, both in the world frame.
    const float rz[3][3] = {{ cy, -sy, 0.0f}, { sy,  cy, 0.0f}, {0.0f, 0.0f, 1.0f}};
    const float ry[3][3] = {{ cp, 0.0f,  sp}, {0.0f, 1.0f, 0.0f}, { -sp, 0.0f,  cp}};

    float r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i][j] = ry[i][0] * rz[0][j] + ry[i][1] * rz[1][j] + ry[i][2] * rz[2][j];

    float out[12];
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 4; ++c)
            out[i * 4 + c] = r[i][0] * m[0 + c] + r[i][1] * m[4 + c] + r[i][2] * m[8 + c];

    for (int i = 0; i < 12; ++i) m[i] = out[i];
}

// ---------------------------------------------------------------------------
// per-vtable original pointers
//
// Immediate and deferred contexts are different classes. Patching both means two sets of
// originals, and a hook must call the one belonging to the vtable it was reached through.
// ---------------------------------------------------------------------------

using VSSetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndexedInstancedIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DrawInstancedIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PSSetShaderResourcesFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);

// Originals are MinHook trampolines now, not vtable slots. See InlineHookContext below for
// why: this game's context vtable lives in the object's own heap allocation and gets rebuilt,
// so a patched slot does not stay patched. Inline hooks sit on the function itself and are
// reached no matter which vtable, wrapper or proxy the call arrives through.
VSSetConstantBuffersFn g_originalVSSetConstantBuffers = nullptr;
MapFn g_originalMap = nullptr;
UnmapFn g_originalUnmap = nullptr;
UpdateSubresourceFn g_originalUpdateSubresource = nullptr;
DrawFn g_originalDraw = nullptr;
DrawIndexedFn g_originalDrawIndexed = nullptr;
DrawIndexedInstancedFn g_originalDrawIndexedInstanced = nullptr;
DrawInstancedFn g_originalDrawInstanced = nullptr;
DrawAutoFn g_originalDrawAuto = nullptr;
DrawIndexedInstancedIndirectFn g_originalDrawIndexedInstancedIndirect = nullptr;
DrawInstancedIndirectFn g_originalDrawInstancedIndirect = nullptr;
GetDataFn g_originalGetData = nullptr;
RSSetViewportsFn g_originalRSSetViewports = nullptr;
OMSetRenderTargetsFn g_originalOMSetRenderTargets = nullptr;
PSSetShaderResourcesFn g_originalPSSetShaderResources = nullptr;
PSSetShaderFn g_originalPSSetShader = nullptr;

// ---------------------------------------------------------------------------
// Which half is the engine actually rendering into?
//
// The split builds two views and both are genuinely in the family, yet only one half of the
// backbuffer gets picture. Rather than keep guessing at why, count the draw calls that land under
// a left-half viewport versus a right-half one. That is a number, it needs no headset, and it
// separates "the engine never renders the second view" from "it renders it somewhere this code
// is not looking".
volatile long g_vpLeftDraws = 0, g_vpRightDraws = 0, g_vpFullDraws = 0, g_vpOtherDraws = 0;
volatile long g_currentHalf = -1;     // 0 = left, 1 = right, -1 = full/other
volatile long g_vpSetCalls = 0;
volatile unsigned g_currentVpX = 0, g_currentVpY = 0, g_currentVpW = 0, g_currentVpH = 0;

// Passive final-compositor trace. IDs are COM IUnknown identities, so interface pointer aliases do
// not make one texture look like several. No GPU state is changed by this instrumentation.
volatile uintptr_t g_boundTargetId = 0;
volatile unsigned g_boundTargetW = 0, g_boundTargetH = 0, g_boundTargetFmt = 0;
ID3D11Texture2D* g_boundColorTexture = nullptr;
volatile long g_boundSawLeft = 0, g_boundSawRight = 0;
volatile uintptr_t g_backbufferId = 0;
volatile long g_eyeDrawsToBackbufferL = 0, g_eyeDrawsToBackbufferR = 0;
volatile long g_eyeDrawsOffscreenL = 0, g_eyeDrawsOffscreenR = 0;
volatile long g_targetBinds = 0;
ID3D11PixelShader* g_currentPixelShader = nullptr;
ID3D11ShaderResourceView* g_psSrvViews[16] = {};
ID3D11ShaderResourceView* g_finalColorSrv = nullptr;
volatile long g_finalColorCaptures = 0;
volatile long g_bypassEnabled = 1;
volatile long g_inBypass = 0;
volatile long g_bypassFrames = 0;

ID3D11VertexShader* g_bypassVs = nullptr;
ID3D11PixelShader* g_bypassPs = nullptr;
ID3D11SamplerState* g_bypassSampler = nullptr;
ID3D11RasterizerState* g_bypassRaster = nullptr;
ID3D11BlendState* g_bypassBlend = nullptr;
ID3D11DepthStencilState* g_bypassDepth = nullptr;

// Outlast renders both split views into a full-size R10 scene-color texture, then its final
// compositor loses/crops the right half. Preserve that texture when it is unbound. At Present,
// convert it to the backbuffer's R8 family with a shader so OpenXR can consume it legally.
ID3D11Texture2D* g_stereoPackedSnapshot = nullptr;
ID3D11ShaderResourceView* g_stereoPackedSnapshotSrv = nullptr;
unsigned g_stereoPackedWidth = 0, g_stereoPackedHeight = 0;
DXGI_FORMAT g_stereoPackedFormat = DXGI_FORMAT_UNKNOWN;
volatile long g_stereoPackedFresh = 0;
volatile long long g_stereoPackedSerial = 0;
ID3D11Texture2D* g_stereoConverted = nullptr;
ID3D11RenderTargetView* g_stereoConvertedRtv = nullptr;
unsigned g_stereoConvertedWidth = 0, g_stereoConvertedHeight = 0;
volatile long g_stereoConvertedFresh = 0;
volatile long long g_stereoConvertedSerial = 0;

ID3D11Texture2D* g_presentSnapshot = nullptr;
ID3D11ShaderResourceView* g_presentSnapshotSrv = nullptr;
unsigned g_presentSnapshotWidth = 0;
unsigned g_presentSnapshotHeight = 0;
DXGI_FORMAT g_presentSnapshotFormat = DXGI_FORMAT_UNKNOWN;

struct SrvTrace
{
    uintptr_t id;
    unsigned width;
    unsigned height;
    unsigned format;
};
SrvTrace g_psSrvs[16] = {};

// Temporary correctness baseline: the engine's synthetic right view has never produced a live
// picture.  Until the independent right-eye render path is replaced, copy the known-live left
// half into the right half at Present.  This is deliberately labelled MONO fallback, not stereo.
ID3D11Texture2D* g_rightMirrorScratch = nullptr;
unsigned g_rightMirrorWidth = 0;
unsigned g_rightMirrorHeight = 0;
DXGI_FORMAT g_rightMirrorFormat = DXGI_FORMAT_UNKNOWN;
volatile long g_rightMirrorEnabled = 0;
volatile long g_rightMirrorFrames = 0;

// Two-primary same-frame capture. Pass one is preserved here after explicitly unbinding every OM
// target. Pass two remains in the same known-live left slot until Present copies it to the right.
IDXGISwapChain* g_doubleSwapChain = nullptr;
ID3D11Texture2D* g_doubleLeftScratch = nullptr;
unsigned g_doubleLeftWidth = 0;
unsigned g_doubleLeftHeight = 0;
DXGI_FORMAT g_doubleLeftFormat = DXGI_FORMAT_UNKNOWN;
volatile long g_doubleLeftFresh = 0;
volatile long g_doubleCaptures = 0;
volatile long g_doubleComposes = 0;

bool EnsureHalfScratch(ID3D11Texture2D* backbuffer, ID3D11Texture2D** texture,
                       unsigned* width, unsigned* height, DXGI_FORMAT* format,
                       const char* label)
{
    if (backbuffer == nullptr || texture == nullptr) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    backbuffer->GetDesc(&desc);
    const unsigned halfWidth = desc.Width / 2;
    if (*texture != nullptr && *width == halfWidth && *height == desc.Height && *format == desc.Format)
        return true;

    if (*texture != nullptr)
    {
        (*texture)->Release();
        *texture = nullptr;
    }

    ID3D11Device* device = nullptr;
    backbuffer->GetDevice(&device);
    if (device == nullptr) return false;

    D3D11_TEXTURE2D_DESC scratchDesc = desc;
    scratchDesc.Width = halfWidth;
    scratchDesc.MipLevels = 1;
    scratchDesc.ArraySize = 1;
    scratchDesc.Usage = D3D11_USAGE_DEFAULT;
    scratchDesc.BindFlags = 0;
    scratchDesc.CPUAccessFlags = 0;
    scratchDesc.MiscFlags = 0;
    scratchDesc.SampleDesc.Count = 1;
    scratchDesc.SampleDesc.Quality = 0;
    const HRESULT hr = device->CreateTexture2D(&scratchDesc, nullptr, texture);
    device->Release();
    if (FAILED(hr) || *texture == nullptr) return false;

    *width = halfWidth;
    *height = desc.Height;
    *format = desc.Format;
    Ol2Log("[OL2DOUBLE] %s scratch created %ux%u fmt=%u", label, halfWidth, desc.Height,
           static_cast<unsigned>(desc.Format));
    return true;
}

void SaveAndUnbindTargets(ID3D11DeviceContext* context,
                          ID3D11RenderTargetView** renderTargets,
                          ID3D11DepthStencilView** depthTarget)
{
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, depthTarget);
    context->OMSetRenderTargets(0, nullptr, nullptr);
}

void RestoreTargets(ID3D11DeviceContext* context,
                    ID3D11RenderTargetView** renderTargets,
                    ID3D11DepthStencilView* depthTarget)
{
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, depthTarget);
    for (unsigned i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (renderTargets[i] != nullptr) renderTargets[i]->Release();
    if (depthTarget != nullptr) depthTarget->Release();
}

std::mutex g_vpMutex;
std::unordered_map<unsigned long long, long> g_vpSeen;   // packed rect -> draws under it
std::unordered_map<unsigned long long, long> g_eyeTargetsSeen;
std::unordered_map<unsigned long long, long> g_compositesSeen;

bool ReadResourceTrace(ID3D11Resource* resource, SrvTrace* trace)
{
    if (resource == nullptr || trace == nullptr) return false;
    IUnknown* identity = nullptr;
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identity))) ||
        identity == nullptr) return false;
    trace->id = reinterpret_cast<uintptr_t>(identity);
    identity->Release();

    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) ||
        texture == nullptr) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    texture->Release();
    trace->width = desc.Width;
    trace->height = desc.Height;
    trace->format = static_cast<unsigned>(desc.Format);
    return true;
}

bool ReadViewTrace(ID3D11View* view, SrvTrace* trace)
{
    if (view == nullptr || trace == nullptr) return false;
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (resource == nullptr) return false;
    const bool ok = ReadResourceTrace(resource, trace);
    resource->Release();
    return ok;
}

void LogEyeTargetIfNew()
{
    const long half = InterlockedCompareExchange(&g_currentHalf, 0, 0);
    const uintptr_t target = g_boundTargetId;
    if ((half != 0 && half != 1) || target == 0) return;
    const unsigned long long key = static_cast<unsigned long long>(target) ^
                                   (static_cast<unsigned long long>(half + 1) << 61);
    bool isNew = false;
    {
        std::lock_guard<std::mutex> lock(g_vpMutex);
        if (g_eyeTargetsSeen.find(key) == g_eyeTargetsSeen.end() && g_eyeTargetsSeen.size() < 128)
        {
            g_eyeTargetsSeen[key] = 1;
            isNew = true;
        }
    }
    if (isNew)
        Ol2Log("[OL2RT] %s eye viewport targets id=0x%p %ux%u fmt=%u %s",
               half == 0 ? "LEFT" : "RIGHT", reinterpret_cast<void*>(target),
               g_boundTargetW, g_boundTargetH, g_boundTargetFmt,
               target == g_backbufferId ? "BACKBUFFER" : "offscreen");
}

void TrackRenderTarget(ID3D11RenderTargetView* view)
{
    SrvTrace trace = {};
    ID3D11Texture2D* texture = nullptr;
    if (view != nullptr)
    {
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        if (resource != nullptr)
        {
            resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
            resource->Release();
        }
    }

    if (view != nullptr && ReadViewTrace(view, &trace))
    {
        const bool sameTarget = trace.id != 0 && trace.id == g_boundTargetId;
        g_boundTargetId = trace.id;
        g_boundTargetW = trace.width;
        g_boundTargetH = trace.height;
        g_boundTargetFmt = trace.format;
        if (!sameTarget)
        {
            InterlockedExchange(&g_boundSawLeft, 0);
            InterlockedExchange(&g_boundSawRight, 0);
        }
    }
    else
    {
        g_boundTargetId = 0;
        g_boundTargetW = g_boundTargetH = g_boundTargetFmt = 0;
        InterlockedExchange(&g_boundSawLeft, 0);
        InterlockedExchange(&g_boundSawRight, 0);
    }

    if (g_boundColorTexture != nullptr) g_boundColorTexture->Release();
    g_boundColorTexture = texture;
    InterlockedIncrement(&g_targetBinds);
    LogEyeTargetIfNew();
}

bool IsPackedStereoCandidate(const SrvTrace& trace)
{
    const unsigned bbW = g_backbufferWidthForViewport;
    const unsigned bbH = g_backbufferHeightForViewport;
    if (bbW == 0 || bbH == 0 || trace.format != DXGI_FORMAT_R10G10B10A2_UNORM) return false;
    const unsigned dw = trace.width > bbW ? trace.width - bbW : bbW - trace.width;
    const unsigned dh = trace.height > bbH ? trace.height - bbH : bbH - trace.height;
    return dw <= 32 && dh <= 32;
}

bool EnsurePackedStereoSnapshot(ID3D11Texture2D* source)
{
    if (source == nullptr) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1) return false;
    if (g_stereoPackedSnapshot != nullptr && g_stereoPackedSnapshotSrv != nullptr &&
        g_stereoPackedWidth == desc.Width && g_stereoPackedHeight == desc.Height &&
        g_stereoPackedFormat == desc.Format) return true;

    if (g_stereoPackedSnapshotSrv != nullptr)
    {
        g_stereoPackedSnapshotSrv->Release();
        g_stereoPackedSnapshotSrv = nullptr;
    }
    if (g_stereoPackedSnapshot != nullptr)
    {
        g_stereoPackedSnapshot->Release();
        g_stereoPackedSnapshot = nullptr;
    }

    ID3D11Device* device = nullptr;
    source->GetDevice(&device);
    if (device == nullptr) return false;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_stereoPackedSnapshot);
    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(g_stereoPackedSnapshot, nullptr,
                                              &g_stereoPackedSnapshotSrv);
    device->Release();

    if (FAILED(hr) || g_stereoPackedSnapshot == nullptr || g_stereoPackedSnapshotSrv == nullptr)
    {
        if (g_stereoPackedSnapshotSrv != nullptr)
        {
            g_stereoPackedSnapshotSrv->Release();
            g_stereoPackedSnapshotSrv = nullptr;
        }
        if (g_stereoPackedSnapshot != nullptr)
        {
            g_stereoPackedSnapshot->Release();
            g_stereoPackedSnapshot = nullptr;
        }
        return false;
    }

    g_stereoPackedWidth = desc.Width;
    g_stereoPackedHeight = desc.Height;
    g_stereoPackedFormat = desc.Format;
    Ol2Log("[OL2STEREO] packed snapshot created %ux%u fmt=%u", desc.Width, desc.Height,
           static_cast<unsigned>(desc.Format));
    return true;
}

void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* context, UINT numViews,
                                               ID3D11RenderTargetView* const* views,
                                               ID3D11DepthStencilView* depthView)
{
    if (InterlockedCompareExchange(&g_inBypass, 0, 0) != 0 || Ol2Sidecar::Active())
    {
        if (g_originalOMSetRenderTargets != nullptr)
            g_originalOMSetRenderTargets(context, numViews, views, depthView);
        return;
    }

    ID3D11Texture2D* previousTexture = g_boundColorTexture;
    if (previousTexture != nullptr) previousTexture->AddRef();
    const SrvTrace previous = {g_boundTargetId, g_boundTargetW, g_boundTargetH, g_boundTargetFmt};
    const bool sawLeft = InterlockedCompareExchange(&g_boundSawLeft, 0, 0) != 0;
    const bool sawRight = InterlockedCompareExchange(&g_boundSawRight, 0, 0) != 0;

    if (g_originalOMSetRenderTargets != nullptr)
        g_originalOMSetRenderTargets(context, numViews, views, depthView);
    TrackRenderTarget((numViews > 0 && views != nullptr) ? views[0] : nullptr);

    // The original call above has unbound the previous render target, making CopyResource legal.
    if (previousTexture != nullptr && previous.id != g_boundTargetId && sawLeft && sawRight &&
        IsPackedStereoCandidate(previous) && EnsurePackedStereoSnapshot(previousTexture))
    {
        context->CopyResource(g_stereoPackedSnapshot, previousTexture);
        InterlockedExchange(&g_stereoPackedFresh, 1);
        const long long serial = InterlockedIncrement64(&g_stereoPackedSerial);
        if (serial == 1 || (serial % 600) == 0)
            Ol2Log("[OL2STEREO] pre-compositor pair captured serial=%lld id=0x%p %ux%u fmt=%u",
                   serial, reinterpret_cast<void*>(previous.id), previous.width, previous.height,
                   previous.format);
    }
    if (previousTexture != nullptr) previousTexture->Release();
}

void STDMETHODCALLTYPE PSSetShaderResourcesHook(ID3D11DeviceContext* context, UINT startSlot,
                                                 UINT numViews, ID3D11ShaderResourceView* const* views)
{
    if (g_originalPSSetShaderResources != nullptr)
        g_originalPSSetShaderResources(context, startSlot, numViews, views);
    for (UINT i = 0; i < numViews && startSlot + i < 16; ++i)
    {
        SrvTrace trace = {};
        if (views != nullptr && views[i] != nullptr) ReadViewTrace(views[i], &trace);
        g_psSrvs[startSlot + i] = trace;
        g_psSrvViews[startSlot + i] = (views != nullptr) ? views[i] : nullptr;
    }
}

void STDMETHODCALLTYPE PSSetShaderHook(ID3D11DeviceContext* context, ID3D11PixelShader* shader,
                                       ID3D11ClassInstance* const* classes, UINT numClasses)
{
    if (g_originalPSSetShader != nullptr)
        g_originalPSSetShader(context, shader, classes, numClasses);
    g_currentPixelShader = shader;
}

void TraceDrawDestination()
{
    const long half = InterlockedCompareExchange(&g_currentHalf, 0, 0);
    const uintptr_t target = g_boundTargetId;
    const uintptr_t backbuffer = g_backbufferId;
    if (half == 0)
    {
        if (target != 0 && target == backbuffer) InterlockedIncrement(&g_eyeDrawsToBackbufferL);
        else InterlockedIncrement(&g_eyeDrawsOffscreenL);
    }
    else if (half == 1)
    {
        if (target != 0 && target == backbuffer) InterlockedIncrement(&g_eyeDrawsToBackbufferR);
        else InterlockedIncrement(&g_eyeDrawsOffscreenR);
    }

    const unsigned bbW = g_backbufferWidthForViewport;
    const unsigned bbH = g_backbufferHeightForViewport;
    const bool fullBackbufferDraw = target != 0 && target == backbuffer && bbW > 0 && bbH > 0 &&
                                    g_currentVpW + 16 >= bbW && g_currentVpH + 16 >= bbH;
    if (!fullBackbufferDraw) return;

    // The trace identifies the main final-color input by shape and format. Keep an owned SRV for
    // Present; the game may unbind its own reference before the proxy gets control.
    const SrvTrace finalTrace = g_psSrvs[0];
    ID3D11ShaderResourceView* finalView = g_psSrvViews[0];
    if (InterlockedCompareExchange(&g_inBypass, 0, 0) == 0 && finalView != nullptr &&
        finalTrace.format == static_cast<unsigned>(DXGI_FORMAT_R10G10B10A2_UNORM) &&
        finalTrace.width + 32 >= bbW && finalTrace.height + 32 >= bbH &&
        finalView != g_finalColorSrv)
    {
        finalView->AddRef();
        if (g_finalColorSrv != nullptr) g_finalColorSrv->Release();
        g_finalColorSrv = finalView;
        const long n = InterlockedIncrement(&g_finalColorCaptures);
        if (n <= 3 || (n % 600) == 0)
        {
            Ol2Log("[OL2BYPASS] captured live final-color SRV #%ld id=0x%p %ux%u fmt=%u",
                   n, reinterpret_cast<void*>(finalTrace.id), finalTrace.width,
                   finalTrace.height, finalTrace.format);
        }
    }

    for (unsigned slot = 0; slot < 16; ++slot)
    {
        const SrvTrace trace = g_psSrvs[slot];
        if (trace.id == 0 || trace.width + 32 < bbW / 2 || trace.height + 32 < bbH / 2) continue;
        const unsigned long long key = static_cast<unsigned long long>(trace.id) ^
                                       (static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_currentPixelShader)) >> 4) ^
                                       (static_cast<unsigned long long>(slot) << 58);
        bool isNew = false;
        {
            std::lock_guard<std::mutex> lock(g_vpMutex);
            if (g_compositesSeen.find(key) == g_compositesSeen.end() && g_compositesSeen.size() < 96)
            {
                g_compositesSeen[key] = 1;
                isNew = true;
            }
        }
        if (isNew)
            Ol2Log("[OL2COMPOSITE] backbuffer draw ps=0x%p samples slot=%u id=0x%p %ux%u fmt=%u",
                   g_currentPixelShader, slot, reinterpret_cast<void*>(trace.id),
                   trace.width, trace.height, trace.format);
    }
}

void STDMETHODCALLTYPE RSSetViewportsHook(ID3D11DeviceContext* context, UINT numViewports,
                                          const D3D11_VIEWPORT* viewports)
{
    if (InterlockedCompareExchange(&g_inBypass, 0, 0) != 0 || Ol2Sidecar::Active())
    {
        if (g_originalRSSetViewports != nullptr)
            g_originalRSSetViewports(context, numViewports, viewports);
        return;
    }

    if (numViewports >= 1 && viewports != nullptr)
    {
        const unsigned x = static_cast<unsigned>(viewports[0].TopLeftX);
        const unsigned y = static_cast<unsigned>(viewports[0].TopLeftY);
        const unsigned w = static_cast<unsigned>(viewports[0].Width);
        const unsigned h = static_cast<unsigned>(viewports[0].Height);
        g_currentVpX = x; g_currentVpY = y; g_currentVpW = w; g_currentVpH = h;

        // Classify against the SPLIT's actual shape: half width and FULL height. The previous
        // version matched on width alone, so a half-resolution post buffer (half in BOTH
        // dimensions) counted as an eye and made it look like both halves were rendering.
        const unsigned bbW = g_backbufferWidthForViewport;
        const unsigned bbH = g_backbufferHeightForViewport;
        int half = -1;
        if (bbW > 0 && bbH > 0)
        {
            const unsigned halfW = bbW / 2;
            const auto withinTolerance = [](unsigned a, unsigned b, unsigned tolerance)
            {
                return a > b ? (a - b) <= tolerance : (b - a) <= tolerance;
            };
            const bool exactEyeShape = withinTolerance(w, halfW, 8) && withinTolerance(h, bbH, 8) && y <= 8;
            if (exactEyeShape && x <= 8) half = 0;
            else if (exactEyeShape && withinTolerance(x, halfW, 8)) half = 1;
        }
        InterlockedExchange(&g_currentHalf, half);
        InterlockedIncrement(&g_vpSetCalls);
        LogEyeTargetIfNew();

        const unsigned long long key = (static_cast<unsigned long long>(x) << 48) |
                                       (static_cast<unsigned long long>(y) << 32) |
                                       (static_cast<unsigned long long>(w) << 16) |
                                       static_cast<unsigned long long>(h);
        bool isNew = false;
        {
            std::lock_guard<std::mutex> lock(g_vpMutex);
            if (g_vpSeen.find(key) == g_vpSeen.end() && g_vpSeen.size() < 256)
            {
                g_vpSeen[key] = 0;
                isNew = true;
            }
        }
        if (isNew)
        {
            Ol2Log("[OL2VP] NEW rect x=%u y=%u w=%u h=%u -> %s   (a split eye must be w=%u h=%u)",
                   x, y, w, h,
                   half == 0 ? "LEFT EYE" : (half == 1 ? "RIGHT EYE" : "not an eye"),
                   bbW / 2, bbH);
        }
    }
    if (g_originalRSSetViewports != nullptr) g_originalRSSetViewports(context, numViewports, viewports);
}

void CountDrawIntoHalf()
{
    if (InterlockedCompareExchange(&g_inBypass, 0, 0) != 0 || Ol2Sidecar::Active()) return;
    TraceDrawDestination();
    const long half = InterlockedCompareExchange(&g_currentHalf, 0, 0);
    if (half == 0) InterlockedExchange(&g_boundSawLeft, 1);
    else if (half == 1) InterlockedExchange(&g_boundSawRight, 1);
    switch (half)
    {
    case 0:  InterlockedIncrement(&g_vpLeftDraws); break;
    case 1:  InterlockedIncrement(&g_vpRightDraws); break;
    default: InterlockedIncrement(&g_vpFullDraws); break;
    }
}
// ---------------------------------------------------------------------------
// Occlusion-query force
//
// The split builds two views that share ONE scene view state - deliberately, because in this
// engine the post chain hangs off that object, so an eye built from a freshly allocated state
// renders without the night-vision tint and colour grading and the two eyes visibly disagree
// (Outlast 1 measured that and shipped AllocViewStateRva=0 with a "do not switch this back on").
//
// The cost of sharing is that the two views also share the hardware occlusion query results, so
// the verdicts collide and a view can end up culling everything it has. Outlast 1 hit exactly
// this, and its shipped fix is OcclForce=1: hook the query read and answer "visible" every time.
// This is the D3D11 form of that - the same idea one API along.
std::mutex g_queryMutex;
std::unordered_map<void*, bool> g_queryIsOcclusion;

volatile long g_occlForce = 1;      // Outlast 1 ships this ON
volatile long g_occlReads = 0, g_occlForced = 0;

bool IsOcclusionQuery(ID3D11Asynchronous* async)
{
    if (async == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(g_queryMutex);
        auto it = g_queryIsOcclusion.find(async);
        if (it != g_queryIsOcclusion.end()) return it->second;
    }

    bool occlusion = false;
    ID3D11Query* query = nullptr;
    if (SUCCEEDED(async->QueryInterface(__uuidof(ID3D11Query), reinterpret_cast<void**>(&query))) && query != nullptr)
    {
        D3D11_QUERY_DESC desc = {};
        query->GetDesc(&desc);
        occlusion = (desc.Query == D3D11_QUERY_OCCLUSION || desc.Query == D3D11_QUERY_OCCLUSION_PREDICATE);
        query->Release();
    }

    std::lock_guard<std::mutex> lock(g_queryMutex);
    if (g_queryIsOcclusion.size() < 8192) g_queryIsOcclusion[async] = occlusion;
    return occlusion;
}

HRESULT STDMETHODCALLTYPE GetDataHook(ID3D11DeviceContext* context, ID3D11Asynchronous* async,
                                      void* data, UINT dataSize, UINT flags)
{
    const HRESULT hr = (g_originalGetData != nullptr)
                           ? g_originalGetData(context, async, data, dataSize, flags)
                           : E_FAIL;

    if (!InterlockedCompareExchange(&g_occlForce, 0, 0) || data == nullptr || dataSize == 0)
        return hr;
    if (!IsOcclusionQuery(async)) return hr;

    const long n = InterlockedIncrement(&g_occlReads);
    if (n == 1) Ol2Log("[OL2OCCL] hardware occlusion queries are in use; forcing every result to VISIBLE");

    // Answer "plenty of pixels passed" / "visible", and report the answer as ready. A pending
    // query would otherwise leave the engine using last frame's verdict, which is the collision
    // this is trying to remove in the first place.
    if (dataSize >= sizeof(UINT64)) *reinterpret_cast<UINT64*>(data) = 0xFFFFFFFFull;
    else if (dataSize >= sizeof(UINT32)) *reinterpret_cast<UINT32*>(data) = 0xFFFFFFFFu;
    else *reinterpret_cast<unsigned char*>(data) = 1;

    InterlockedIncrement(&g_occlForced);
    return S_OK;
}

CreateDeferredContextFn g_originalCreateDeferredContext = nullptr;

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

std::mutex g_slotMutex;
std::unordered_map<ID3D11Buffer*, unsigned> g_vsBoundBuffers;   // buffer -> VS slot bitmask

std::mutex g_mapMutex;
struct MappedInfo { void* data = nullptr; UINT bytes = 0; };
std::unordered_map<ID3D11Resource*, MappedInfo> g_mapped;

volatile LONG g_vpWrites = 0;
volatile LONG g_vpPatches = 0;
volatile LONG g_vsCbCalls = 0;
volatile LONG g_draws = 0;
volatile LONG g_drawIndexed = 0;
volatile LONG g_drawIndexedInstanced = 0;
volatile LONG g_drawInstanced = 0;
volatile LONG g_drawOther = 0;
volatile LONG g_devicesSeen = 0;
volatile LONG g_deferredContexts = 0;
volatile LONG g_mapsSeen = 0;

// Discovered layout. Written once, then only read.
volatile LONG g_layoutReported = 0;
UINT g_cameraByteOffset = 0;
unsigned g_cameraSlotMask = 0;

// Hotkey state, touched on the render thread only.
bool g_dumpRequest = false;
int g_mode = 0;                    // 0 = observe, 1 = sway (F10), 2 = static offset (F11)
ULONGLONG g_modeStartTick = 0;

float g_swayAmplitudeUu = 200.0f;  // 2 metres sideways. Unmissable on success.
float g_staticOffsetUu = 3.2f;     // about a half IPD at 100 uu per metre
float g_halfEyeUu = 3.2f;          // per-eye parallax for AER. 100 uu = 1 metre, so 3.2 = 64mm IPD

// ---------------------------------------------------------------------------
// the camera write
// ---------------------------------------------------------------------------

unsigned VsSlotMask(ID3D11Resource* resource)
{
    if (resource == nullptr) return 0;
    std::lock_guard<std::mutex> lock(g_slotMutex);
    auto it = g_vsBoundBuffers.find(reinterpret_cast<ID3D11Buffer*>(resource));
    return it != g_vsBoundBuffers.end() ? it->second : 0;
}

bool IsSmallConstantBuffer(ID3D11Resource* resource, UINT& byteWidth)
{
    byteWidth = 0;
    if (resource == nullptr) return false;

    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return false;

    ID3D11Buffer* buffer = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) || buffer == nullptr)
        return false;
    D3D11_BUFFER_DESC desc = {};
    buffer->GetDesc(&desc);
    buffer->Release();

    if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0) return false;
    if (desc.ByteWidth < kVpBytes || desc.ByteWidth > 8192) return false;
    byteWidth = desc.ByteWidth;
    return true;
}

float CurrentShiftUu()
{
    if (g_mode == 2) return g_staticOffsetUu;
    if (g_mode == 1)
    {
        const ULONGLONG elapsed = GetTickCount64() - g_modeStartTick;
        const float seconds = static_cast<float>(elapsed) * 0.001f;
        return g_swayAmplitudeUu * std::sin(seconds * 2.0f);
    }
    return 0.0f;
}

// `data` is always writable memory that may be modified: the mapped constant buffer, or a private staging
// copy. Scans every register boundary for the VP, reports the layout once, then applies the mode.
void HandleConstantBufferWrite(ID3D11Resource* resource, void* data, UINT bytes, unsigned slotMask, const char* api) noexcept
{
    if (data == nullptr || bytes < kVpBytes) return;

    float* base = static_cast<float*>(data);
    const UINT floatCount = bytes / static_cast<UINT>(sizeof(float));
    const float shiftUu = CurrentShiftUu();

    // Scan EVERY register boundary every time, never lock to the primary offset. This CB is 160
    // bytes and carries a second, temporal ViewProjection at byte 64. Shifting only the primary
    // makes the motion-vector/TAA pass see a fake camera jump between the eyes, which shimmers on
    // railings and other thin geometry. Both get the same treatment, so they stay consistent.
    const UINT firstFloat = 0;
    const UINT lastFloat = floatCount >= 16 ? floatCount - 16 : 0;

    for (UINT offsetFloats = firstFloat; offsetFloats <= lastFloat; offsetFloats += 4)
    {
        float* m = base + offsetFloats;
        VpFacts facts;
        if (!LooksLikeViewProjection(m, facts)) continue;

        const LONG n = InterlockedIncrement(&g_vpWrites);

        if (InterlockedCompareExchange(&g_layoutReported, 1, 0) == 0)
        {
            g_cameraByteOffset = offsetFloats * 4;
            g_cameraSlotMask = slotMask;
            Ol2Log("[OL2CAM] LAYOUT FOUND api=%s vsSlotMask=0x%04X cbBytes=%u vpByteOffset=%u (register b%u)",
                   api, slotMask, bytes, g_cameraByteOffset, g_cameraByteOffset / 16);
        }

        const bool dump = g_dumpRequest;
        if (n <= 5 || (n % 20000) == 0 || dump)
        {
            g_dumpRequest = false;
            const float hFovDeg = 2.0f * std::atan(1.0f / facts.p00) * kRadToDeg;
            const float vFovDeg = 2.0f * std::atan(1.0f / facts.p11) * kRadToDeg;
            Ol2Log("[OL2CAM] VP #%ld api=%s bytes=%u off=%u p00=%.4f p11=%.4f hFov=%.2f vFov=%.2f "
                   "fwd=(%.3f %.3f %.3f) right=(%.3f %.3f %.3f) up=(%.3f %.3f %.3f) row3=(%.1f %.1f %.1f %.1f)",
                   n, api, bytes, offsetFloats * 4,
                   static_cast<double>(facts.p00), static_cast<double>(facts.p11),
                   static_cast<double>(hFovDeg), static_cast<double>(vFovDeg),
                   static_cast<double>(facts.fwd[0]), static_cast<double>(facts.fwd[1]), static_cast<double>(facts.fwd[2]),
                   static_cast<double>(facts.right[0]), static_cast<double>(facts.right[1]), static_cast<double>(facts.right[2]),
                   static_cast<double>(facts.up[0]), static_cast<double>(facts.up[1]), static_cast<double>(facts.up[2]),
                   static_cast<double>(m[12]), static_cast<double>(m[13]), static_cast<double>(m[14]), static_cast<double>(m[15]));
        }

        // With the engine hooks driving, head rotation and per-eye parallax are both applied in
        // GetPlayerViewPoint, where the engine's own culling sees them. Doing it here as well
        // would apply everything twice.
        const bool engineOwnsCamera = Ol2Ue3::Enabled();

        // A normal Outlast 2 frame is 16:9 (measured 96x64 degrees), while a headset eye is
        // portrait-ish and much taller. In AER, widen the MAIN gameplay projection to the exact
        // runtime eye FOV before applying pose/parallax. Scaling VP columns is the row-vector
        // equivalent of changing P00/P11 and keeps an already-rotated camera basis intact.
        bool touched = false;
        if (!engineOwnsCamera && Ol2Xr::SessionReady())
        {
            const float currentH = 2.0f * std::atan(1.0f / facts.p00);
            const float currentV = 2.0f * std::atan(1.0f / facts.p11);
            const float aspect = facts.p11 / facts.p00;
            float runtimeH = 0.0f, runtimeV = 0.0f;
            const bool mainGameCamera = currentH > 1.35f && currentH < 2.10f &&
                                        currentV > 0.80f && currentV < 1.40f &&
                                        aspect > 1.60f && aspect < 1.95f;
            if (mainGameCamera && Ol2Xr::RuntimeEyeFov(&runtimeH, &runtimeV))
            {
                const float targetP00 = 1.0f / std::tan(runtimeH * 0.5f);
                const float targetP11 = 1.0f / std::tan(runtimeV * 0.5f);
                const float scaleX = targetP00 / facts.p00;
                const float scaleY = targetP11 / facts.p11;
                for (int row = 0; row < 4; ++row)
                {
                    m[row * 4 + 0] *= scaleX;
                    m[row * 4 + 1] *= scaleY;
                }
                LooksLikeViewProjection(m, facts);
                touched = true;
                static volatile long s_fillLogged = 0;
                if (InterlockedCompareExchange(&s_fillLogged, 1, 0) == 0)
                    Ol2Log("[OL2XRFOV] main view widened %.1fx%.1f -> %.1fx%.1f deg to fill headset",
                           static_cast<double>(currentH * kRadToDeg), static_cast<double>(currentV * kRadToDeg),
                           static_cast<double>(runtimeH * kRadToDeg), static_cast<double>(runtimeV * kRadToDeg));
            }
        }

        // Head orientation first: it changes the basis the eye offset is measured along.
        if (!engineOwnsCamera && g_headEnabled && InterlockedCompareExchange(&g_headValid, 0, 0) != 0)
        {
            PremultiplyRotation(m, g_headYawSign * g_headYawRad, g_headPitchRad);
            LooksLikeViewProjection(m, facts);   // re-derive right/up/fwd from the rotated matrix
            touched = true;
        }

        // Stable mono baseline: both headset eyes consume this same centered render every frame.
        // Per-eye camera translation is deliberately zero until a non-flickering stereo path exists.
        const float eyeShift = 0.0f;
        const float totalShift = shiftUu + eyeShift;
        if (totalShift != 0.0f)
        {
            const float d[3] = {facts.right[0] * totalShift, facts.right[1] * totalShift, facts.right[2] * totalShift};
            TranslateCamera(m, d);
            touched = true;
        }

        if (touched) InterlockedIncrement(&g_vpPatches);
    }

    // Bank the final centered/head-tracked bytes for symmetric per-draw eye replay.
    Ol2Sidecar::ObserveConstantBuffer(resource, data, bytes);
}

// ---------------------------------------------------------------------------
// context hooks
// ---------------------------------------------------------------------------

void STDMETHODCALLTYPE VSSetConstantBuffersHook(ID3D11DeviceContext* context, UINT startSlot, UINT numBuffers, ID3D11Buffer* const* buffers)
{
    if (Ol2Sidecar::Active())
    {
        if (g_originalVSSetConstantBuffers != nullptr)
            g_originalVSSetConstantBuffers(context, startSlot, numBuffers, buffers);
        return;
    }
    InterlockedIncrement(&g_vsCbCalls);

    if (buffers != nullptr)
    {
        std::lock_guard<std::mutex> lock(g_slotMutex);
        if (g_vsBoundBuffers.size() < 4096)
        {
            for (UINT i = 0; i < numBuffers && i < 16; ++i)
            {
                const UINT slot = startSlot + i;
                if (buffers[i] != nullptr && slot < 16)
                    g_vsBoundBuffers[buffers[i]] |= (1u << slot);
            }
        }
    }

    if (g_originalVSSetConstantBuffers != nullptr)
        g_originalVSSetConstantBuffers(context, startSlot, numBuffers, buffers);
}

HRESULT STDMETHODCALLTYPE MapHook(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    if (g_originalMap == nullptr) return E_FAIL;

    const HRESULT hr = g_originalMap(context, resource, subresource, mapType, mapFlags, mapped);

    if (SUCCEEDED(hr) && mapped != nullptr && mapped->pData != nullptr && subresource == 0)
    {
        UINT bytes = 0;
        if (IsSmallConstantBuffer(resource, bytes) && VsSlotMask(resource) != 0)
        {
            InterlockedIncrement(&g_mapsSeen);
            std::lock_guard<std::mutex> lock(g_mapMutex);
            g_mapped[resource] = MappedInfo{mapped->pData, bytes};
        }
    }

    return hr;
}

void STDMETHODCALLTYPE UnmapHook(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource)
{
    MappedInfo info;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        auto it = g_mapped.find(resource);
        if (it != g_mapped.end())
        {
            info = it->second;
            g_mapped.erase(it);
            found = true;
        }
    }

    // Patch while the staging memory is still ours to touch, before the GPU sees it.
    if (found && subresource == 0)
        HandleConstantBufferWrite(resource, info.data, info.bytes, VsSlotMask(resource), "Map");

    if (g_originalUnmap != nullptr) g_originalUnmap(context, resource, subresource);
}

void STDMETHODCALLTYPE UpdateSubresourceHook(ID3D11DeviceContext* context, ID3D11Resource* dstResource, UINT dstSubresource, const D3D11_BOX* dstBox, const void* srcData, UINT srcRowPitch, UINT srcDepthPitch)
{
    if (g_originalUpdateSubresource == nullptr) return;
    if (Ol2Sidecar::Active())
    {
        g_originalUpdateSubresource(context, dstResource, dstSubresource, dstBox, srcData, srcRowPitch, srcDepthPitch);
        return;
    }

    UINT bytes = 0;
    const unsigned slotMask = VsSlotMask(dstResource);
    if (srcData != nullptr && dstSubresource == 0 && dstBox == nullptr && slotMask != 0 &&
        IsSmallConstantBuffer(dstResource, bytes) && bytes >= kVpBytes)
    {
        // srcData is the caller's memory. Never write through it: stage, patch, forward.
        unsigned char staging[8192];
        const UINT copyBytes = bytes < sizeof(staging) ? bytes : static_cast<UINT>(sizeof(staging));
        bool copied = false;
        __try
        {
            memcpy(staging, srcData, copyBytes);
            copied = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (copied)
        {
            HandleConstantBufferWrite(dstResource, staging, copyBytes, slotMask, "UpdateSubresource");
            g_originalUpdateSubresource(context, dstResource, dstSubresource, dstBox, staging, srcRowPitch, srcDepthPitch);
            return;
        }
    }

    g_originalUpdateSubresource(context, dstResource, dstSubresource, dstBox, srcData, srcRowPitch, srcDepthPitch);
}

void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation)
{
    InterlockedIncrement(&g_draws);
    CountDrawIntoHalf();
    if (g_originalDraw != nullptr) g_originalDraw(context, vertexCount, startVertexLocation);
}

void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* context, UINT indexCount, UINT startIndexLocation, INT baseVertexLocation)
{
    InterlockedIncrement(&g_drawIndexed);
    CountDrawIntoHalf();
    if (g_originalDrawIndexed != nullptr)
    {
        if (!Ol2Ue3::Enabled())
            Ol2Sidecar::ReplayDrawIndexed(context, indexCount, startIndexLocation,
                                         baseVertexLocation, g_originalDrawIndexed);
        g_originalDrawIndexed(context, indexCount, startIndexLocation, baseVertexLocation);
    }
}

void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext* context, UINT indexCountPerInstance, UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation)
{
    InterlockedIncrement(&g_drawIndexedInstanced);
    if (g_originalDrawIndexedInstanced != nullptr)
        g_originalDrawIndexedInstanced(context, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
}

void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext* context, UINT vertexCountPerInstance, UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation)
{
    InterlockedIncrement(&g_drawInstanced);
    if (g_originalDrawInstanced != nullptr)
        g_originalDrawInstanced(context, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
}

void STDMETHODCALLTYPE DrawAutoHook(ID3D11DeviceContext* context)
{
    InterlockedIncrement(&g_drawOther);
    if (g_originalDrawAuto != nullptr) g_originalDrawAuto(context);
}

void STDMETHODCALLTYPE DrawIndexedInstancedIndirectHook(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset)
{
    InterlockedIncrement(&g_drawOther);
    if (g_originalDrawIndexedInstancedIndirect != nullptr) g_originalDrawIndexedInstancedIndirect(context, args, offset);
}

void STDMETHODCALLTYPE DrawInstancedIndirectHook(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset)
{
    InterlockedIncrement(&g_drawOther);
    if (g_originalDrawInstancedIndirect != nullptr) g_originalDrawInstancedIndirect(context, args, offset);
}

// Name the module that owns a code address. If a context method points somewhere other than
// d3d11.dll, another layer is already in the chain and that belongs in the log, not inferred.
const char* OwningModule(void* address, char* buffer, size_t bufferBytes)
{
    HMODULE module = nullptr;
    if (address != nullptr &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) && module != nullptr)
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* slash = strrchr(path, 92);   // backslash
        strncpy_s(buffer, bufferBytes, slash != nullptr ? slash + 1 : path, _TRUNCATE);
    }
    else
    {
        strncpy_s(buffer, bufferBytes, "<heap, not a module>", _TRUNCATE);
    }
    return buffer;
}

// Hook the context methods INLINE, at the function addresses, rather than by swapping vtable
// slots.
//
// An early build measured why this is necessary. The context's vtable pointer was
// 0x...4F1686F0 while the context object itself was 0x...4F1686E8 - the vtable lives eight bytes
// into the object's own heap allocation, not in d3d11.dll's image. Something rebuilds that
// per-object table, and the frame - Hinny that GOAT - check came back patch=OVERWRITTEN with zero calls observed.
// Patching a slot in a table that gets rewritten is a losing game.
//
// An inline hook sits on the implementation itself, so every call reaches the detour regardless of which
// vtable, wrapper or proxy object it was dispatched through.
bool InlineHookOne(void** vtable, size_t slot, void* detour, void** original, const char* name, const char* where)
{
    void* target = vtable[slot];

    char owner[64] = {};
    OwningModule(target, owner, sizeof(owner));

    const MH_STATUS created = MH_CreateHook(target, detour, original);
    const MH_STATUS enabled = (created == MH_OK) ? MH_EnableHook(target) : created;

    Ol2Log("[OL2CAM]   %-28s slot=%-2zu addr=0x%p owner=%s create=%d enable=%d",
           name, slot, target, owner, static_cast<int>(created), static_cast<int>(enabled));

    return enabled == MH_OK;
}

volatile LONG g_contextHooked = 0;

void HookContext(ID3D11DeviceContext* context, const char* where)
{
    if (context == nullptr) return;
    if (InterlockedCompareExchange(&g_contextHooked, 1, 0) != 0) return;

    void** vtable = *reinterpret_cast<void***>(context);
    if (vtable == nullptr) { InterlockedExchange(&g_contextHooked, 0); return; }

    const D3D11_DEVICE_CONTEXT_TYPE type = context->GetType();

    MEMORY_BASIC_INFORMATION info = {};
    VirtualQuery(vtable, &info, sizeof(info));
    const char* residence = (info.Type == MEM_IMAGE) ? "MEM_IMAGE (module)" : "MEM_PRIVATE (heap, per-object)";

    Ol2Log("[OL2CAM] inline-hooking context at %s ctx=0x%p vtable=0x%p (%+lld from ctx) %s type=%s",
           where, context, vtable,
           static_cast<long long>(reinterpret_cast<char*>(vtable) - reinterpret_cast<char*>(context)),
           residence, type == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "IMMEDIATE" : "DEFERRED");

    int ok = 0;
    ok += InlineHookOne(vtable,  7, reinterpret_cast<void*>(&VSSetConstantBuffersHook),         reinterpret_cast<void**>(&g_originalVSSetConstantBuffers), "VSSetConstantBuffers", where) ? 1 : 0;
    ok += InlineHookOne(vtable,  8, reinterpret_cast<void*>(&PSSetShaderResourcesHook),         reinterpret_cast<void**>(&g_originalPSSetShaderResources), "PSSetShaderResources", where) ? 1 : 0;
    ok += InlineHookOne(vtable,  9, reinterpret_cast<void*>(&PSSetShaderHook),                  reinterpret_cast<void**>(&g_originalPSSetShader), "PSSetShader", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 12, reinterpret_cast<void*>(&DrawIndexedHook),                  reinterpret_cast<void**>(&g_originalDrawIndexed), "DrawIndexed", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 13, reinterpret_cast<void*>(&DrawHook),                         reinterpret_cast<void**>(&g_originalDraw), "Draw", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 14, reinterpret_cast<void*>(&MapHook),                          reinterpret_cast<void**>(&g_originalMap), "Map", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 15, reinterpret_cast<void*>(&UnmapHook),                        reinterpret_cast<void**>(&g_originalUnmap), "Unmap", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 20, reinterpret_cast<void*>(&DrawIndexedInstancedHook),         reinterpret_cast<void**>(&g_originalDrawIndexedInstanced), "DrawIndexedInstanced", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 21, reinterpret_cast<void*>(&DrawInstancedHook),                reinterpret_cast<void**>(&g_originalDrawInstanced), "DrawInstanced", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 38, reinterpret_cast<void*>(&DrawAutoHook),                     reinterpret_cast<void**>(&g_originalDrawAuto), "DrawAuto", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 39, reinterpret_cast<void*>(&DrawIndexedInstancedIndirectHook), reinterpret_cast<void**>(&g_originalDrawIndexedInstancedIndirect), "DrawIndexedInstancedIndirect", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 40, reinterpret_cast<void*>(&DrawInstancedIndirectHook),        reinterpret_cast<void**>(&g_originalDrawInstancedIndirect), "DrawInstancedIndirect", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 44, reinterpret_cast<void*>(&RSSetViewportsHook),               reinterpret_cast<void**>(&g_originalRSSetViewports), "RSSetViewports", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 29, reinterpret_cast<void*>(&GetDataHook),                      reinterpret_cast<void**>(&g_originalGetData), "GetData (occlusion)", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 33, reinterpret_cast<void*>(&OMSetRenderTargetsHook),           reinterpret_cast<void**>(&g_originalOMSetRenderTargets), "OMSetRenderTargets", where) ? 1 : 0;
    ok += InlineHookOne(vtable, 48, reinterpret_cast<void*>(&UpdateSubresourceHook),            reinterpret_cast<void**>(&g_originalUpdateSubresource), "UpdateSubresource", where) ? 1 : 0;

    Ol2Log("[OL2CAM] inline hooks installed: %d of 16", ok);
}

HRESULT STDMETHODCALLTYPE CreateDeferredContextHook(ID3D11Device* device, UINT flags, ID3D11DeviceContext** deferred)
{
    const HRESULT hr = g_originalCreateDeferredContext(device, flags, deferred);
    const LONG n = InterlockedIncrement(&g_deferredContexts);

    if (SUCCEEDED(hr) && deferred != nullptr && *deferred != nullptr)
    {
        Ol2Log("[OL2CAM] CreateDeferredContext #%ld ctx=0x%p - the game DOES use deferred contexts", n, *deferred);
        HookContext(*deferred, "CreateDeferredContext");
    }

    return hr;
}

void HookDevice(ID3D11Device* device, const char* where)
{
    if (device == nullptr) return;

    const LONG n = InterlockedIncrement(&g_devicesSeen);
    ID3D11DeviceContext* immediate = nullptr;
    device->GetImmediateContext(&immediate);

    Ol2Log("[OL2CAM] device #%ld at %s dev=0x%p immediateCtx=0x%p creationFlags=0x%X featureLevel=0x%04X",
           n, where, device, immediate, device->GetCreationFlags(),
           static_cast<unsigned>(device->GetFeatureLevel()));

    Ol2PatchVTable(device, 27, reinterpret_cast<void*>(&CreateDeferredContextHook),
                   reinterpret_cast<void**>(&g_originalCreateDeferredContext));

    if (immediate != nullptr)
    {
        HookContext(immediate, where);
        immediate->Release();
    }
}

// ---------------------------------------------------------------------------
// d3d11.dll device entry points
// ---------------------------------------------------------------------------

using D3D11CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

D3D11CreateDeviceFn g_originalD3D11CreateDevice = nullptr;
D3D11CreateDeviceAndSwapChainFn g_originalD3D11CreateDeviceAndSwapChain = nullptr;

HRESULT WINAPI D3D11CreateDeviceHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
                                     const D3D_FEATURE_LEVEL* featureLevels, UINT numFeatureLevels, UINT sdkVersion,
                                     ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context)
{
    const HRESULT hr = g_originalD3D11CreateDevice(adapter, driverType, software, flags, featureLevels,
                                                   numFeatureLevels, sdkVersion, device, featureLevel, context);
    if (SUCCEEDED(hr) && device != nullptr && *device != nullptr)
        HookDevice(*device, "D3D11CreateDevice");
    return hr;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChainHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
                                                 const D3D_FEATURE_LEVEL* featureLevels, UINT numFeatureLevels, UINT sdkVersion,
                                                 const DXGI_SWAP_CHAIN_DESC* swapChainDesc, IDXGISwapChain** swapChain,
                                                 ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context)
{
    const HRESULT hr = g_originalD3D11CreateDeviceAndSwapChain(adapter, driverType, software, flags, featureLevels,
                                                               numFeatureLevels, sdkVersion, swapChainDesc, swapChain,
                                                               device, featureLevel, context);
    if (SUCCEEDED(hr) && device != nullptr && *device != nullptr)
        HookDevice(*device, "D3D11CreateDeviceAndSwapChain");
    return hr;
}

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                      ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);

template <typename T>
void ReleaseTraceCom(T*& value)
{
    if (value != nullptr) { value->Release(); value = nullptr; }
}

bool EnsureBypassPipeline(ID3D11Device* device)
{
    if (g_bypassVs != nullptr && g_bypassPs != nullptr && g_bypassSampler != nullptr &&
        g_bypassRaster != nullptr && g_bypassBlend != nullptr && g_bypassDepth != nullptr)
        return true;
    if (device == nullptr) return false;

    HMODULE compilerModule = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compilerModule == nullptr) return false;
    auto compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(compilerModule, "D3DCompile"));
    if (compile == nullptr) { FreeLibrary(compilerModule); return false; }

    static constexpr char kVs[] = R"(
struct Out { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
Out main(uint id : SV_VertexID) {
    Out o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2,-2) + float2(-1,1), 0, 1); o.uv = uv; return o;
})";
    static constexpr char kPs[] = R"(
Texture2D finalColor : register(t0); SamplerState clampLinear : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return finalColor.Sample(clampLinear, uv);
})";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(kVs, sizeof(kVs) - 1, "OL2BypassVS", nullptr, nullptr,
                         "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errors);
    ReleaseTraceCom(errors);
    if (SUCCEEDED(hr))
        hr = compile(kPs, sizeof(kPs) - 1, "OL2BypassPS", nullptr, nullptr,
                     "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors);
    ReleaseTraceCom(errors);
    if (SUCCEEDED(hr) && vsBlob != nullptr)
        hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_bypassVs);
    if (SUCCEEDED(hr) && psBlob != nullptr)
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_bypassPs);
    ReleaseTraceCom(vsBlob);
    ReleaseTraceCom(psBlob);
    FreeLibrary(compilerModule);
    if (FAILED(hr) || g_bypassVs == nullptr || g_bypassPs == nullptr) return false;

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampler, &g_bypassSampler);

    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&raster, &g_bypassRaster);

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = device->CreateBlendState(&blend, &g_bypassBlend);

    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    if (SUCCEEDED(hr)) hr = device->CreateDepthStencilState(&depth, &g_bypassDepth);

    if (FAILED(hr) || g_bypassSampler == nullptr || g_bypassRaster == nullptr ||
        g_bypassBlend == nullptr || g_bypassDepth == nullptr) return false;
    Ol2Log("[OL2BYPASS] complete-frame dual-eye pipeline created");
    return true;
}

bool EnsureStereoConvertedOutput(ID3D11Device* device)
{
    if (device == nullptr || g_stereoPackedWidth == 0 || g_stereoPackedHeight == 0) return false;
    if (g_stereoConverted != nullptr && g_stereoConvertedRtv != nullptr &&
        g_stereoConvertedWidth == g_stereoPackedWidth &&
        g_stereoConvertedHeight == g_stereoPackedHeight) return true;

    ReleaseTraceCom(g_stereoConvertedRtv);
    ReleaseTraceCom(g_stereoConverted);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = g_stereoPackedWidth;
    desc.Height = g_stereoPackedHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_stereoConverted);
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(g_stereoConverted, nullptr, &g_stereoConvertedRtv);
    if (FAILED(hr) || g_stereoConverted == nullptr || g_stereoConvertedRtv == nullptr)
    {
        ReleaseTraceCom(g_stereoConvertedRtv);
        ReleaseTraceCom(g_stereoConverted);
        return false;
    }

    g_stereoConvertedWidth = desc.Width;
    g_stereoConvertedHeight = desc.Height;
    Ol2Log("[OL2STEREO] R10-to-R8 conversion target created %ux%u", desc.Width, desc.Height);
    return true;
}

bool ConvertPackedStereoAtPresent()
{
    if (InterlockedCompareExchange(&g_stereoPackedFresh, 0, 0) == 0 ||
        g_stereoPackedSnapshot == nullptr || g_stereoPackedSnapshotSrv == nullptr) return false;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    g_stereoPackedSnapshot->GetDevice(&device);
    if (device != nullptr) device->GetImmediateContext(&context);
    if (device == nullptr || context == nullptr || !EnsureBypassPipeline(device) ||
        !EnsureStereoConvertedOutput(device))
    {
        static volatile long warned = 0;
        if (InterlockedIncrement(&warned) == 1)
            Ol2Log("[OL2STEREO] packed conversion setup FAILED; stable mono remains active");
        ReleaseTraceCom(context);
        ReleaseTraceCom(device);
        return false;
    }

    ID3D11InputLayout* oldLayout = nullptr;
    ID3D11Buffer* oldVertexBuffer = nullptr; UINT oldStride = 0, oldOffset = 0;
    ID3D11Buffer* oldIndexBuffer = nullptr; DXGI_FORMAT oldIndexFormat = DXGI_FORMAT_UNKNOWN; UINT oldIndexOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* oldVs = nullptr; ID3D11HullShader* oldHs = nullptr;
    ID3D11DomainShader* oldDs = nullptr; ID3D11GeometryShader* oldGs = nullptr;
    ID3D11PixelShader* oldPs = nullptr;
    ID3D11ShaderResourceView* oldSrv = nullptr; ID3D11SamplerState* oldSampler = nullptr;
    ID3D11RenderTargetView* oldRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    ID3D11RasterizerState* oldRaster = nullptr; ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldBlendFactor[4] = {}; UINT oldSampleMask = 0;
    ID3D11DepthStencilState* oldDepth = nullptr; UINT oldStencilRef = 0;
    D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT oldViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    context->IAGetInputLayout(&oldLayout);
    context->IAGetVertexBuffers(0, 1, &oldVertexBuffer, &oldStride, &oldOffset);
    context->IAGetIndexBuffer(&oldIndexBuffer, &oldIndexFormat, &oldIndexOffset);
    context->IAGetPrimitiveTopology(&oldTopology);
    context->VSGetShader(&oldVs, nullptr, nullptr); context->HSGetShader(&oldHs, nullptr, nullptr);
    context->DSGetShader(&oldDs, nullptr, nullptr); context->GSGetShader(&oldGs, nullptr, nullptr);
    context->PSGetShader(&oldPs, nullptr, nullptr);
    context->PSGetShaderResources(0, 1, &oldSrv); context->PSGetSamplers(0, 1, &oldSampler);
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, &oldDsv);
    context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    context->OMGetDepthStencilState(&oldDepth, &oldStencilRef);
    context->RSGetState(&oldRaster); context->RSGetViewports(&oldViewportCount, oldViewports);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(g_stereoPackedWidth);
    viewport.Height = static_cast<float>(g_stereoPackedHeight);
    viewport.MaxDepth = 1.0f;
    UINT zero = 0;
    ID3D11Buffer* noBuffer = nullptr;
    ID3D11ShaderResourceView* noSrv = nullptr;
    const FLOAT blendFactor[4] = {};

    InterlockedExchange(&g_inBypass, 1);
    context->OMSetRenderTargets(1, &g_stereoConvertedRtv, nullptr);
    context->OMSetBlendState(g_bypassBlend, blendFactor, 0xffffffffu);
    context->OMSetDepthStencilState(g_bypassDepth, 0);
    context->RSSetState(g_bypassRaster); context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr); context->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_bypassVs, nullptr, 0); context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0); context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(g_bypassPs, nullptr, 0);
    context->PSSetShaderResources(0, 1, &g_stereoPackedSnapshotSrv);
    context->PSSetSamplers(0, 1, &g_bypassSampler);
    context->Draw(3, 0);
    context->PSSetShaderResources(0, 1, &noSrv);

    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, oldDsv);
    context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    context->OMSetDepthStencilState(oldDepth, oldStencilRef);
    context->RSSetState(oldRaster);
    if (oldViewportCount > 0) context->RSSetViewports(oldViewportCount, oldViewports);
    context->IASetInputLayout(oldLayout);
    context->IASetVertexBuffers(0, 1, &oldVertexBuffer, &oldStride, &oldOffset);
    context->IASetIndexBuffer(oldIndexBuffer, oldIndexFormat, oldIndexOffset);
    context->IASetPrimitiveTopology(oldTopology);
    context->VSSetShader(oldVs, nullptr, 0); context->HSSetShader(oldHs, nullptr, 0);
    context->DSSetShader(oldDs, nullptr, 0); context->GSSetShader(oldGs, nullptr, 0);
    context->PSSetShader(oldPs, nullptr, 0); context->PSSetShaderResources(0, 1, &oldSrv);
    context->PSSetSamplers(0, 1, &oldSampler);
    InterlockedExchange(&g_inBypass, 0);

    ReleaseTraceCom(oldLayout); ReleaseTraceCom(oldVertexBuffer); ReleaseTraceCom(oldIndexBuffer);
    ReleaseTraceCom(oldVs); ReleaseTraceCom(oldHs); ReleaseTraceCom(oldDs); ReleaseTraceCom(oldGs);
    ReleaseTraceCom(oldPs); ReleaseTraceCom(oldSrv); ReleaseTraceCom(oldSampler);
    for (auto& rtv : oldRtvs) ReleaseTraceCom(rtv);
    ReleaseTraceCom(oldDsv); ReleaseTraceCom(oldRaster); ReleaseTraceCom(oldBlend); ReleaseTraceCom(oldDepth);
    ReleaseTraceCom(context); ReleaseTraceCom(device);

    InterlockedExchange(&g_stereoPackedFresh, 0);
    const long long serial = InterlockedCompareExchange64(&g_stereoPackedSerial, 0, 0);
    InterlockedExchange64(&g_stereoConvertedSerial, serial);
    InterlockedExchange(&g_stereoConvertedFresh, 1);
    if (serial == 1 || (serial % 600) == 0)
        Ol2Log("[OL2STEREO] converted pre-compositor pair ready serial=%lld", serial);
    return true;
}

bool CopyStereoToFlatBackbuffer(IDXGISwapChain* swapChain)
{
    if (swapChain == nullptr || g_stereoConverted == nullptr) return false;

    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backbuffer))) ||
        backbuffer == nullptr) return false;

    D3D11_TEXTURE2D_DESC src = {}, dst = {};
    g_stereoConverted->GetDesc(&src);
    backbuffer->GetDesc(&dst);
    if (src.Format != dst.Format)
    {
        backbuffer->Release();
        return false;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    backbuffer->GetDevice(&device);
    if (device != nullptr) device->GetImmediateContext(&context);
    if (context == nullptr)
    {
        ReleaseTraceCom(device);
        backbuffer->Release();
        return false;
    }

    ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* depthTarget = nullptr;
    InterlockedExchange(&g_inBypass, 1);
    SaveAndUnbindTargets(context, renderTargets, &depthTarget);
    const UINT width = src.Width < dst.Width ? src.Width : dst.Width;
    const UINT height = src.Height < dst.Height ? src.Height : dst.Height;
    const D3D11_BOX box = {0, 0, 0, width, height, 1};
    context->CopySubresourceRegion(backbuffer, 0, 0, 0, 0, g_stereoConverted, 0, &box);
    RestoreTargets(context, renderTargets, depthTarget);
    InterlockedExchange(&g_inBypass, 0);

    ReleaseTraceCom(context);
    ReleaseTraceCom(device);
    backbuffer->Release();
    InterlockedExchange(&g_stereoConvertedFresh, 0);

    static volatile long frames = 0;
    const long n = InterlockedIncrement(&frames);
    if (n == 1 || (n % 600) == 0)
        Ol2Log("[OL2STEREO] raw pre-compositor pair copied to flat SBS preview: frames=%ld", n);
    return true;
}

bool EnsurePresentSnapshot(ID3D11Device* device, ID3D11Texture2D* backbuffer)
{
    if (device == nullptr || backbuffer == nullptr) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    backbuffer->GetDesc(&desc);
    if (g_presentSnapshot != nullptr && g_presentSnapshotSrv != nullptr &&
        g_presentSnapshotWidth == desc.Width && g_presentSnapshotHeight == desc.Height &&
        g_presentSnapshotFormat == desc.Format) return true;

    ReleaseTraceCom(g_presentSnapshotSrv);
    ReleaseTraceCom(g_presentSnapshot);
    D3D11_TEXTURE2D_DESC copyDesc = desc;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copyDesc.CPUAccessFlags = 0;
    copyDesc.MiscFlags = 0;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.SampleDesc.Count = 1;
    copyDesc.SampleDesc.Quality = 0;
    HRESULT hr = device->CreateTexture2D(&copyDesc, nullptr, &g_presentSnapshot);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(g_presentSnapshot, nullptr,
                                                              &g_presentSnapshotSrv);
    if (FAILED(hr) || g_presentSnapshot == nullptr || g_presentSnapshotSrv == nullptr)
    {
        ReleaseTraceCom(g_presentSnapshotSrv);
        ReleaseTraceCom(g_presentSnapshot);
        return false;
    }
    g_presentSnapshotWidth = desc.Width;
    g_presentSnapshotHeight = desc.Height;
    g_presentSnapshotFormat = desc.Format;
    Ol2Log("[OL2BYPASS] completed-frame snapshot created %ux%u fmt=%u (includes UI)",
           desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
    return true;
}

bool DrawRightFromFinalColor(IDXGISwapChain* swapChain)
{
    // This compositor exists only to make the FLAT monitor preview readable. In VR the OpenXR
    // path must capture the original complete frame; feeding it this already-duplicated SBS
    // preview would put two miniature copies of the game into each headset eye.
    if (Ol2XrEnabled() || Ol2Ue3::Enabled()) return false;
    if (swapChain == nullptr ||
        InterlockedCompareExchange(&g_bypassEnabled, 0, 0) == 0) return false;

    ID3D11Texture2D* backbuffer = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backbuffer))) || backbuffer == nullptr)
        return false;
    backbuffer->GetDevice(&device);
    if (device != nullptr) device->GetImmediateContext(&context);
    if (device == nullptr || context == nullptr || !EnsureBypassPipeline(device) ||
        !EnsurePresentSnapshot(device, backbuffer) ||
        FAILED(device->CreateRenderTargetView(backbuffer, nullptr, &target)) || target == nullptr)
    {
        ReleaseTraceCom(context); ReleaseTraceCom(device); ReleaseTraceCom(backbuffer);
        return false;
    }

    ID3D11InputLayout* oldLayout = nullptr;
    ID3D11Buffer* oldVertexBuffer = nullptr; UINT oldStride = 0, oldOffset = 0;
    ID3D11Buffer* oldIndexBuffer = nullptr; DXGI_FORMAT oldIndexFormat = DXGI_FORMAT_UNKNOWN; UINT oldIndexOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* oldVs = nullptr; ID3D11HullShader* oldHs = nullptr;
    ID3D11DomainShader* oldDs = nullptr; ID3D11GeometryShader* oldGs = nullptr;
    ID3D11PixelShader* oldPs = nullptr;
    ID3D11ShaderResourceView* oldSrv = nullptr; ID3D11SamplerState* oldSampler = nullptr;
    ID3D11RenderTargetView* oldRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    ID3D11RasterizerState* oldRaster = nullptr; ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldBlendFactor[4] = {}; UINT oldSampleMask = 0;
    ID3D11DepthStencilState* oldDepth = nullptr; UINT oldStencilRef = 0;
    D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT oldViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    context->IAGetInputLayout(&oldLayout);
    context->IAGetVertexBuffers(0, 1, &oldVertexBuffer, &oldStride, &oldOffset);
    context->IAGetIndexBuffer(&oldIndexBuffer, &oldIndexFormat, &oldIndexOffset);
    context->IAGetPrimitiveTopology(&oldTopology);
    context->VSGetShader(&oldVs, nullptr, nullptr); context->HSGetShader(&oldHs, nullptr, nullptr);
    context->DSGetShader(&oldDs, nullptr, nullptr); context->GSGetShader(&oldGs, nullptr, nullptr);
    context->PSGetShader(&oldPs, nullptr, nullptr);
    context->PSGetShaderResources(0, 1, &oldSrv); context->PSGetSamplers(0, 1, &oldSampler);
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, &oldDsv);
    context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    context->OMGetDepthStencilState(&oldDepth, &oldStencilRef);
    context->RSGetState(&oldRaster); context->RSGetViewports(&oldViewportCount, oldViewports);

    D3D11_TEXTURE2D_DESC desc = {}; backbuffer->GetDesc(&desc);
    const UINT halfWidth = desc.Width / 2;
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = 0.0f; viewport.Width = static_cast<float>(halfWidth);
    viewport.Height = static_cast<float>(desc.Height); viewport.MaxDepth = 1.0f;
    UINT zero = 0; ID3D11Buffer* noBuffer = nullptr; ID3D11ShaderResourceView* noSrv = nullptr;
    const FLOAT blendFactor[4] = {};

    InterlockedExchange(&g_inBypass, 1);
    // Preserve the FINISHED frame before the mod's own draw. Unlike the earlier scene-color SRV,
    // this contains everything Unreal rendered, including HUD, pause screens, and selection UI.
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(g_presentSnapshot, backbuffer);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(g_bypassBlend, blendFactor, 0xffffffffu);
    context->OMSetDepthStencilState(g_bypassDepth, 0);
    context->RSSetState(g_bypassRaster); context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr); context->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_bypassVs, nullptr, 0); context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0); context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(g_bypassPs, nullptr, 0);
    context->PSSetShaderResources(0, 1, &g_presentSnapshotSrv); context->PSSetSamplers(0, 1, &g_bypassSampler);
    context->Draw(3, 0);
    viewport.TopLeftX = static_cast<float>(halfWidth);
    viewport.Width = static_cast<float>(desc.Width - halfWidth);
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);
    context->PSSetShaderResources(0, 1, &noSrv);

    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, oldDsv);
    context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    context->OMSetDepthStencilState(oldDepth, oldStencilRef);
    context->RSSetState(oldRaster);
    if (oldViewportCount > 0) context->RSSetViewports(oldViewportCount, oldViewports);
    context->IASetInputLayout(oldLayout); context->IASetVertexBuffers(0, 1, &oldVertexBuffer, &oldStride, &oldOffset);
    context->IASetIndexBuffer(oldIndexBuffer, oldIndexFormat, oldIndexOffset);
    context->IASetPrimitiveTopology(oldTopology);
    context->VSSetShader(oldVs, nullptr, 0); context->HSSetShader(oldHs, nullptr, 0);
    context->DSSetShader(oldDs, nullptr, 0); context->GSSetShader(oldGs, nullptr, 0);
    context->PSSetShader(oldPs, nullptr, 0); context->PSSetShaderResources(0, 1, &oldSrv);
    context->PSSetSamplers(0, 1, &oldSampler);
    InterlockedExchange(&g_inBypass, 0);

    ReleaseTraceCom(oldLayout); ReleaseTraceCom(oldVertexBuffer); ReleaseTraceCom(oldIndexBuffer);
    ReleaseTraceCom(oldVs); ReleaseTraceCom(oldHs); ReleaseTraceCom(oldDs); ReleaseTraceCom(oldGs); ReleaseTraceCom(oldPs);
    ReleaseTraceCom(oldSrv); ReleaseTraceCom(oldSampler);
    for (auto& rtv : oldRtvs) ReleaseTraceCom(rtv);
    ReleaseTraceCom(oldDsv); ReleaseTraceCom(oldRaster); ReleaseTraceCom(oldBlend); ReleaseTraceCom(oldDepth);
    ReleaseTraceCom(target); ReleaseTraceCom(context); ReleaseTraceCom(device); ReleaseTraceCom(backbuffer);

    const long n = InterlockedIncrement(&g_bypassFrames);
    if (n == 1 || (n % 600) == 0)
        Ol2Log("[OL2BYPASS] completed game+UI frame duplicated to both eye slots: frames=%ld", n);
    return true;
}

bool KeyWentDown(int key, bool& wasDown)
{
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}
}

namespace Ol2Camera
{
void InstallDeviceExportHooks()
{
    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) wcsncpy_s(path, L"C:\\Windows\\System32", _TRUNCATE);
    wcsncat_s(path, L"\\d3d11.dll", _TRUNCATE);

    HMODULE d3d11 = LoadLibraryW(path);
    if (d3d11 == nullptr)
    {
        Ol2Log("[OL2CAM] could not load d3d11.dll err=%lu", GetLastError());
        return;
    }

    if (MH_Initialize() != MH_OK)
    {
        Ol2Log("[OL2CAM] MH_Initialize FAILED");
        return;
    }

    void* createDevice = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    void* createDeviceAndSwapChain = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));

    const bool a = createDevice != nullptr &&
                   MH_CreateHook(createDevice, reinterpret_cast<void*>(&D3D11CreateDeviceHook),
                                 reinterpret_cast<void**>(&g_originalD3D11CreateDevice)) == MH_OK;
    const bool b = createDeviceAndSwapChain != nullptr &&
                   MH_CreateHook(createDeviceAndSwapChain, reinterpret_cast<void*>(&D3D11CreateDeviceAndSwapChainHook),
                                 reinterpret_cast<void**>(&g_originalD3D11CreateDeviceAndSwapChain)) == MH_OK;

    const bool enabled = MH_EnableHook(MH_ALL_HOOKS) == MH_OK;

    Ol2Log("[OL2CAM] d3d11 export hooks: D3D11CreateDevice=%u D3D11CreateDeviceAndSwapChain=%u enabled=%u",
           a ? 1u : 0u, b ? 1u : 0u, enabled ? 1u : 0u);
}

void InstallContextHooks(IDXGISwapChain* swapChain, const char* where)
{
    if (swapChain == nullptr) return;

    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || device == nullptr)
    {
        Ol2Log("[OL2CAM] GetDevice failed at %s", where);
        return;
    }

    HookDevice(device, where);
    device->Release();

    Ol2Log("[OL2CAM] F9 = dump live VP, F10 = sway proof (world must move), F11 = static half-IPD offset");
}

bool CapturePrimaryHalf()
{
    IDXGISwapChain* swapChain = g_doubleSwapChain;
    if (swapChain == nullptr) return false;
    swapChain->AddRef();

    ID3D11Texture2D* backbuffer = nullptr;
    const HRESULT gotBuffer = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                    reinterpret_cast<void**>(&backbuffer));
    swapChain->Release();
    if (FAILED(gotBuffer) || backbuffer == nullptr) return false;

    bool captured = false;
    if (EnsureHalfScratch(backbuffer, &g_doubleLeftScratch, &g_doubleLeftWidth,
                          &g_doubleLeftHeight, &g_doubleLeftFormat, "left-eye"))
    {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        backbuffer->GetDevice(&device);
        if (device != nullptr) device->GetImmediateContext(&context);
        if (context != nullptr)
        {
            ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
            ID3D11DepthStencilView* depthTarget = nullptr;
            SaveAndUnbindTargets(context, renderTargets, &depthTarget);

            D3D11_TEXTURE2D_DESC desc = {};
            backbuffer->GetDesc(&desc);
            const D3D11_BOX liveHalf = {0, 0, 0, desc.Width / 2, desc.Height, 1};
            context->CopySubresourceRegion(g_doubleLeftScratch, 0, 0, 0, 0,
                                           backbuffer, 0, &liveHalf);
            RestoreTargets(context, renderTargets, depthTarget);
            context->Release();
            captured = true;
            InterlockedExchange(&g_doubleLeftFresh, 1);
            const long n = InterlockedIncrement(&g_doubleCaptures);
            if (n == 1 || (n % 600) == 0)
                Ol2Log("[OL2DOUBLE] pass-one GPU capture completed after OM unbind: frames=%ld", n);
        }
        if (device != nullptr) device->Release();
    }
    backbuffer->Release();
    return captured;
}

bool AcquireStereoFrame(ID3D11Texture2D** texture, unsigned* width, unsigned* height,
                        unsigned long long* serial)
{
    if (Ol2Sidecar::AcquireStereoFrame(texture, width, height, serial)) return true;
    if (texture == nullptr) return false;
    *texture = nullptr;
    if (InterlockedCompareExchange(&g_stereoConvertedFresh, 0, 0) == 0 ||
        g_stereoConverted == nullptr) return false;

    g_stereoConverted->AddRef();
    *texture = g_stereoConverted;
    if (width != nullptr) *width = g_stereoConvertedWidth;
    if (height != nullptr) *height = g_stereoConvertedHeight;
    if (serial != nullptr)
        *serial = static_cast<unsigned long long>(
            InterlockedCompareExchange64(&g_stereoConvertedSerial, 0, 0));
    InterlockedExchange(&g_stereoConvertedFresh, 0);
    return true;
}

void OnPresent(IDXGISwapChain* swapChain, unsigned backbufferWidth, unsigned backbufferHeight)
{
    g_backbufferWidthForViewport = backbufferWidth;
    g_backbufferHeightForViewport = backbufferHeight;

    if (swapChain != nullptr)
    {
        ID3D11Texture2D* traceBackbuffer = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                           reinterpret_cast<void**>(&traceBackbuffer))) &&
            traceBackbuffer != nullptr)
        {
            SrvTrace trace = {};
            if (ReadResourceTrace(traceBackbuffer, &trace))
            {
                const uintptr_t previous = g_backbufferId;
                g_backbufferId = trace.id;
                if (previous != trace.id)
                    Ol2Log("[OL2RT] backbuffer identity id=0x%p %ux%u fmt=%u",
                           reinterpret_cast<void*>(trace.id), trace.width, trace.height, trace.format);
            }
            traceBackbuffer->Release();
        }
    }

    static bool s_f1 = false;
    if (KeyWentDown(VK_F1, s_f1))
    {
        const bool enabled = !Ol2Ue3::Enabled();
        Ol2Ue3::SetEnabled(enabled);
        if (!enabled)
        {
            InterlockedExchange(&g_stereoPackedFresh, 0);
            InterlockedExchange(&g_stereoConvertedFresh, 0);
        }
        Ol2Log("[OL2STEREO] F1 mode: %s", enabled ? "experimental same-frame stereo" : "stable mono");
    }

    const bool stereoConvertedThisFrame =
        Ol2Ue3::Enabled() && ConvertPackedStereoAtPresent();

    if (swapChain != nullptr && swapChain != g_doubleSwapChain)
    {
        swapChain->AddRef();
        if (g_doubleSwapChain != nullptr) g_doubleSwapChain->Release();
        g_doubleSwapChain = swapChain;
        Ol2Log("[OL2DOUBLE] swapchain cached; safe between-pass capture armed for the next frame");
    }

    // Pass two is now sitting in Outlast's known-live LEFT slot. Preserve it, restore pass one to
    // the left, and copy pass two to the right. Both sources came from complete primary renders.
    if (swapChain != nullptr && InterlockedCompareExchange(&g_doubleLeftFresh, 0, 0) != 0 &&
        g_doubleLeftScratch != nullptr)
    {
        ID3D11Texture2D* backbuffer = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                           reinterpret_cast<void**>(&backbuffer))) &&
            backbuffer != nullptr)
        {
            if (EnsureHalfScratch(backbuffer, &g_rightMirrorScratch, &g_rightMirrorWidth,
                                  &g_rightMirrorHeight, &g_rightMirrorFormat, "right-eye"))
            {
                ID3D11Device* device = nullptr;
                ID3D11DeviceContext* context = nullptr;
                backbuffer->GetDevice(&device);
                if (device != nullptr) device->GetImmediateContext(&context);
                if (context != nullptr)
                {
                    ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
                    ID3D11DepthStencilView* depthTarget = nullptr;
                    SaveAndUnbindTargets(context, renderTargets, &depthTarget);

                    D3D11_TEXTURE2D_DESC desc = {};
                    backbuffer->GetDesc(&desc);
                    const unsigned halfWidth = desc.Width / 2;
                    const D3D11_BOX liveHalf = {0, 0, 0, halfWidth, desc.Height, 1};
                    const D3D11_BOX scratchHalf = {0, 0, 0, halfWidth, desc.Height, 1};
                    context->CopySubresourceRegion(g_rightMirrorScratch, 0, 0, 0, 0,
                                                   backbuffer, 0, &liveHalf);
                    context->CopySubresourceRegion(backbuffer, 0, 0, 0, 0,
                                                   g_doubleLeftScratch, 0, &scratchHalf);
                    context->CopySubresourceRegion(backbuffer, 0, halfWidth, 0, 0,
                                                   g_rightMirrorScratch, 0, &scratchHalf);
                    RestoreTargets(context, renderTargets, depthTarget);
                    context->Release();
                    InterlockedExchange(&g_doubleLeftFresh, 0);
                    const long n = InterlockedIncrement(&g_doubleComposes);
                    if (n == 1 || (n % 600) == 0)
                        Ol2Log("[OL2DOUBLE] Present assembled two captured primary eyes: frames=%ld", n);
                }
                if (device != nullptr) device->Release();
            }
            backbuffer->Release();
        }
    }

    static bool s_f2 = false;
    if (KeyWentDown(VK_F2, s_f2))
    {
        const long enabled = InterlockedCompareExchange(&g_bypassEnabled, 0, 0) ? 0 : 1;
        InterlockedExchange(&g_bypassEnabled, enabled);
        Ol2Log("[OL2BYPASS] direct right-half compositor bypass %s", enabled ? "ON" : "OFF");
    }
    const bool bypassDrawn = DrawRightFromFinalColor(swapChain);

    static bool s_f3 = false;
    if (KeyWentDown(VK_F3, s_f3))
    {
        const long enabled = InterlockedCompareExchange(&g_rightMirrorEnabled, 0, 0) ? 0 : 1;
        InterlockedExchange(&g_rightMirrorEnabled, enabled);
        Ol2Log("[OL2MIRROR] live-left to frozen-right MONO fallback %s", enabled ? "ON" : "OFF");
    }

    // Until the live final-color SRV is discovered, automatically retain the verified mono image.
    // F3 forces that fallback even after the bypass is running.
    if (swapChain != nullptr && Ol2Ue3::SideBySide() &&
        (InterlockedCompareExchange(&g_rightMirrorEnabled, 0, 0) != 0 || !bypassDrawn))
    {
        ID3D11Texture2D* backbuffer = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                           reinterpret_cast<void**>(&backbuffer))) &&
            backbuffer != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            backbuffer->GetDesc(&desc);
            const unsigned halfWidth = desc.Width / 2;

            if (g_rightMirrorScratch == nullptr || g_rightMirrorWidth != halfWidth ||
                g_rightMirrorHeight != desc.Height || g_rightMirrorFormat != desc.Format)
            {
                if (g_rightMirrorScratch != nullptr)
                {
                    g_rightMirrorScratch->Release();
                    g_rightMirrorScratch = nullptr;
                }

                ID3D11Device* device = nullptr;
                backbuffer->GetDevice(&device);
                if (device != nullptr)
                {
                    D3D11_TEXTURE2D_DESC scratchDesc = desc;
                    scratchDesc.Width = halfWidth;
                    scratchDesc.MipLevels = 1;
                    scratchDesc.ArraySize = 1;
                    scratchDesc.Usage = D3D11_USAGE_DEFAULT;
                    scratchDesc.BindFlags = 0;
                    scratchDesc.CPUAccessFlags = 0;
                    scratchDesc.MiscFlags = 0;
                    scratchDesc.SampleDesc.Count = 1;
                    scratchDesc.SampleDesc.Quality = 0;
                    if (SUCCEEDED(device->CreateTexture2D(&scratchDesc, nullptr, &g_rightMirrorScratch)))
                    {
                        g_rightMirrorWidth = halfWidth;
                        g_rightMirrorHeight = desc.Height;
                        g_rightMirrorFormat = desc.Format;
                        Ol2Log("[OL2MIRROR] fallback texture created %ux%u fmt=%u; F3 toggles it",
                               halfWidth, desc.Height, static_cast<unsigned>(desc.Format));
                    }
                    device->Release();
                }
            }

            if (g_rightMirrorScratch != nullptr)
            {
                ID3D11Device* device = nullptr;
                ID3D11DeviceContext* context = nullptr;
                backbuffer->GetDevice(&device);
                if (device != nullptr) device->GetImmediateContext(&context);
                if (context != nullptr)
                {
                    const D3D11_BOX leftBox = {0, 0, 0, halfWidth, desc.Height, 1};
                    const D3D11_BOX scratchBox = {0, 0, 0, halfWidth, desc.Height, 1};
                    context->CopySubresourceRegion(g_rightMirrorScratch, 0, 0, 0, 0,
                                                   backbuffer, 0, &leftBox);
                    context->CopySubresourceRegion(backbuffer, 0, halfWidth, 0, 0,
                                                   g_rightMirrorScratch, 0, &scratchBox);
                    const long mirrors = InterlockedIncrement(&g_rightMirrorFrames);
                    if (mirrors == 1 || (mirrors % 600) == 0)
                        Ol2Log("[OL2MIRROR] right half refreshed from live left at Present: frames=%ld", mirrors);
                    context->Release();
                }
                if (device != nullptr) device->Release();
            }
            backbuffer->Release();
        }
    }

    // Flat diagnostic mode: overwrite the game's broken final composite with the untouched
    // packed pair so left and right can be compared directly without a headset.
    if (!Ol2XrEnabled() && stereoConvertedThisFrame)
        CopyStereoToFlatBackbuffer(swapChain);

    // Last writer in flat mode: symmetric same-frame replay proof, if this frame produced one.
    Ol2Sidecar::OnPresent(swapChain, backbufferWidth, backbufferHeight);

    static bool s_f9 = false, s_f10 = false, s_f11 = false;
    static LONG s_frames = 0;

    if (KeyWentDown(VK_F9, s_f9))
    {
        g_dumpRequest = true;
        Ol2Log("[OL2CAM] F9 dump requested");
    }

    if (KeyWentDown(VK_F10, s_f10))
    {
        g_mode = (g_mode == 1) ? 0 : 1;
        g_modeStartTick = GetTickCount64();
        Ol2Log("[OL2CAM] F10 sway %s (amplitude %.0f uu)", g_mode == 1 ? "ON" : "OFF",
               static_cast<double>(g_swayAmplitudeUu));
    }

    static bool s_f7 = false, s_f8 = false, s_pgup = false, s_pgdn = false;

    static bool s_f5 = false, s_f6 = false;

    static bool s_f4 = false;
    if (KeyWentDown(VK_F4, s_f4))
    {
        const long v = InterlockedCompareExchange(&g_occlForce, 0, 0) ? 0 : 1;
        InterlockedExchange(&g_occlForce, v);
        Ol2Log("[OL2OCCL] occlusion force %s (reads=%ld forced=%ld)", v ? "ON" : "OFF",
               InterlockedCompareExchange(&g_occlReads, 0, 0),
               InterlockedCompareExchange(&g_occlForced, 0, 0),
               InterlockedCompareExchange(&g_vpLeftDraws, 0, 0),
               InterlockedCompareExchange(&g_vpRightDraws, 0, 0),
               InterlockedCompareExchange(&g_vpFullDraws, 0, 0),
               InterlockedCompareExchange(&g_vpSetCalls, 0, 0));
        Ol2Log("[OL2RT] eye draws by destination: backbuffer L=%ld R=%ld | offscreen L=%ld R=%ld | "
               "targetBinds=%ld currentTarget=0x%p %ux%u fmt=%u backbuffer=0x%p",
               InterlockedCompareExchange(&g_eyeDrawsToBackbufferL, 0, 0),
               InterlockedCompareExchange(&g_eyeDrawsToBackbufferR, 0, 0),
               InterlockedCompareExchange(&g_eyeDrawsOffscreenL, 0, 0),
               InterlockedCompareExchange(&g_eyeDrawsOffscreenR, 0, 0),
               InterlockedCompareExchange(&g_targetBinds, 0, 0),
               reinterpret_cast<void*>(g_boundTargetId), g_boundTargetW, g_boundTargetH,
               g_boundTargetFmt, reinterpret_cast<void*>(g_backbufferId));
    }

    if (KeyWentDown(VK_F5, s_f5)) Ol2Ue3::ToggleBuildOrder();
    if (KeyWentDown(VK_F6, s_f6)) Ol2Ue3::ToggleFovFill();

    if (KeyWentDown(VK_F7, s_f7))
    {
        g_headYawSign = -g_headYawSign;
        Ol2Log("[OL2HEAD] yaw sign flipped to %+.0f (use this if the world turns the wrong way)",
               static_cast<double>(g_headYawSign));
    }

    if (KeyWentDown(VK_F8, s_f8))
    {
        g_headEnabled = !g_headEnabled;
        Ol2Log("[OL2HEAD] head tracking %s", g_headEnabled ? "ON" : "OFF");
    }

    if (KeyWentDown(VK_PRIOR, s_pgup))
    {
        g_halfEyeUu += 0.4f;
        Ol2Log("[OL2AER] half-eye separation %.1f uu", static_cast<double>(g_halfEyeUu));
    }

    if (KeyWentDown(VK_NEXT, s_pgdn))
    {
        g_halfEyeUu -= 0.4f;
        if (g_halfEyeUu < 0.0f) g_halfEyeUu = 0.0f;
        Ol2Log("[OL2AER] half-eye separation %.1f uu", static_cast<double>(g_halfEyeUu));
    }

    if (KeyWentDown(VK_F11, s_f11))
    {
        g_mode = (g_mode == 2) ? 0 : 2;
        g_modeStartTick = GetTickCount64();
        Ol2Log("[OL2CAM] F11 static offset %s (%.1f uu)", g_mode == 2 ? "ON" : "OFF",
               static_cast<double>(g_staticOffsetUu));
    }

    const LONG frame = InterlockedIncrement(&s_frames);
    if (frame == 300 || (frame % 3000) == 0)
    {
        size_t vsBuffers = 0;
        {
            std::lock_guard<std::mutex> lock(g_slotMutex);
            vsBuffers = g_vsBoundBuffers.size();
        }

        // Is the patched context still patched, and is it still the one this swapchain's
        // device hands out? Separates "something overwrote the hook" from "wrong object" without guessing.
        // With inline hooks there is no vtable slot to verify. What matters now is simply
        // whether calls are arriving at all, which the counters below answer directly.
        const char* integrity = (InterlockedCompareExchange(&g_vsCbCalls, 0, 0) > 0) ? "calls-arriving" : "SILENT";

        Ol2Log("[OL2CAM] frame=%ld %ux%u ctxVtables=%ld devices=%ld deferred=%ld patch=%s | "
               "vsCbCalls=%ld vsCbs=%zu maps=%ld vpWrites=%ld vpPatches=%ld mode=%d | "
               "draw=%ld drawIdx=%ld drawIdxInst=%ld drawInst=%ld drawOther=%ld occlForced=%ld | "
               "DRAWS PER HALF: left=%ld right=%ld full=%ld (viewportSets=%ld)",
               frame, backbufferWidth, backbufferHeight,
               InterlockedCompareExchange(&g_contextHooked, 0, 0),
               InterlockedCompareExchange(&g_devicesSeen, 0, 0),
               InterlockedCompareExchange(&g_deferredContexts, 0, 0), integrity,
               InterlockedCompareExchange(&g_vsCbCalls, 0, 0), vsBuffers,
               InterlockedCompareExchange(&g_mapsSeen, 0, 0),
               InterlockedCompareExchange(&g_vpWrites, 0, 0),
               InterlockedCompareExchange(&g_vpPatches, 0, 0), g_mode,
               InterlockedCompareExchange(&g_draws, 0, 0),
               InterlockedCompareExchange(&g_drawIndexed, 0, 0),
               InterlockedCompareExchange(&g_drawIndexedInstanced, 0, 0),
               InterlockedCompareExchange(&g_drawInstanced, 0, 0),
               InterlockedCompareExchange(&g_drawOther, 0, 0),
               InterlockedCompareExchange(&g_occlForced, 0, 0),
               InterlockedCompareExchange(&g_vpLeftDraws, 0, 0),
               InterlockedCompareExchange(&g_vpRightDraws, 0, 0),
               InterlockedCompareExchange(&g_vpFullDraws, 0, 0),
               InterlockedCompareExchange(&g_vpSetCalls, 0, 0));
        Ol2Log("[OL2RT] eye draws by destination: backbuffer L=%ld R=%ld | offscreen L=%ld R=%ld | "
               "targetBinds=%ld currentTarget=0x%p %ux%u fmt=%u backbuffer=0x%p",
               InterlockedCompareExchange(&g_eyeDrawsToBackbufferL, 0, 0),
               InterlockedCompareExchange(&g_eyeDrawsToBackbufferR, 0, 0),
               InterlockedCompareExchange(&g_eyeDrawsOffscreenL, 0, 0),
               InterlockedCompareExchange(&g_eyeDrawsOffscreenR, 0, 0),
               InterlockedCompareExchange(&g_targetBinds, 0, 0),
               reinterpret_cast<void*>(g_boundTargetId), g_boundTargetW, g_boundTargetH,
               g_boundTargetFmt, reinterpret_cast<void*>(g_backbufferId));
    }
}
}
