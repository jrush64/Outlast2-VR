#pragma once

#include <Windows.h>

struct IDXGISwapChain;
struct ID3D11Texture2D;

// Shared between the proxy/boot layer and the camera layer.
void Ol2Log(const char* format, ...);

// Settings, read from outlast2vr.ini next to the exe. Flat mode exists so the stereo render can be
// debugged on the monitor without anyone wearing a headset.
bool Ol2XrEnabled();
bool Ol2PatchVTable(void* object, size_t slot, void* hook, void** original);

namespace Ol2Camera
{
// Inline-hooks d3d11.dll's device entry points so every device is seen at birth, before it has
// rendered anything. Called from the boot thread, as early as the proxy loads.
void InstallDeviceExportHooks();

// Late path: whatever device owns the swapchain, in case it was created before the proxy loaded.
void InstallContextHooks(IDXGISwapChain* swapChain, const char* where);

// Called between the two complete primary Draw passes. Safely unbinds the backbuffer, captures
// the live left-position output, then restores every render target before pass two begins.
bool CapturePrimaryHalf();

// Called every frame from inside the Present hook, i.e. on the render thread.
void OnPresent(IDXGISwapChain* swapChain, unsigned backbufferWidth, unsigned backbufferHeight);

// Returns the newest same-frame packed stereo image captured before Outlast's final compositor.
// The caller owns one reference and must release it. False means use the stable mono fallback.
bool AcquireStereoFrame(ID3D11Texture2D** texture, unsigned* width, unsigned* height,
                        unsigned long long* serial);
}
