#pragma once

#include <cstdint>

struct ID3D11Device;
struct IDXGISwapChain;

namespace Ol2Xr
{
void TryCreateSession(ID3D11Device* device, int64_t colorFormat, uint32_t width, uint32_t height) noexcept;
void OnPresent(IDXGISwapChain* gameSwapChain) noexcept;
void RequestShutdown() noexcept;
void SetGameSwapChain(IDXGISwapChain* swapChain) noexcept;

// Legacy eye index retained for guarded experiments. Stable mono keeps it at zero.
int NextEye() noexcept;
bool SessionReady() noexcept;

// Same-frame experimental capture entry point. All calls must remain on the render thread because
// Outlast 2 creates its D3D11 device with D3D11_CREATE_DEVICE_SINGLETHREADED.
bool CaptureLeftEye() noexcept;

// Runtime per-eye FOV in radians. The render and submitted projection must match exactly.
bool RuntimeEyeFov(float* horizontalRadians, float* verticalRadians) noexcept;
bool RuntimeEyeFovH(float* radians) noexcept;
}