#pragma once

// Shims for the Mass Effect VR modules that the OpenXR core (ol2_xr.cpp) was written against, so the OpenXR core
// lifts across with no edits to its 700 lines of session/frame/submit logic.
//
// Kept deliberately thin:
//   Log            -> the Outlast 2 logger
//   RenderViewHook -> head look is published to the camera layer, which applies it to the
//                     ViewProjection. Outlast 2 has no CalcSceneView hook to write into.
//   DrawDouble     -> stubbed off. Per-draw doubling is not used; eye sources come from
//                     ol2_xr.cpp's eye caches (stereo pair or mono backbuffer).
//   VsStereo       -> stubbed. Counters exist only to satisfy a status log line.

#include <Windows.h>

#include <cstdint>
#include <string>

#include "ol2_common.h"
#include "ol2_ue3.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace ME1VR::Log
{
inline void Line(const std::string& message) noexcept { Ol2Log("%s", message.c_str()); }
inline void WindowsError(const char* context, DWORD error) noexcept
{
    Ol2Log("%s (win32 error %lu)", context != nullptr ? context : "<null>", error);
}
}

namespace ME1VR::RenderViewHook
{
// Head look in unreal rotator units. With the engine hooks in, this goes straight into the game's
// own view rotation, so culling and occlusion follow the head instead of fighting it.
inline void SetHeadLook(int32_t yawUU, int32_t pitchUU, bool enabled) noexcept
{
    Ol2Ue3::SetHeadLook(yawUU, pitchUU, enabled);
}
}

namespace ME1VR::DrawDouble
{
inline bool Enabled() noexcept { return false; }
inline bool Installed() noexcept { return false; }
inline ID3D11Texture2D* EyeColorTexture(int) noexcept { return nullptr; }
inline void ClearEyeTargets() noexcept {}
inline void SetEnabled(bool) noexcept {}
inline bool Install(ID3D11Device*, ID3D11DeviceContext*) noexcept { return false; }
inline uint64_t WorldDrawsDoubled() noexcept { return 0; }
inline uint64_t DrawsSeen() noexcept { return 0; }
inline uint64_t DoublingFailures() noexcept { return 0; }
}

namespace ME1VR::VsStereo
{
inline bool Install(ID3D11Device*) noexcept { return false; }
inline bool Installed() noexcept { return false; }
inline uint64_t TotalShadersSeen() noexcept { return 0; }
inline uint64_t WorldShadersClassified() noexcept { return 0; }
inline uint64_t ShearedVariantsCompiled() noexcept { return 0; }
}
