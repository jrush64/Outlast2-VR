#pragma once

#include <cstdint>

// Outlast 2 engine-side stereo: the same-frame render, ported from Outlast 1's ol_ue3.cpp.
namespace Ol2Ue3
{
void Init();

// True once the same-frame pair is actually rendering, with a few frames of hysteresis so a
// loading screen does not flip the headset to mono and back.
bool Active();

// True while the backbuffer holds a side-by-side pair, so the XR submit takes halves.
bool SideBySide();

// Head orientation from the XR runtime, in unreal rotator units. Injected into the engine's own
// view rotation, so culling and occlusion follow the head instead of fighting it.
void SetHeadLook(int32_t yawUU, int32_t pitchUU, bool enabled);

// The FOV the engine ACTUALLY rendered, read off the built FSceneView every frame. Never
// computed from the ini: the game changes FOV when running and the camcorder zooms.
void GetEyeFov(float* fovXRad, float* fovYRad);

void AdjustEye(float deltaUu);
float HalfEyeUu();
void ToggleEyeSwap();
void ToggleBuildOrder();
void ToggleFovFill();
void FrameTick();
void SetEnabled(bool enabled);
bool Enabled();
}
