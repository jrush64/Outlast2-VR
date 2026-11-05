# Technical notes

How the mod works, which engine addresses it uses, and which approaches were tried and failed,
so whoever continues does not repeat them. All addresses are RVAs into the current Steam
`Outlast2.exe` (x64, PE32+, 34.5 MB, linked 2018-03-13; image base `0x140000000` in Ghidra).
The game is UE3 with the D3D11 RHI (cooked shaders are SM4/SM5 only, no D3D9 renderer), Steam
DRM only (no Denuvo), so a proxy DLL loads without trouble.

## Architecture

```
Outlast2.exe
  └─ dxgi.dll (this mod)  ── forwards every dxgi export to the real System32\dxgi.dll
       ├─ ol2_boot.cpp     proxy exports, factory/swapchain vtable hooks, Present hook,
       │                   outlast2vr.ini, logging
       ├─ ol2_camera.cpp   D3D11 device/context inline hooks, ViewProjection detection in VS
       │                   constant buffers, head look on the VP, occlusion-query force,
       │                   flat preview, hotkeys, diagnostic counters
       ├─ ol2_sidecar.cpp  same-frame draw-replay stereo proof (two origin-zero eye targets,
       │                   packed side-by-side at Present)
       ├─ ol2_ue3.cpp      UE3 engine hooks: CalcSceneView, UGameViewportClient::Draw,
       │                   GetPlayerViewPoint, GetFovAngle (FOV fill, rendered-FOV readback)
       ├─ ol2_xr.cpp       OpenXR session, per-eye swapchains, eye caches, pose/FOV tagging
       ├─ ol2_xr_types.h   hand-written OpenXR 1.0 ABI types (no SDK needed)
       └─ ol2_me1_shims.h  adapters for the OpenXR core, which was written for the Mass Effect mods
```

The OpenXR loader (`openxr_loader.dll`) and `d3dcompiler_47.dll` are loaded at runtime with
`LoadLibrary`; nothing is statically linked except `user32.lib` and the vendored MinHook.

### Per game frame

1. The game renders normally. Every D3D11 context call that matters (`VSSetConstantBuffers`,
   `Map/Unmap`, `UpdateSubresource`, `PSSetShaderResources`, the `Draw*` family,
   `OMSetRenderTargets`, `GetData`) is caught by a MinHook **inline** hook on the implementation.
2. When the scene ViewProjection matrix is uploaded, `ol2_camera.cpp` finds it by content (not by
   slot or offset) and applies the HMD yaw/pitch to it. The centered, head-tracked camera buffer
   is recorded for the sidecar.
3. **Stereo replay (`ol2_sidecar.cpp`).** For each `DrawIndexed` that targets a full-size
   `R10G10B10A2_UNORM` colour target with an origin-zero full-size viewport and a depth-writing
   depth state, the draw is issued two more times: once into a private left colour/depth pair and
   once into a private right pair, each with a shadow copy of the camera constant buffer
   translated by -3.2 / +3.2 uu along the camera's right vector (100 uu = 1 m, so 64 mm IPD).
   Predication is turned off for the replays. The game's original draw always runs unchanged.
4. **At Present** the two eye targets are drawn side by side into a packed texture the size of the
   backbuffer (a runtime-compiled copy shader). In flat mode (`EnableXR=0`) that packed texture
   replaces the backbuffer. In VR mode it is handed to `ol2_xr.cpp`.
5. **OpenXR (`ol2_xr.cpp`).** If a fresh packed pair exists, its two halves become the two eye
   images. If not (menus, loading, no qualifying draws), both eyes are refreshed from the same
   complete backbuffer: the **stable mono** path. Each eye carries the pose and FOV it was
   rendered with, and the declared FOV is the FOV the engine actually rendered (read from the
   built `FSceneView`), never the headset's own FOV.

### Head look and FOV fill

- HMD yaw and pitch go into the engine's own view rotation through the `CalcSceneView` hook, so
  culling follows the head. Rotation only; no roll, no translation.
- **FOV fill.** `GetFovAngle` is hooked and the camera angle is widened only inside the mod's own
  view-building passes, so aim, HUD and script logic keep the real value. The mapping
  `rendered = 2*atan(k*tan(camera/2))` corrects `k` from the engine's own output every frame
  (seeded at 0.560, settles near 0.555); the target comes from the headset via `xrLocateViews`.
  Measured 96.0 x 102.7 deg per eye with fill on.

### Safety

- The device is created `D3D11_CREATE_DEVICE_SINGLETHREADED`: no internal locking. Every D3D11
  call the mod makes is on the game's render thread.
- Game memory reads/writes go through `__try/__except` helpers.
- There are **no opcode signature checks** on the engine addresses. On a different
  `Outlast2.exe` build the engine hooks will land on the wrong code. Check the addresses before
  using the mod on any other build.

## Engine facts

### D3D11 / renderer

- Factory path is `CreateDXGIFactory1`; presents through `Present`, not `Present1`. The exe
  imports no D3D statically, but an app-folder `dxgi.dll` still wins the DLL search. Fallback
  proxy hosts if that ever changes: `xinput1_3.dll` or `dbghelp.dll` (both shipped and statically
  imported).
- **The context's vtable lives inside the context object's own heap allocation** (vtable pointer
  8 bytes into the object, `MEM_PRIVATE`) and is rebuilt. Vtable patching does not stick; all
  context hooks are inline hooks on the function addresses.
- Backbuffer `R8G8B8A8_UNORM`, 3 buffers, no MSAA (e.g. 3601x2026 windowed, 3840x2160
  fullscreen).
- **The scene renders into a target rounded up to a multiple of 8** (3608x2032 for a 3601x2026
  window) and is rescaled on the way out. The main scene colour target is `R10G10B10A2_UNORM`.
- Module base changes per launch; it is logged, never hardcoded.

### Addresses

Found in Ghidra by fingerprinting on arithmetic, not symbols (the exe is stripped and keeps no
`check()` source strings). Outlast 1's `CalcSceneView` builds its projection with
`tanf(FOV * PI/360)` and reads exactly two float constants, PI `0x40490FDB` and 1/360
`0x3B360B61`. Scanning for functions that read both, ranked by size and caller count, gives five
candidates and one structural twin.

| Symbol | Outlast 1 RVA | **Outlast 2 RVA** | Notes |
|---|---|---|---|
| `ULocalPlayer::CalcSceneView` | 0x62A020 | **0x677750** | identical 6-arg signature; hooked |
| `UGameViewportClient::Draw` | 0x62FBC0 | **0x66E3B0** | size 10634; CalcSceneView's biggest caller; hooked |
| `AActor::GetPlayerViewPoint` | 0x168E50 | **0x191790** | decompiles line-for-line identical; hooked |
| `GetFovAngle` | 0x60F4F0 | **0x67E660** | size 77, 4 callers, identical; hooked |
| engine singleton (`GEngine`-like) | - | **0x219B4A8** | `+0x7D0` is `TArray<ULocalPlayer*>` (data, num, max) |

`ULocalPlayer` fields are Outlast 1's **+8** (one extra pointer in a base class; a vtable slot in
`GetPlayerViewPoint`/`GetFovAngle` moved `0x218 -> 0x220`):

| Field | Outlast 1 | **Outlast 2** | |
|---|---|---|---|
| viewport rect (Origin.X/Y, Size.X/Y) | +0xA0 | **+0xA8** | confirmed live (`{0,0,1,1}`) |
| ViewState | +0xB0 | **+0xB8** | inferred from the +8 rule |
| Actor / PlayerController | +0x68 | **+0x70** | seen in the Draw decompile |

`FSceneView` is not a UObject and did not shift: the projection matrix is at **+0xC0**. The code
verifies it by shape (`m[11]==1, m[15]==0, m[1]/m[2]/m[4]==0`) and rescans if that fails.

## Approaches that failed (do not silently restore)

1. **Half-viewport split in one view family** (`Ol2Ue3`, `g_enabled`, off; F1 turns it on for
   diagnosis only). Rewrites the player's viewport rect to the left half, builds a view, rewrites
   to the right half, builds a second. Both views exist (distinct `FSceneView` pointers, balanced
   draw counts), but the **physical right half is dark, streaked, overexposed or stale**. Reversing
   build order did not move the corruption; exposing the same player twice through the engine's
   real `GamePlayers` array (`Hook_Draw`, still in the code) made the native loop call
   `CalcSceneView` twice with full per-player setup, and the right half was unchanged. The common
   factor is rendering through a **nonzero viewport origin**. Do not make another variant of it.
2. **Calling `UGameViewportClient::Draw` twice per frame** with a backbuffer copy in between.
   Crashes in the NVIDIA D3D11 driver (`0xC0000005` in `nvwgf2umx.dll`) after about 300 frames.
3. **A second scene view state per eye.** The post chain (night-vision tint, colour grading)
   hangs off the view state, so an eye built from a fresh one renders without it and the eyes
   disagree. Outlast 1 found the same.
4. **Forcing occlusion queries visible** (`GetData` hook, F4) changes nothing for the split
   artifact. It is still on by default as harmless insurance.
5. **Vtable patching** of the D3D11 context: see the heap-vtable note above.
6. **Alternate-eye rendering (AER).** Mechanically works, but gives eye-position disagreement and
   visible temporal flicker (TAA, exposure, rain). Removed from the active path; the AER comments
   and PgUp/PgDn controls in the source are leftovers.
7. **Sampling the `R10`/`R16F` scene targets as final colour.** Copying HDR straight to the R8
   backbuffer skips tone mapping and gives crushed or blown-out output.
8. **Extending the draw replay into a full eye renderer.** Work after this snapshot (not
   included) widened the sidecar to more draw types, an HDR target and a replayed final
   post-process pass, and submitted the result to the headset. It showed only lights or isolated
   bright geometry on black. Replaying a whitelist of draws into private targets misses clears,
   non-indexed/instanced draws, compute, the G-buffer/lighting/shadow/reflection/exposure passes,
   translucency, fog, particles, the real post chain and UI. A complete D3D command-stream mirror
   would be a second renderer. The camera-buffer classifier (any VP-shaped 16 floats in buffers up
   to 8 KB) can also patch unrelated matrices.
9. **Adding positional (6DOF) tracking before stereo is proven.** Tried later; produced warped,
   inverted-feeling motion with almost no depth. Keep rotation only until depth, scale, FOV and
   eye order are verified.

## Debug hotkeys

| Key | Action |
|---|---|
| F1 | toggle the rejected half-viewport split (diagnostic only; leave off) |
| F2 | toggle the direct right-half compositor bypass (split diagnostic) |
| F3 | force the live-left to right-half mono mirror (split diagnostic) |
| F4 | occlusion force on/off (default on) |
| F5 | swap which eye is built first in the split (diagnostic) |
| F6 | FOV fill on/off (default on) |
| F7 | flip head-yaw sign (if the world turns the wrong way) |
| F8 | head tracking on/off (default on) |
| F9 | dump the live ViewProjection and derived FOV to the log once |
| F10 | camera sway proof: oscillates the camera sideways; the world must visibly slide |
| F11 | static half-IPD sideways offset |
| PgUp / PgDn | legacy AER half-eye separation +/- 0.4 uu (does not affect the sidecar, which is fixed at 3.2 uu) |

Do not bind F12; it was deliberately kept free.

## Settings

`outlast2vr.ini` next to `Outlast2.exe`. Only one key is read:

```ini
[VR]
EnableXR=0   ; 0 = flat, no OpenXR; 1 = create an OpenXR session
```

## Expected log lines (`%LOCALAPPDATA%\Outlast2VR\Outlast2VR_Log.txt`)

Falls back beside the exe if `LOCALAPPDATA` is missing.

| Tag | Meaning |
|---|---|
| `[OL2BOOT] proxy attached` | the proxy loaded |
| `[OL2BOOT] FIRST PRESENT via Present` | the Present hook owns the frame |
| `[OL2CFG] EnableXR=...` | which mode was read from the ini |
| `[OL2SFR] engine hooks ... (calcSceneView=1 drawPair=1 getViewPoint=1 fovGetter=1)` | all four engine hooks installed |
| `[OL2SFR] FSceneView projection confirmed at +0xC0` | projection offset verified by shape |
| `[OL2FOV] widening camera A -> B deg ...` | FOV fill is acting |
| `[OL2SIDECAR] created two independent origin-zero targets: WxH fmt=10 ...` | replay targets allocated |
| `[OL2SIDECAR] symmetric origin-zero pair composed: serial=N draws=M` | a packed stereo pair was built this frame |
| `[OL2XR] xrCreateSession: XR_SUCCESS` | OpenXR session up |
| `[OL2EYES] frame N PRECOMPOSITOR-SFR serial=...` / `... stable-mono` | which source the headset got |
| `[OL2CAM] frame=...` | periodic counters: context hooks, VP writes/patches, draws |

Counters prove that code ran, not that the picture is right. A build that allocates two textures
and submits two OpenXR views is a transport milestone, not proof of stereo.

## Where to go next

1. **Verify this snapshot in the headset.** The draw-replay pair has only been checked flat.
   Set `EnableXR=1` and check depth, eye order and scale. Expect the replay to contain only the
   opaque world draws it captures; if the image is incomplete, that matches failure 8.
2. **Own a complete render below `UGameViewportClient::Draw`.** The renderer that can work is two
   complete, origin-zero, full-size views per frame. Find the native entry that renders one
   complete view family (UE3's `FSceneRenderer::Render` or this engine's equivalent), capture the
   view-family inputs, and invoke it once per eye into owned targets with equal and opposite eye
   offsets applied to the real view state, keeping rotation common to both eyes.
3. **Gate A, flat only:** two full-size eye images side by side, each with geometry, sky,
   lighting, shadows, translucency, particles, fog, post and UI. No four-panel duplication, no
   half-screen clipping, no black right eye.
4. **Gate B:** log both final textures (identity, size, format, serial, camera origin,
   projection) and add a left-only / right-only / |L-R| flat diagnostic. Near geometry must show
   more parallax than far; eye order must be right; neither eye may be a copy of the other.
5. **Gate C:** only then submit those same textures to OpenXR, rotation first, positional
   tracking last.
6. UI: decide how the HUD, subtitles and pause menu reach each eye (the completed-backbuffer
   capture at Present carries all of them, which is why the mono path shows them correctly).
7. Add opcode signature checks to the four engine hooks so another exe build fails safe.
