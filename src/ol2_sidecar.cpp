// Outlast 2 VR - symmetric same-frame draw-replay proof.
//
// The rejected engine split rendered the right eye through a nonzero half-width viewport origin.
// This path never does that. Qualifying opaque world draws are issued twice more, into two
// independent full-size color/depth targets. Both eye viewports begin at (0,0).

#include "ol2_sidecar.h"

#include <d3dcompiler.h>
#include <dxgi.h>

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ol2_common.h"

namespace
{
template <typename T>
void ReleaseCom(T*& value)
{
    if (value != nullptr) { value->Release(); value = nullptr; }
}

struct VpFacts
{
    float right[3] = {};
};

bool LooksLikeViewProjection(const float* m, VpFacts* facts)
{
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    if (std::fabs(m[15]) > 0.001f) return false;

    const float fx = m[3], fy = m[7], fz = m[11];
    const float fwdLen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fwdLen < 0.95f || fwdLen > 1.05f) return false;

    const float rx = m[0], ry = m[4], rz = m[8];
    const float p00 = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (p00 < 0.4f || p00 > 5.0f) return false;

    const float ux = m[1], uy = m[5], uz = m[9];
    const float p11 = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (p11 < 0.4f || p11 > 6.0f) return false;

    const float dotRF = (rx * fx + ry * fy + rz * fz) / p00;
    const float dotUF = (ux * fx + uy * fy + uz * fz) / p11;
    const float dotRU = (rx * ux + ry * uy + rz * uz) / (p00 * p11);
    if (std::fabs(dotRF) > 0.03f || std::fabs(dotUF) > 0.03f ||
        std::fabs(dotRU) > 0.03f) return false;

    if (facts != nullptr)
    {
        facts->right[0] = rx / p00;
        facts->right[1] = ry / p00;
        facts->right[2] = rz / p00;
    }
    return true;
}

void TranslateCamera(float* m, const float d[3])
{
    for (int c = 0; c < 4; ++c)
        m[12 + c] -= d[0] * m[0 + c] + d[1] * m[4 + c] + d[2] * m[8 + c];
}

bool PatchEye(std::vector<unsigned char>& bytes, float eyeShift)
{
    if (bytes.size() < 64) return false;
    bool patched = false;
    float* base = reinterpret_cast<float*>(bytes.data());
    const UINT floats = static_cast<UINT>(bytes.size() / sizeof(float));
    for (UINT offset = 0; offset + 16 <= floats; offset += 4)
    {
        VpFacts facts = {};
        float* m = base + offset;
        if (!LooksLikeViewProjection(m, &facts)) continue;
        const float d[3] = {
            facts.right[0] * eyeShift,
            facts.right[1] * eyeShift,
            facts.right[2] * eyeShift
        };
        TranslateCamera(m, d);
        patched = true;
    }
    return patched;
}

struct BufferRecord
{
    std::vector<unsigned char> bytes;
};

struct ShadowPair
{
    ID3D11Buffer* original = nullptr;
    ID3D11Buffer* left = nullptr;
    ID3D11Buffer* right = nullptr;
    UINT byteWidth = 0;
};

std::mutex g_bufferMutex;
std::unordered_map<ID3D11Buffer*, BufferRecord> g_bufferRecords;
std::unordered_map<ID3D11Buffer*, ShadowPair> g_shadowPairs;

volatile LONG g_active = 0;
volatile LONG g_sidecarFresh = 0;
volatile LONG64 g_presentSerial = 0;
volatile LONG64 g_clearSerial = -1;
volatile LONG64 g_pairSerial = 0;
volatile LONG g_replayedDraws = 0;
unsigned g_backbufferWidth = 0;
unsigned g_backbufferHeight = 0;

ID3D11Texture2D* g_leftColor = nullptr;
ID3D11RenderTargetView* g_leftRtv = nullptr;
ID3D11ShaderResourceView* g_leftSrv = nullptr;
ID3D11Texture2D* g_leftDepth = nullptr;
ID3D11DepthStencilView* g_leftDsv = nullptr;
ID3D11Texture2D* g_rightColor = nullptr;
ID3D11RenderTargetView* g_rightRtv = nullptr;
ID3D11ShaderResourceView* g_rightSrv = nullptr;
ID3D11Texture2D* g_rightDepth = nullptr;
ID3D11DepthStencilView* g_rightDsv = nullptr;
D3D11_TEXTURE2D_DESC g_colorDesc = {};
D3D11_TEXTURE2D_DESC g_depthDesc = {};
D3D11_DEPTH_STENCIL_VIEW_DESC g_dsvDesc = {};
bool g_targetsValid = false;

ID3D11Texture2D* g_packed = nullptr;
ID3D11RenderTargetView* g_packedRtv = nullptr;
ID3D11ShaderResourceView* g_packedSrv = nullptr;
unsigned g_packedWidth = 0;
unsigned g_packedHeight = 0;
volatile LONG g_packedFresh = 0;
volatile LONG64 g_packedSerial = 0;

ID3D11VertexShader* g_copyVs = nullptr;
ID3D11PixelShader* g_copyPs = nullptr;
ID3D11SamplerState* g_copySampler = nullptr;
ID3D11RasterizerState* g_copyRaster = nullptr;
ID3D11BlendState* g_copyBlend = nullptr;
ID3D11DepthStencilState* g_copyDepth = nullptr;

bool Near(unsigned a, unsigned b, unsigned tolerance)
{
    return a > b ? a - b <= tolerance : b - a <= tolerance;
}

bool GetTextureDesc(ID3D11View* view, D3D11_TEXTURE2D_DESC* desc)
{
    if (view == nullptr || desc == nullptr) return false;
    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* texture = nullptr;
    view->GetResource(&resource);
    if (resource != nullptr)
    {
        resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                 reinterpret_cast<void**>(&texture));
        resource->Release();
    }
    if (texture == nullptr) return false;
    texture->GetDesc(desc);
    texture->Release();
    return true;
}

bool SameTargetDesc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b)
{
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality;
}

void ReleaseTargets()
{
    ReleaseCom(g_leftDsv); ReleaseCom(g_leftDepth);
    ReleaseCom(g_leftSrv); ReleaseCom(g_leftRtv); ReleaseCom(g_leftColor);
    ReleaseCom(g_rightDsv); ReleaseCom(g_rightDepth);
    ReleaseCom(g_rightSrv); ReleaseCom(g_rightRtv); ReleaseCom(g_rightColor);
    g_targetsValid = false;
}

bool CreateEyeTarget(ID3D11Device* device,
                     const D3D11_TEXTURE2D_DESC& colorDesc,
                     const D3D11_TEXTURE2D_DESC& depthDesc,
                     const D3D11_DEPTH_STENCIL_VIEW_DESC& dsvDesc,
                     ID3D11Texture2D** color,
                     ID3D11RenderTargetView** rtv,
                     ID3D11ShaderResourceView** srv,
                     ID3D11Texture2D** depth,
                     ID3D11DepthStencilView** dsv)
{
    HRESULT hr = device->CreateTexture2D(&colorDesc, nullptr, color);
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(*color, nullptr, rtv);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(*color, nullptr, srv);
    if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&depthDesc, nullptr, depth);
    if (SUCCEEDED(hr)) hr = device->CreateDepthStencilView(*depth, &dsvDesc, dsv);
    return SUCCEEDED(hr) && *color != nullptr && *rtv != nullptr && *srv != nullptr &&
           *depth != nullptr && *dsv != nullptr;
}

bool EnsureTargets(ID3D11Device* device,
                   const D3D11_TEXTURE2D_DESC& sourceColor,
                   const D3D11_TEXTURE2D_DESC& sourceDepth,
                   const D3D11_DEPTH_STENCIL_VIEW_DESC& sourceDsv)
{
    D3D11_TEXTURE2D_DESC color = sourceColor;
    color.MipLevels = 1;
    color.ArraySize = 1;
    color.Usage = D3D11_USAGE_DEFAULT;
    color.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    color.CPUAccessFlags = 0;
    color.MiscFlags = 0;

    D3D11_TEXTURE2D_DESC depth = sourceDepth;
    depth.MipLevels = 1;
    depth.ArraySize = 1;
    depth.Usage = D3D11_USAGE_DEFAULT;
    depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depth.CPUAccessFlags = 0;
    depth.MiscFlags = 0;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsv = sourceDsv;
    if (dsv.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D)
        return false;
    dsv.Texture2D.MipSlice = 0;

    if (g_targetsValid && SameTargetDesc(g_colorDesc, color) &&
        SameTargetDesc(g_depthDesc, depth) && g_dsvDesc.Format == dsv.Format)
        return true;

    ReleaseTargets();
    if (!CreateEyeTarget(device, color, depth, dsv, &g_leftColor, &g_leftRtv,
                         &g_leftSrv, &g_leftDepth, &g_leftDsv) ||
        !CreateEyeTarget(device, color, depth, dsv, &g_rightColor, &g_rightRtv,
                         &g_rightSrv, &g_rightDepth, &g_rightDsv))
    {
        ReleaseTargets();
        return false;
    }

    g_colorDesc = color;
    g_depthDesc = depth;
    g_dsvDesc = dsv;
    g_targetsValid = true;
    InterlockedExchange64(&g_clearSerial, -1);
    Ol2Log("[OL2SIDECAR] created two independent origin-zero targets: %ux%u fmt=%u depth=%u",
           color.Width, color.Height, static_cast<unsigned>(color.Format),
           static_cast<unsigned>(dsv.Format));
    return true;
}

bool GetRecordedCamera(ID3D11Buffer* const* buffers, UINT count, UINT* cameraSlot,
                       ID3D11Buffer** cameraBuffer, std::vector<unsigned char>* bytes)
{
    if (buffers == nullptr || cameraSlot == nullptr || cameraBuffer == nullptr || bytes == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    for (UINT slot = 0; slot < count; ++slot)
    {
        auto it = g_bufferRecords.find(buffers[slot]);
        if (buffers[slot] != nullptr && it != g_bufferRecords.end())
        {
            *cameraSlot = slot;
            *cameraBuffer = buffers[slot];
            *bytes = it->second.bytes;
            return true;
        }
    }
    return false;
}

bool EnsureShadows(ID3D11Device* device, ID3D11Buffer* original, UINT bytes,
                   ID3D11Buffer** left, ID3D11Buffer** right)
{
    if (device == nullptr || original == nullptr || left == nullptr || right == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    auto it = g_shadowPairs.find(original);
    if (it != g_shadowPairs.end() && it->second.left != nullptr &&
        it->second.right != nullptr && it->second.byteWidth == bytes)
    {
        *left = it->second.left;
        *right = it->second.right;
        return true;
    }

    D3D11_BUFFER_DESC desc = {};
    original->GetDesc(&desc);
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;

    ShadowPair pair = {};
    pair.original = original;
    pair.byteWidth = bytes;
    HRESULT hr = device->CreateBuffer(&desc, nullptr, &pair.left);
    if (SUCCEEDED(hr)) hr = device->CreateBuffer(&desc, nullptr, &pair.right);
    if (FAILED(hr) || pair.left == nullptr || pair.right == nullptr)
    {
        ReleaseCom(pair.left);
        ReleaseCom(pair.right);
        return false;
    }
    original->AddRef();
    auto old = g_shadowPairs.find(original);
    if (old != g_shadowPairs.end())
    {
        ReleaseCom(old->second.left);
        ReleaseCom(old->second.right);
        ReleaseCom(old->second.original);
        old->second = pair;
    }
    else
    {
        g_shadowPairs.emplace(original, pair);
    }
    *left = pair.left;
    *right = pair.right;
    return true;
}

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
                                      const D3D_SHADER_MACRO*, ID3DInclude*,
                                      LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);

bool EnsureCopyPipeline(ID3D11Device* device)
{
    if (g_copyVs != nullptr && g_copyPs != nullptr && g_copySampler != nullptr &&
        g_copyRaster != nullptr && g_copyBlend != nullptr && g_copyDepth != nullptr)
        return true;

    HMODULE compilerModule = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compilerModule == nullptr) return false;
    auto compile = reinterpret_cast<D3DCompileFn>(
        GetProcAddress(compilerModule, "D3DCompile"));
    if (compile == nullptr) { FreeLibrary(compilerModule); return false; }

    static constexpr char vsCode[] =
        "struct O{float4 p:SV_POSITION;float2 u:TEXCOORD0;};"
        "O main(uint i:SV_VertexID){O o;float2 u=float2((i<<1)&2,i&2);"
        "o.p=float4(u*float2(2,-2)+float2(-1,1),0,1);o.u=u;return o;}";
    static constexpr char psCode[] =
        "Texture2D s:register(t0);SamplerState q:register(s0);"
        "float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_TARGET"
        "{return s.Sample(q,u);}";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(vsCode, sizeof(vsCode) - 1, "OL2SidecarVS", nullptr,
                         nullptr, "main", "vs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errors);
    ReleaseCom(errors);
    if (SUCCEEDED(hr))
        hr = compile(psCode, sizeof(psCode) - 1, "OL2SidecarPS", nullptr,
                     nullptr, "main", "ps_5_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors);
    ReleaseCom(errors);
    if (SUCCEEDED(hr))
        hr = device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                        vsBlob->GetBufferSize(), nullptr, &g_copyVs);
    if (SUCCEEDED(hr))
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(),
                                       psBlob->GetBufferSize(), nullptr, &g_copyPs);
    ReleaseCom(vsBlob); ReleaseCom(psBlob); FreeLibrary(compilerModule);

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sampler, &g_copySampler);

    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&raster, &g_copyRaster);

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = device->CreateBlendState(&blend, &g_copyBlend);

    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    if (SUCCEEDED(hr)) hr = device->CreateDepthStencilState(&depth, &g_copyDepth);

    return SUCCEEDED(hr) && g_copyVs != nullptr && g_copyPs != nullptr &&
           g_copySampler != nullptr && g_copyRaster != nullptr &&
           g_copyBlend != nullptr && g_copyDepth != nullptr;
}

bool EnsurePacked(ID3D11Device* device, ID3D11Texture2D* backbuffer)
{
    D3D11_TEXTURE2D_DESC desc = {};
    backbuffer->GetDesc(&desc);
    if (g_packed != nullptr && g_packedRtv != nullptr && g_packedSrv != nullptr &&
        g_packedWidth == desc.Width && g_packedHeight == desc.Height)
        return true;

    ReleaseCom(g_packedSrv); ReleaseCom(g_packedRtv); ReleaseCom(g_packed);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_packed);
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(g_packed, nullptr, &g_packedRtv);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(g_packed, nullptr, &g_packedSrv);
    if (FAILED(hr)) return false;
    g_packedWidth = desc.Width;
    g_packedHeight = desc.Height;
    Ol2Log("[OL2SIDECAR] packed flat proof target created %ux%u", desc.Width, desc.Height);
    return true;
}

bool ComposePair(IDXGISwapChain* swapChain)
{
    if (swapChain == nullptr || !g_targetsValid || g_leftSrv == nullptr || g_rightSrv == nullptr)
        return false;

    ID3D11Texture2D* backbuffer = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backbuffer))) ||
        backbuffer == nullptr) return false;
    backbuffer->GetDevice(&device);
    if (device != nullptr) device->GetImmediateContext(&context);
    if (device == nullptr || context == nullptr || !EnsureCopyPipeline(device) ||
        !EnsurePacked(device, backbuffer))
    {
        ReleaseCom(context); ReleaseCom(device); ReleaseCom(backbuffer);
        return false;
    }

    ID3D11InputLayout* oldLayout = nullptr;
    ID3D11Buffer* oldVb = nullptr; UINT oldStride = 0, oldOffset = 0;
    ID3D11Buffer* oldIb = nullptr; DXGI_FORMAT oldIf = DXGI_FORMAT_UNKNOWN; UINT oldIo = 0;
    D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* oldVs = nullptr; ID3D11HullShader* oldHs = nullptr;
    ID3D11DomainShader* oldDs = nullptr; ID3D11GeometryShader* oldGs = nullptr;
    ID3D11PixelShader* oldPs = nullptr;
    ID3D11ShaderResourceView* oldSrv = nullptr; ID3D11SamplerState* oldSampler = nullptr;
    ID3D11RenderTargetView* oldRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    ID3D11RasterizerState* oldRaster = nullptr; ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldBlendFactor[4] = {}; UINT oldMask = 0;
    ID3D11DepthStencilState* oldDepth = nullptr; UINT oldStencil = 0;
    D3D11_VIEWPORT oldVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT oldVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    context->IAGetInputLayout(&oldLayout);
    context->IAGetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOffset);
    context->IAGetIndexBuffer(&oldIb, &oldIf, &oldIo);
    context->IAGetPrimitiveTopology(&oldTopology);
    context->VSGetShader(&oldVs, nullptr, nullptr); context->HSGetShader(&oldHs, nullptr, nullptr);
    context->DSGetShader(&oldDs, nullptr, nullptr); context->GSGetShader(&oldGs, nullptr, nullptr);
    context->PSGetShader(&oldPs, nullptr, nullptr);
    context->PSGetShaderResources(0, 1, &oldSrv); context->PSGetSamplers(0, 1, &oldSampler);
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, &oldDsv);
    context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldMask);
    context->OMGetDepthStencilState(&oldDepth, &oldStencil);
    context->RSGetState(&oldRaster); context->RSGetViewports(&oldVpCount, oldVps);

    const FLOAT black[4] = {};
    const FLOAT blendFactor[4] = {};
    UINT zero = 0;
    ID3D11Buffer* noBuffer = nullptr;
    ID3D11ShaderResourceView* noSrv = nullptr;
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(g_packedWidth / 2);
    vp.Height = static_cast<float>(g_packedHeight);
    vp.MaxDepth = 1.0f;

    InterlockedExchange(&g_active, 1);
    context->OMSetRenderTargets(1, &g_packedRtv, nullptr);
    context->ClearRenderTargetView(g_packedRtv, black);
    context->OMSetBlendState(g_copyBlend, blendFactor, 0xffffffffu);
    context->OMSetDepthStencilState(g_copyDepth, 0);
    context->RSSetState(g_copyRaster);
    context->IASetInputLayout(nullptr);
    context->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_copyVs, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0); context->DSSetShader(nullptr, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0); context->PSSetShader(g_copyPs, nullptr, 0);
    context->PSSetSamplers(0, 1, &g_copySampler);

    context->RSSetViewports(1, &vp);
    context->PSSetShaderResources(0, 1, &g_leftSrv);
    context->Draw(3, 0);
    vp.TopLeftX = static_cast<float>(g_packedWidth / 2);
    vp.Width = static_cast<float>(g_packedWidth - g_packedWidth / 2);
    context->RSSetViewports(1, &vp);
    context->PSSetShaderResources(0, 1, &g_rightSrv);
    context->Draw(3, 0);
    context->PSSetShaderResources(0, 1, &noSrv);

    // Flat mode gets the packed proof directly. XR consumes the same texture below.
    if (!Ol2XrEnabled())
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(backbuffer, g_packed);
    }

    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, oldDsv);
    context->OMSetBlendState(oldBlend, oldBlendFactor, oldMask);
    context->OMSetDepthStencilState(oldDepth, oldStencil);
    context->RSSetState(oldRaster);
    if (oldVpCount > 0) context->RSSetViewports(oldVpCount, oldVps);
    context->IASetInputLayout(oldLayout);
    context->IASetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOffset);
    context->IASetIndexBuffer(oldIb, oldIf, oldIo);
    context->IASetPrimitiveTopology(oldTopology);
    context->VSSetShader(oldVs, nullptr, 0); context->HSSetShader(oldHs, nullptr, 0);
    context->DSSetShader(oldDs, nullptr, 0); context->GSSetShader(oldGs, nullptr, 0);
    context->PSSetShader(oldPs, nullptr, 0);
    context->PSSetShaderResources(0, 1, &oldSrv);
    context->PSSetSamplers(0, 1, &oldSampler);
    InterlockedExchange(&g_active, 0);

    ReleaseCom(oldLayout); ReleaseCom(oldVb); ReleaseCom(oldIb);
    ReleaseCom(oldVs); ReleaseCom(oldHs); ReleaseCom(oldDs); ReleaseCom(oldGs); ReleaseCom(oldPs);
    ReleaseCom(oldSrv); ReleaseCom(oldSampler);
    for (auto& rtv : oldRtvs) ReleaseCom(rtv);
    ReleaseCom(oldDsv); ReleaseCom(oldRaster); ReleaseCom(oldBlend); ReleaseCom(oldDepth);
    ReleaseCom(context); ReleaseCom(device); ReleaseCom(backbuffer);

    const LONG64 serial = InterlockedIncrement64(&g_pairSerial);
    InterlockedExchange64(&g_packedSerial, serial);
    InterlockedExchange(&g_packedFresh, 1);
    if (serial == 1 || (serial % 600) == 0)
        Ol2Log("[OL2SIDECAR] symmetric origin-zero pair composed: serial=%lld draws=%ld",
               serial, InterlockedCompareExchange(&g_replayedDraws, 0, 0));
    return true;
}
}

namespace Ol2Sidecar
{
bool Active()
{
    return InterlockedCompareExchange(&g_active, 0, 0) != 0;
}

void ObserveConstantBuffer(ID3D11Resource* resource, const void* data, UINT bytes)
{
    if (Active() || resource == nullptr || data == nullptr || bytes < 64 || bytes > 8192)
        return;

    const float* base = static_cast<const float*>(data);
    const UINT floats = bytes / sizeof(float);
    bool camera = false;
    for (UINT offset = 0; offset + 16 <= floats; offset += 4)
    {
        if (LooksLikeViewProjection(base + offset, nullptr))
        {
            camera = true;
            break;
        }
    }
    if (!camera) return;

    ID3D11Buffer* buffer = reinterpret_cast<ID3D11Buffer*>(resource);
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    BufferRecord& record = g_bufferRecords[buffer];
    record.bytes.resize(bytes);
    memcpy(record.bytes.data(), data, bytes);
}

bool ReplayDrawIndexed(ID3D11DeviceContext* context, UINT indexCount,
                       UINT startIndexLocation, INT baseVertexLocation,
                       DrawIndexedFn original)
{
    if (context == nullptr || original == nullptr || Active() ||
        g_backbufferWidth == 0 || g_backbufferHeight == 0)
        return false;

    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    if (rtvs[0] == nullptr || dsv == nullptr)
    {
        for (auto& rtv : rtvs) ReleaseCom(rtv);
        ReleaseCom(dsv);
        return false;
    }

    D3D11_TEXTURE2D_DESC colorDesc = {}, depthDesc = {};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    context->RSGetViewports(&vpCount, viewports);
    const bool validTarget =
        GetTextureDesc(rtvs[0], &colorDesc) && GetTextureDesc(dsv, &depthDesc) &&
        colorDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        Near(colorDesc.Width, g_backbufferWidth, 32) &&
        Near(colorDesc.Height, g_backbufferHeight, 32) &&
        vpCount > 0 && viewports[0].TopLeftX < 1.0f && viewports[0].TopLeftY < 1.0f &&
        Near(static_cast<unsigned>(viewports[0].Width), colorDesc.Width, 8) &&
        Near(static_cast<unsigned>(viewports[0].Height), colorDesc.Height, 8);
    if (!validTarget)
    {
        for (auto& rtv : rtvs) ReleaseCom(rtv);
        ReleaseCom(dsv);
        return false;
    }
    dsv->GetDesc(&dsvDesc);

    ID3D11DepthStencilState* depthState = nullptr;
    UINT stencilRef = 0;
    context->OMGetDepthStencilState(&depthState, &stencilRef);
    D3D11_DEPTH_STENCIL_DESC depthStateDesc = {};
    if (depthState != nullptr) depthState->GetDesc(&depthStateDesc);
    const bool opaqueDepth = depthState != nullptr && depthStateDesc.DepthEnable &&
                             depthStateDesc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
    if (!opaqueDepth)
    {
        ReleaseCom(depthState);
        for (auto& rtv : rtvs) ReleaseCom(rtv);
        ReleaseCom(dsv);
        return false;
    }

    ID3D11Buffer* vsBuffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    context->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, vsBuffers);
    UINT cameraSlot = 0;
    ID3D11Buffer* cameraBuffer = nullptr;
    std::vector<unsigned char> centered;
    if (!GetRecordedCamera(vsBuffers, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT,
                           &cameraSlot, &cameraBuffer, &centered))
    {
        for (auto& b : vsBuffers) ReleaseCom(b);
        ReleaseCom(depthState);
        for (auto& rtv : rtvs) ReleaseCom(rtv);
        ReleaseCom(dsv);
        return false;
    }

    std::vector<unsigned char> leftBytes = centered;
    std::vector<unsigned char> rightBytes = centered;
    constexpr float kHalfEyeUu = 3.2f;
    if (!PatchEye(leftBytes, -kHalfEyeUu) || !PatchEye(rightBytes, kHalfEyeUu))
    {
        for (auto& b : vsBuffers) ReleaseCom(b);
        ReleaseCom(depthState);
        for (auto& rtv : rtvs) ReleaseCom(rtv);
        ReleaseCom(dsv);
        return false;
    }

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    ID3D11Buffer* leftCb = nullptr;
    ID3D11Buffer* rightCb = nullptr;
    if (device == nullptr || !EnsureTargets(device, colorDesc, depthDesc, dsvDesc) ||
        !EnsureShadows(device, cameraBuffer, static_cast<UINT>(centered.size()),
                       &leftCb, &rightCb))
    {
        ReleaseCom(device);
        for (auto& b : vsBuffers) ReleaseCom(b);
        ReleaseCom(depthState);
        for (auto& rtv : rtvs) ReleaseCom(rtv);
        ReleaseCom(dsv);
        return false;
    }

    ID3D11Predicate* predicate = nullptr;
    BOOL predicateValue = FALSE;
    context->GetPredication(&predicate, &predicateValue);

    const LONG64 present = InterlockedCompareExchange64(&g_presentSerial, 0, 0);
    InterlockedExchange(&g_active, 1);
    if (InterlockedCompareExchange64(&g_clearSerial, present, present) != present)
    {
        const FLOAT black[4] = {};
        context->ClearRenderTargetView(g_leftRtv, black);
        context->ClearDepthStencilView(g_leftDsv,
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        context->ClearRenderTargetView(g_rightRtv, black);
        context->ClearDepthStencilView(g_rightDsv,
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        InterlockedExchange64(&g_clearSerial, present);
        InterlockedExchange(&g_replayedDraws, 0);
    }

    D3D11_VIEWPORT eyeVp = {};
    eyeVp.Width = static_cast<float>(colorDesc.Width);
    eyeVp.Height = static_cast<float>(colorDesc.Height);
    eyeVp.MinDepth = viewports[0].MinDepth;
    eyeVp.MaxDepth = viewports[0].MaxDepth;
    context->SetPredication(nullptr, FALSE);
    context->RSSetViewports(1, &eyeVp);

    context->UpdateSubresource(leftCb, 0, nullptr, leftBytes.data(), 0, 0);
    context->VSSetConstantBuffers(cameraSlot, 1, &leftCb);
    context->OMSetRenderTargets(1, &g_leftRtv, g_leftDsv);
    original(context, indexCount, startIndexLocation, baseVertexLocation);

    context->UpdateSubresource(rightCb, 0, nullptr, rightBytes.data(), 0, 0);
    context->VSSetConstantBuffers(cameraSlot, 1, &rightCb);
    context->OMSetRenderTargets(1, &g_rightRtv, g_rightDsv);
    original(context, indexCount, startIndexLocation, baseVertexLocation);

    context->VSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, vsBuffers);
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    if (vpCount > 0) context->RSSetViewports(vpCount, viewports);
    context->SetPredication(predicate, predicateValue);
    InterlockedExchange(&g_active, 0);

    InterlockedIncrement(&g_replayedDraws);
    InterlockedExchange(&g_sidecarFresh, 1);

    ReleaseCom(predicate);
    ReleaseCom(device);
    for (auto& b : vsBuffers) ReleaseCom(b);
    ReleaseCom(depthState);
    for (auto& rtv : rtvs) ReleaseCom(rtv);
    ReleaseCom(dsv);
    return true;
}

void OnPresent(IDXGISwapChain* swapChain, unsigned backbufferWidth,
               unsigned backbufferHeight)
{
    g_backbufferWidth = backbufferWidth;
    g_backbufferHeight = backbufferHeight;
    if (InterlockedExchange(&g_sidecarFresh, 0) != 0)
        ComposePair(swapChain);
    InterlockedIncrement64(&g_presentSerial);
}

bool AcquireStereoFrame(ID3D11Texture2D** texture, unsigned* width, unsigned* height,
                        unsigned long long* serial)
{
    if (texture == nullptr) return false;
    *texture = nullptr;
    if (InterlockedCompareExchange(&g_packedFresh, 0, 0) == 0 || g_packed == nullptr)
        return false;
    g_packed->AddRef();
    *texture = g_packed;
    if (width != nullptr) *width = g_packedWidth;
    if (height != nullptr) *height = g_packedHeight;
    if (serial != nullptr)
        *serial = static_cast<unsigned long long>(
            InterlockedCompareExchange64(&g_packedSerial, 0, 0));
    InterlockedExchange(&g_packedFresh, 0);
    return true;
}
}