#pragma once

#include <Windows.h>
#include <d3d11.h>

struct IDXGISwapChain;

namespace Ol2Sidecar
{
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);

// True only while the proof renderer is issuing its own D3D11 calls.
bool Active();

// Records the final CPU image of a constant buffer after the normal camera hook has patched it.
void ObserveConstantBuffer(ID3D11Resource* resource, const void* data, UINT bytes);

// Replays one qualifying opaque world draw into two independent origin-zero targets.
bool ReplayDrawIndexed(ID3D11DeviceContext* context, UINT indexCount,
                       UINT startIndexLocation, INT baseVertexLocation,
                       DrawIndexedFn original);

// Composes the two independent targets into one packed R8 SBS frame at Present.
void OnPresent(IDXGISwapChain* swapChain, unsigned backbufferWidth,
               unsigned backbufferHeight);

// Returns the newest packed proof frame. The caller owns one reference.
bool AcquireStereoFrame(ID3D11Texture2D** texture, unsigned* width, unsigned* height,
                        unsigned long long* serial);
}