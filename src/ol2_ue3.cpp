// Outlast 2 engine-side stereo - SAME-FRAME rendering.
//
// STATUS: the half-viewport split described below is DISABLED (g_enabled = 0). It was tested and
// rejected: the right half rendered through a nonzero viewport origin came out dark, streaked or
// stale. What remains active in this file is the CalcSceneView/GetFovAngle hooks used for head
// look, FOV fill and the rendered-FOV readback. See docs/TECHNICAL_NOTES.md.
//
// Ported from the Outlast (1) VR mod's ol_ue3.cpp, which shipped this exact mechanism on
// Outlast 1. Only the addresses and struct offsets are new; the shape is unchanged because it is
// the same engine four years on.
//
// THE MECHANISM: the half-viewport split, which is what Outlast 1 actually SHIPPED (Mode=split in
// both its runtime and release ini).
//
// UE3 builds a view each frame in ULocalPlayer::CalcSceneView, reading the local player's
// normalised viewport rect to decide where on the backbuffer to render. Split-screen co-op uses
// exactly this. With a single player the split exploits it: rewrite the rect to the LEFT half, let the
// original build a view, rewrite to the RIGHT half, call it again for a second view, restore.
// The engine renders both into one side-by-side backbuffer, in the same frame, each with its own
// culling and post. The engine gets the left view as the primary return.
//
// A first port of this file used ol_ue3.cpp's OTHER mode instead - Mode=sfr, which calls
// UGameViewportClient::Draw twice per frame. That crashed Outlast 2 inside nvwgf2umx.dll with an
// access violation after about 300 frames, and the mid-Draw backbuffer copy it needs is the prime
// suspect: reading the backbuffer while the engine still has it bound as a render target. The
// split needs no second Draw and no mid-frame copy, which removes that whole class of problem.
//
// ADDRESSES - Outlast 1 (verified live) -> Outlast 2 (Ghidra 2026-08-21)
//   ULocalPlayer::CalcSceneView       0x62A020 -> 0x677750   identical 6-arg signature
//   UGameViewportClient::Draw         0x62FBC0 -> 0x66E3B0   CalcSceneView's biggest caller
//   AActor::GetPlayerViewPoint        0x168E50 -> 0x191790   decompiles line-for-line identical
//
// STRUCT OFFSETS - every ULocalPlayer field moved by exactly +8 between the two builds (one extra
// pointer in a base class), consistent across the whole decompile, and corroborated by a vtable
// slot in GetPlayerViewPoint moving 0x218 -> 0x220.
//   viewport rect (Origin.X/Y, Size.X/Y)  +0xA0 -> +0xA8
//   ViewState                             +0xB0 -> +0xB8
//
// FSceneView is NOT a UObject, so it is not assumed to have shifted. The projection matrix offset
// is verified by SHAPE at runtime before it is trusted.

#include <Windows.h>
#include <intrin.h>

#include <cmath>
#include <cstdio>

#include "MinHook.h"
#include "ol2_common.h"
#include "ol2_ue3.h"
#include "ol2_xr.h"

namespace
{
// ---- addresses ---------------------------------------------------------------------------
constexpr uintptr_t kCalcSceneViewRva  = 0x677750;
constexpr uintptr_t kDrawRva           = 0x66E3B0;
constexpr uintptr_t kGetViewPointRva   = 0x191790;
constexpr uintptr_t kFovGetterRva      = 0x67E660;   // decompiles identically to Outlast 1's 0x60F4F0
constexpr uintptr_t kEngineGlobalRva   = 0x219B4A8;  // DAT_14219b4a8 in the Draw decompile
constexpr size_t    kGamePlayersOff    = 0x7D0;      // TArray<ULocalPlayer*>: data,num,max

// ---- struct offsets ----------------------------------------------------------------------
constexpr size_t kRectOff      = 0xA8;   // Origin.X, Origin.Y, Size.X, Size.Y
constexpr size_t kViewStateOff = 0xB8;
constexpr size_t kProjOffGuess = 0xC0;   // FSceneView projection matrix, confirmed by shape below

constexpr float kRotUnitToRad = 6.28318531f / 65536.0f;
constexpr float kRadToRotUnit = 65536.0f / 6.28318531f;

typedef void* (__fastcall* CalcSceneView_t)(void*, void*, void*, void*, void*, void*);
typedef void  (__fastcall* Draw_t)(void*, float*, void*);
typedef void  (__fastcall* GetViewPoint_t)(void*, float*, int*);
typedef float (__fastcall* GetFovAngle_t)(void*);

CalcSceneView_t o_CalcSceneView = nullptr;
Draw_t           o_Draw = nullptr;
GetViewPoint_t   o_GetViewPoint = nullptr;
GetFovAngle_t    o_GetFovAngle = nullptr;

uintptr_t g_base = 0;
uintptr_t g_drawLo = 0, g_drawHi = 0;

volatile long g_enabled = 0;  // Rejected half-viewport lane stays off; F1 is diagnostic only.
volatile long g_eyePhase = 0;      // 0 = off, 1 = LEFT (-half), 2 = RIGHT (+half)
volatile long g_inSplit = 0;
volatile long g_splitFrames = 0, g_splitFail = 0;
volatile long g_buildRightFirst = 0;
volatile long g_fresh = 0;         // frames of hysteresis since the last good pair
volatile long g_cvCalls = 0;

// Drive the engine's real per-player loop twice inside ONE Draw. Unlike calling Draw twice, this
// gives both eyes the complete caller-side setup without duplicating the whole frame or copying a
// bound backbuffer. The fake array is visible only for the duration of the synchronous Draw call.
volatile long g_pairLoopActive = 0;
volatile long g_pairCalcCalls = 0;
volatile long g_pairDraws = 0;
void* g_pairPlayer = nullptr;
void* g_pairPlayers[2] = {nullptr, nullptr};

// Safe same-frame path: two complete PRIMARY Draws, both using the left-position viewport that
// Outlast 2 actually publishes. The camera module captures pass one after unbinding the backbuffer;
// Present captures pass two and assembles the two live halves. No bound-resource copy is performed.
volatile long g_doubleDrawActive = 0;
volatile long g_doublePass = 0;       // 1 = left eye, 2 = right eye
volatile long g_doubleFrames = 0;
volatile long g_doubleMisses = 0;

float g_halfEyeUu = 3.2f;          // 100 uu = 1 metre, so 3.2 uu = a 64 mm IPD

// ---- FOV-fill -----------------------------------------------------------------------------
// Halving the viewport width costs most of the horizontal field: measured live on Outlast 2, a
// full-screen view renders 96.0 x 64.0 deg but each half-width eye renders 58.1 x 64.0. Outlast 1
// measured the same thing and its source says so outright: "a 90 deg camera renders ~58 deg/eye
// here - that is the whole problem".
//
// The engine's mapping is rendered = 2*atan(k * tan(camera/2)). So to RENDER a target angle, ask
// the camera for 2*atan(tan(target/2) / k). k is seeded at Outlast 1's measured 0.560 and then
// self-corrects from what the engine actually produced, so it needs no constant to be trusted.
volatile long g_fovFill = 1;
float g_fovK = 0.560f;
float g_fovAsked = 0.0f;           // camera angle last handed to the engine, degrees
volatile long g_fovApplied = 0;
bool  g_eyeSwap = false;

// head look, published by the XR layer
volatile long g_headValid = 0;
volatile long g_headYawUu = 0;
volatile long g_headPitchUu = 0;

float g_fovX = 0.0f, g_fovY = 0.0f;
size_t g_projOff = kProjOffGuess;
volatile long g_projConfirmed = 0;

// ---- SEH-guarded memory access. Every pointer here can be freed or garbage. ----------------
bool SafeReadBytes(const void* src, void* dst, size_t n)
{
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* SafeReadPtr(void* obj, size_t off)
{
    void* p = nullptr;
    if (!SafeReadBytes(static_cast<char*>(obj) + off, &p, sizeof(p))) return nullptr;
    return p;
}

bool SafeWritePtr(void* obj, size_t off, void* value)
{
    __try { *reinterpret_cast<void**>(static_cast<char*>(obj) + off) = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeWriteBytes(void* dst, const void* src, size_t n)
{
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- the rendered FOV ----------------------------------------------------------------------
// What is declared to the compositor must be EXACTLY what the engine rendered, or the image warps
// and swims. Do not derive it from the ini's DefaultFOV: the game widens FOV when running and the
// camcorder zooms. Read it off the projection matrix of the view the engine actually built.
//
// The offset is not assumed. A UE3 perspective projection has m[11]==1, m[15]==0, m[0] and m[5]
// positive, and m[1]/m[2]/m[4] zero. That shape is checked before the offset is trusted, and the
// first frames scan for it so a moved field announces itself instead of producing silent garbage.
bool ProjectionShaped(const float* m)
{
    return m[11] == 1.0f && m[15] == 0.0f &&
           m[0] > 0.05f && m[0] < 20.0f &&
           m[5] > 0.05f && m[5] < 20.0f &&
           m[1] == 0.0f && m[2] == 0.0f && m[4] == 0.0f;
}

void CaptureRenderedFov(void* view)
{
    if (view == nullptr) return;

    float m[16];
    if (InterlockedCompareExchange(&g_projConfirmed, 0, 0) == 0)
    {
        // Confirm the offset by shape once, scanning if the ported guess is wrong.
        if (SafeReadBytes(static_cast<char*>(view) + kProjOffGuess, m, sizeof(m)) && ProjectionShaped(m))
        {
            g_projOff = kProjOffGuess;
            InterlockedExchange(&g_projConfirmed, 1);
            Ol2Log("[OL2SFR] FSceneView projection confirmed at +0x%zX (same as Outlast 1)", g_projOff);
        }
        else
        {
            unsigned char buf[0x600];
            if (!SafeReadBytes(view, buf, sizeof(buf))) return;
            for (size_t off = 0; off + 64 <= sizeof(buf); off += 4)
            {
                const float* c = reinterpret_cast<const float*>(buf + off);
                if (ProjectionShaped(c))
                {
                    g_projOff = off;
                    InterlockedExchange(&g_projConfirmed, 1);
                    Ol2Log("[OL2SFR] FSceneView projection found at +0x%zX (Outlast 1 had +0xC0)", off);
                    break;
                }
            }
            if (InterlockedCompareExchange(&g_projConfirmed, 0, 0) == 0) return;
        }
    }

    if (!SafeReadBytes(static_cast<char*>(view) + g_projOff, m, sizeof(m))) return;
    if (!ProjectionShaped(m)) return;

    g_fovX = 2.0f * std::atan(1.0f / m[0]);
    g_fovY = 2.0f * std::atan(1.0f / m[5]);

    // Self-correct the camera-to-rendered mapping from what the engine actually did with the angle
    // that was requested. Measuring beats trusting a ported constant.
    if (g_fovAsked > 5.0f && g_fovAsked < 170.0f)
    {
        const float askedHalfTan = std::tan(g_fovAsked * 0.5f * 0.01745329f);
        if (askedHalfTan > 0.01f)
        {
            const float k = std::tan(g_fovX * 0.5f) / askedHalfTan;
            if (k > 0.3f && k < 1.2f) g_fovK = g_fovK * 0.9f + k * 0.1f;
        }
    }
}

// ---- GetPlayerViewPoint: where the head rotation and the eye offset go ----------------------
// Injecting the head into the engine's own view rotation (rather than rewriting a matrix later)
// means culling, occlusion and every downstream pass agree with where the player is looking.
void __fastcall Hook_GetViewPoint(void* actor, float* outLoc, int* outRot)
{
    o_GetViewPoint(actor, outLoc, outRot);

    if (outLoc == nullptr || outRot == nullptr) return;
    if (!InterlockedCompareExchange(&g_enabled, 0, 0)) return;

    // Head orientation first: the eye offset is measured along the resulting yaw.
    if (InterlockedCompareExchange(&g_headValid, 0, 0) != 0)
    {
        outRot[1] += InterlockedCompareExchange(&g_headYawUu, 0, 0);     // yaw
        outRot[0] += InterlockedCompareExchange(&g_headPitchUu, 0, 0);   // pitch
    }

    const long phase = InterlockedCompareExchange(&g_eyePhase, 0, 0);
    if (phase == 0) return;

    // UE3 is Z-up with X forward and Y right, so with roll ignored the camera's right axis is
    // (-sin(yaw), cos(yaw), 0). The yaw SIGN here cost Outlast 1 a round of left/right inversion;
    // this is the corrected form.
    const float yaw = static_cast<float>(outRot[1] & 0xFFFF) * kRotUnitToRad;
    const float rx = -std::sin(yaw);
    const float ry = std::cos(yaw);

    const float side = (phase == 2) ? 1.0f : -1.0f;
    const float d = g_halfEyeUu * side * (g_eyeSwap ? -1.0f : 1.0f);

    outLoc[0] += rx * d;
    outLoc[1] += ry * d;
}

// ---- CalcSceneView: gate and FOV read -------------------------------------------------------
// CalcSceneView has several call sites and only ONE is the render path; the others build views for
// Project/Deproject-style queries at different FOVs. Outlast 1 saw 86, 96 and 60 degrees in a
// single session. Touching those corrupts whatever they feed, so everything is gated on the return
// address landing inside UGameViewportClient::Draw.
// Only ever widens, and only inside the mod's own view-building passes, so aim, HUD and script logic
// keep the game's real value.
float __fastcall Hook_GetFovAngle(void* self)
{
    const float real = o_GetFovAngle(self);

    if (!InterlockedCompareExchange(&g_fovFill, 0, 0)) return real;
    if (!InterlockedCompareExchange(&g_eyePhase, 0, 0)) return real;
    if (!(real > 5.0f && real < 171.0f)) return real;

    // Aim at what the headset actually wants. Falling back to the game's own full-screen width
    // keeps this sane before the XR runtime has answered.
    float targetRad = 0.0f;
    if (!Ol2Xr::RuntimeEyeFovH(&targetRad)) targetRad = 96.0f * 0.01745329f;
    float targetDeg = targetRad * 57.2957795f;
    if (targetDeg > 130.0f) targetDeg = 130.0f;

    const float k = (g_fovK > 0.3f && g_fovK < 1.2f) ? g_fovK : 0.560f;
    const float wouldRenderDeg = 2.0f * std::atan(k * std::tan(real * 0.5f * 0.01745329f)) * 57.2957795f;
    if (wouldRenderDeg >= targetDeg - 1.0f) { g_fovAsked = real; return real; }

    float asked = 2.0f * std::atan(std::tan(targetDeg * 0.5f * 0.01745329f) / k) * 57.2957795f;
    if (asked > 165.0f) asked = 165.0f;      // past this the tangent blows up
    if (asked <= real + 1.0f) { g_fovAsked = real; return real; }

    g_fovAsked = asked;
    const long n = InterlockedIncrement(&g_fovApplied);
    if (n == 1 || (n % 3000) == 0)
    {
        Ol2Log("[OL2FOV] widening camera %.0f -> %.0f deg (would have rendered %.0f, target %.0f/eye, k=%.3f)",
               static_cast<double>(real), static_cast<double>(asked),
               static_cast<double>(wouldRenderDeg), static_cast<double>(targetDeg),
               static_cast<double>(k));
    }
    return asked;
}

bool SafeWriteRect(void* self, const float rect[4])
{
    __try
    {
        float* dst = reinterpret_cast<float*>(static_cast<char*>(self) + kRectOff);
        dst[0] = rect[0]; dst[1] = rect[1]; dst[2] = rect[2]; dst[3] = rect[3];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* __fastcall Hook_CalcSceneView(void* self, void* family, void* outLoc, void* outRot,
                                    void* viewport, void* drawer)
{
    const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool renderPath = (ra >= g_drawLo && ra < g_drawHi);

    // Two-primary path. Both passes deliberately render into the LEFT half: that is the only screen
    // position Outlast 2's final compositor publishes correctly. Each pass returns normally to its
    // own complete Draw, so neither eye is a secondary view and both receive the full post chain.
    if (renderPath && InterlockedCompareExchange(&g_doubleDrawActive, 0, 0) != 0)
    {
        float save[4] = {0, 0, 0, 0};
        if (SafeReadBytes(static_cast<char*>(self) + kRectOff, save, sizeof(save)) &&
            save[2] > 0.9f && save[3] > 0.9f)
        {
            const float liveSlot[4] = {0.0f, 0.0f, 0.5f, 1.0f};
            SafeWriteRect(self, liveSlot);
            void* view = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
            SafeWriteRect(self, save);
            CaptureRenderedFov(view);
            return view;
        }
    }

    // Native per-player path: Hook_Draw temporarily exposes the same LocalPlayer twice through the
    // engine's real GamePlayers array. Each entry receives the complete caller-side setup that a
    // manual second CalcSceneView call skips.
    if (renderPath && InterlockedCompareExchange(&g_pairLoopActive, 0, 0) != 0 &&
        self == g_pairPlayer)
    {
        const long eye = InterlockedIncrement(&g_pairCalcCalls) - 1;
        if (eye >= 0 && eye < 2)
        {
            float save[4] = {0, 0, 0, 0};
            if (SafeReadBytes(static_cast<char*>(self) + kRectOff, save, sizeof(save)) &&
                save[2] > 0.9f && save[3] > 0.9f)
            {
                const float rect[4] = {eye == 0 ? 0.0f : 0.5f, 0.0f, 0.5f, 1.0f};
                SafeWriteRect(self, rect);
                InterlockedExchange(&g_eyePhase, eye == 0 ? 1 : 2);
                void* view = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
                InterlockedExchange(&g_eyePhase, 0);
                SafeWriteRect(self, save);

                CaptureRenderedFov(view);
                if (eye == 1 && view != nullptr)
                {
                    const long n = InterlockedIncrement(&g_splitFrames);
                    InterlockedExchange(&g_fresh, 30);
                    if (n == 1 || (n % 600) == 0)
                        Ol2Log("[OL2PAIRLOOP] native per-player setup reached both eyes: frames=%ld", n);
                }
                return view;
            }
        }

        // Do not fall through into the manual double-call path while the native loop is active.
        return o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
    }

    // Diagnostic fallback if the native two-player driver cannot be armed.
    if (renderPath && InterlockedCompareExchange(&g_enabled, 0, 0) &&
        !InterlockedCompareExchange(&g_inSplit, 0, 0))
    {
        float save[4] = {0, 0, 0, 0};
        // Only split a player that owns the whole viewport. A letterboxed or cutscene rect means
        // something else is going on and rewriting it would corrupt that.
        if (SafeReadBytes(static_cast<char*>(self) + kRectOff, save, sizeof(save)) &&
            save[2] > 0.9f && save[3] > 0.9f)
        {
            InterlockedExchange(&g_inSplit, 1);

            const float L[4] = {0.0f, 0.0f, 0.5f, 1.0f};
            const float R[4] = {0.5f, 0.0f, 0.5f, 1.0f};
            void* left = nullptr;
            void* right = nullptr;

            // CalcSceneView has two caller-owned output parameters in addition to its returned
            // FSceneView. The caller consumes those outputs after the hook returns. Reusing the same
            // buffers for both synthetic eyes left them describing the SECOND eye while the hook
            // returned the FIRST view. Keep the primary call on the caller's buffers and give
            // the secondary call private storage so the returned view and outputs stay paired.
            alignas(16) float secondaryLoc[4] = {};
            alignas(16) int   secondaryRot[4] = {};

            // CalcSceneView APPENDS each view it builds to the family, so whichever is built first
            // becomes Views[0], the primary, and the second becomes Views[1], the secondary. UE3
            // gives the secondary less of the post/visibility treatment, which is the classic
            // "wrong on one eye only" family. Flipping the order moves the artifact between eyes
            // if that is the cause, and leaves it put if the cause is in the hook instead.
            const bool rightFirst = InterlockedCompareExchange(&g_buildRightFirst, 0, 0) != 0;
            const float* firstRect  = rightFirst ? R : L;
            const float* secondRect = rightFirst ? L : R;
            const long firstPhase   = rightFirst ? 2 : 1;
            const long secondPhase  = rightFirst ? 1 : 2;
            void** firstOut  = rightFirst ? &right : &left;
            void** secondOut = rightFirst ? &left : &right;

            if (SafeWriteRect(self, firstRect))
            {
                InterlockedExchange(&g_eyePhase, firstPhase);
                *firstOut = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
            }
            if (SafeWriteRect(self, secondRect))
            {
                InterlockedExchange(&g_eyePhase, secondPhase);
                *secondOut = o_CalcSceneView(self, family, secondaryLoc, secondaryRot,
                                             viewport, drawer);
            }
            InterlockedExchange(&g_eyePhase, 0);
            SafeWriteRect(self, save);          // ALWAYS restore, even if a build failed
            InterlockedExchange(&g_inSplit, 0);

            if (left != nullptr && right != nullptr)
            {
                const long n = InterlockedIncrement(&g_splitFrames);
                CaptureRenderedFov(right);      // both halves share it; right is the last built
                InterlockedExchange(&g_fresh, 30);

                if (n == 1 || (n % 600) == 0)
                {
                    // THE question. Live evidence says only the SECOND-built view ever renders and
                    // the first one's half is left stale, and that follows build order rather than
                    // screen side. That is exactly what aliasing looks like: if CalcSceneView hands
                    // back a REUSED FSceneView rather than a fresh one, both family entries point at
                    // one object, the second call's parameters overwrite the first's, and the engine
                    // renders a single half. If these two pointers are equal, the split cannot work
                    // as ported and the mechanism has to change.
                    Ol2Log("[OL2VIEWID] left=%p right=%p -> %s", left, right,
                           (left == right) ? "SAME OBJECT (engine reuses one FSceneView)"
                                           : "distinct views");
                }

                if (n == 1)
                {
                    Ol2Log("[OL2SFR] FIRST SPLIT - two views built in one frame. "
                           "ULocalPlayer+0x%zX rect was {%.3f, %.3f, %.3f, %.3f}",
                           kRectOff, static_cast<double>(save[0]), static_cast<double>(save[1]),
                           static_cast<double>(save[2]), static_cast<double>(save[3]));
                }
                if ((n % 600) == 0)
                {
                    Ol2Log("[OL2SFR] split alive: ok=%ld fail=%ld halfEye=%.2fuu fov=%.1fx%.1f deg",
                           n, InterlockedCompareExchange(&g_splitFail, 0, 0),
                           static_cast<double>(g_halfEyeUu),
                           static_cast<double>(g_fovX * 57.2957795f),
                           static_cast<double>(g_fovY * 57.2957795f));
                }
            }
            else
            {
                InterlockedIncrement(&g_splitFail);
            }

            // Return whichever was built FIRST: that is the family's primary view.
            void* primary = rightFirst ? right : left;
            if (primary != nullptr) return primary;
        }
    }

    void* view = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
    if (renderPath)
    {
        // Stable mono still needs the exact FOV the game rendered. Without this, OpenXR falls back
        // to each runtime eye's asymmetric FOV and the two identical images land at visibly
        // different horizontal positions (the live failure logged gameFov=0x0).
        CaptureRenderedFov(view);
        if (InterlockedIncrement(&g_cvCalls) == 1)
            Ol2Log("[OL2SFR] render-path CalcSceneView live (not splitting); publishing stable-mono FOV");
    }
    return view;
}

void __fastcall Hook_Draw(void* self, float* viewport, void* canvas)
{
    if (o_Draw == nullptr) return;

    struct PlayerArray
    {
        void** data;
        int num;
        int max;
    };

    // DAT_14219b4a8 is the engine singleton used by Draw; +0x7D0 is its TArray<ULocalPlayer*>.
    // Validate every field before temporarily publishing a two-entry static array.
    void* engine = SafeReadPtr(reinterpret_cast<void*>(g_base), kEngineGlobalRva);
    PlayerArray saved = {};
    void* arrayAddress = engine != nullptr ? static_cast<char*>(engine) + kGamePlayersOff : nullptr;
    void* player = nullptr;
    const bool valid =
        InterlockedCompareExchange(&g_enabled, 0, 0) != 0 &&
        arrayAddress != nullptr &&
        SafeReadBytes(arrayAddress, &saved, sizeof(saved)) &&
        saved.data != nullptr && saved.num == 1 && saved.max >= 1 && saved.max < 64 &&
        (player = SafeReadPtr(saved.data, 0)) != nullptr;

    if (!valid)
    {
        o_Draw(self, viewport, canvas);
        return;
    }

    g_pairPlayers[0] = player;
    g_pairPlayers[1] = player;
    const PlayerArray pair = {g_pairPlayers, 2, 2};
    if (!SafeWriteBytes(arrayAddress, &pair, sizeof(pair)))
    {
        o_Draw(self, viewport, canvas);
        return;
    }

    g_pairPlayer = player;
    InterlockedExchange(&g_pairCalcCalls, 0);
    InterlockedExchange(&g_pairLoopActive, 1);
    __try
    {
        o_Draw(self, viewport, canvas);
    }
    __finally
    {
        InterlockedExchange(&g_pairLoopActive, 0);
        g_pairPlayer = nullptr;
        SafeWriteBytes(arrayAddress, &saved, sizeof(saved));
    }

    const long calls = InterlockedCompareExchange(&g_pairCalcCalls, 0, 0);
    if (calls >= 2)
    {
        const long n = InterlockedIncrement(&g_pairDraws);
        if (n == 1 || (n % 600) == 0)
            Ol2Log("[OL2PAIRLOOP] Draw used validated native player array: frames=%ld calcCalls=%ld",
                   n, calls);
    }
}
bool HookOne(uintptr_t rva, void* detour, void** original, const char* name)
{
    void* target = reinterpret_cast<void*>(g_base + rva);
    const MH_STATUS created = MH_CreateHook(target, detour, original);
    const MH_STATUS enabled = (created == MH_OK) ? MH_EnableHook(target) : created;
    Ol2Log("[OL2SFR]   %-22s +0x%-8llX addr=0x%p create=%d enable=%d",
           name, static_cast<unsigned long long>(rva), target,
           static_cast<int>(created), static_cast<int>(enabled));
    return enabled == MH_OK;
}
}

namespace Ol2Ue3
{
void Init()
{
    static volatile long s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_drawLo = g_base + kDrawRva;
    g_drawHi = g_drawLo + 10634;   // Draw's size from Ghidra; bounds the render-path gate

    Ol2Log("[OL2SFR] installing engine hooks, module base=0x%p", reinterpret_cast<void*>(g_base));
    const bool a = HookOne(kCalcSceneViewRva, reinterpret_cast<void*>(&Hook_CalcSceneView),
                           reinterpret_cast<void**>(&o_CalcSceneView), "CalcSceneView");
    const bool b = HookOne(kDrawRva, reinterpret_cast<void*>(&Hook_Draw),
                           reinterpret_cast<void**>(&o_Draw), "Draw (native pair loop)");
    const bool c = HookOne(kGetViewPointRva, reinterpret_cast<void*>(&Hook_GetViewPoint),
                           reinterpret_cast<void**>(&o_GetViewPoint), "GetPlayerViewPoint");

    const bool d = HookOne(kFovGetterRva, reinterpret_cast<void*>(&Hook_GetFovAngle),
                           reinterpret_cast<void**>(&o_GetFovAngle), "GetFovAngle");

    Ol2Log("[OL2SFR] engine hooks %s (calcSceneView=%u drawPair=%u getViewPoint=%u fovGetter=%u)",
           (a && b && c && d) ? "INSTALLED" : "INCOMPLETE", a ? 1u : 0u, b ? 1u : 0u,
           c ? 1u : 0u, d ? 1u : 0u);
}

bool Active() { return InterlockedCompareExchange(&g_fresh, 0, 0) > 0; }

// The backbuffer is side-by-side while the split is live, so each eye is the left or right half.
bool SideBySide() { return Active(); }

void SetHeadLook(int32_t yawUU, int32_t pitchUU, bool enabled)
{
    if (!enabled) { InterlockedExchange(&g_headValid, 0); return; }
    InterlockedExchange(&g_headYawUu, yawUU);
    InterlockedExchange(&g_headPitchUu, pitchUU);
    InterlockedExchange(&g_headValid, 1);
}

void GetEyeFov(float* fovXRad, float* fovYRad)
{
    if (fovXRad) *fovXRad = g_fovX;
    if (fovYRad) *fovYRad = g_fovY;
}

void AdjustEye(float deltaUu)
{
    g_halfEyeUu += deltaUu;
    if (g_halfEyeUu < 0.0f) g_halfEyeUu = 0.0f;
    Ol2Log("[OL2SFR] half-eye separation %.2f uu", static_cast<double>(g_halfEyeUu));
}

float HalfEyeUu() { return g_halfEyeUu; }

void ToggleEyeSwap()
{
    g_eyeSwap = !g_eyeSwap;
    Ol2Log("[OL2SFR] eye swap %s", g_eyeSwap ? "ON" : "OFF");
}

void SetEnabled(bool enabled)
{
    InterlockedExchange(&g_enabled, enabled ? 1 : 0);
    Ol2Log("[OL2SFR] same-frame stereo %s", enabled ? "ON" : "OFF");
}

bool Enabled() { return InterlockedCompareExchange(&g_enabled, 0, 0) != 0; }

void ToggleBuildOrder()
{
    const long v = InterlockedCompareExchange(&g_buildRightFirst, 0, 0) ? 0 : 1;
    InterlockedExchange(&g_buildRightFirst, v);
    Ol2Log("[OL2SFR] build order: %s eye is now the PRIMARY view (Views[0])", v ? "RIGHT" : "LEFT");
}

void ToggleFovFill()
{
    const long v = InterlockedCompareExchange(&g_fovFill, 0, 0) ? 0 : 1;
    InterlockedExchange(&g_fovFill, v);
    Ol2Log("[OL2FOV] FOV fill %s", v ? "ON" : "OFF");
}

// Called once per XR frame so the ageing matches the compositor, not the render loop.
void FrameTick()
{
    if (InterlockedCompareExchange(&g_fresh, 0, 0) > 0) InterlockedDecrement(&g_fresh);
}
}
