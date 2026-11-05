# Outlast 2 VR

An unfinished VR mod for Outlast 2 (Steam, x64, D3D11, Unreal Engine 3), built as a `dxgi.dll`
proxy loaded next to `Outlast2.exe`. It hooks the engine camera and the D3D11 renderer and
submits the game to any OpenXR runtime (SteamVR, Meta Quest Link, Virtual Desktop, etc.).

⚠️ Experimental, **unfinished** community mod. Not affiliated with Red Barrels. Use at your own
risk. Development has stopped, and the source is published so that others can pick it up. See
[What doesn't work yet](#what-doesnt-work-yet) and
[`docs/TECHNICAL_NOTES.md`](docs/TECHNICAL_NOTES.md).

Licensed under [GPL-3.0](LICENSE).

## What it does

- **Hooks the game cleanly.** The `dxgi.dll` proxy loads, owns the swapchain and Present, inline
  hooks the D3D11 context, finds the scene camera matrix in the game's constant buffers, and
  installs four UE3 engine hooks (`CalcSceneView`, `UGameViewportClient::Draw`,
  `GetPlayerViewPoint`, `GetFovAngle`).
- **OpenXR output, mono (confirmed in a headset).** An OpenXR session with a projection layer,
  showing the complete game frame, including HUD, pause and selection menus, in both eyes.
  This is the fallback whenever no stereo pair is available (menus, loading screens).
- **Head-look rotation (confirmed in a headset).** HMD yaw and pitch go into the engine's own
  view rotation, so culling follows the head.
- **FOV fill (confirmed in a headset).** The camera FOV is widened only inside the view-building
  passes, so the image fills the headset (measured about 96 x 103 degrees per eye) while aim,
  HUD and script logic keep the real FOV. The FOV declared to OpenXR is read back from what the
  engine actually rendered.
- **Same-frame stereo draw replay (flat only).** Opaque world draws are replayed a second and
  third time in the same frame into two independent full-size eye targets, with the camera moved
  3.2 engine units left and right (64 mm apart in total). The pair is shown side by side on the monitor in flat
  mode. In a flat side-by-side play test it ran smoothly with no visible problem. **It has not
  been checked in a headset**, and it is only the opaque geometry the replay captures, not a
  complete render of each eye (see below). With `EnableXR=1` this pair is what the headset gets
  whenever one is produced.

## What doesn't work yet

- **No verified headset stereo.** The stereo pair in this build has only been seen flat on the
  monitor. The mono headset path is the only confirmed VR output.
- **The draw replay is not a complete eye render.** It replays only qualifying opaque,
  depth-writing indexed draws into its own targets. Clears, lighting and shadow passes, sky,
  translucency, particles, fog, post-processing and UI are not reproduced per eye. Later
  experiments (not included here) that extended this replay further gave a black headset image
  with only lights visible. The approach is a dead end; the next step is to drive a complete
  engine render below `UGameViewportClient::Draw` once per eye. See the technical notes.
- **The engine's own split-screen stereo does not work.** Rendering the right eye through a
  half-width viewport with a nonzero origin gives a dark, streaked or stale right half. It is
  disabled (F1 turns it back on for diagnosis only).
- **No positional (6DOF) tracking.** Rotation only (yaw and pitch, no roll). An attempt to add it
  was rejected because stereo is not working yet.
- **No motion controllers and no VR UI placement.** Play with a gamepad or keyboard and mouse.
- **Intermittent flicker** was reported in the mono headset path in later runs.
- **Tied to one game build.** The engine addresses are fixed RVAs for the current Steam
  `Outlast2.exe` with no signature checks. Another build will likely crash.
- **Debug leftovers.** F1–F11 and PgUp/PgDn are diagnostic hotkeys from development; most of them
  control the rejected split. See the technical notes before using them.

## Building

See [BUILD.md](BUILD.md). In short: install Visual Studio 2022 with the C++ workload, then run
`scripts\build.bat`. MinHook is vendored in `third_party/`.

## Installing

See [`release/README.txt`](release/README.txt).

## Layout

| Path | What's there |
|---|---|
| `src/` | Mod source: DXGI proxy and boot (`ol2_boot`), D3D11 hooks and camera (`ol2_camera`), stereo draw replay (`ol2_sidecar`), UE3 engine hooks (`ol2_ue3`), OpenXR layer (`ol2_xr`) |
| `scripts/` | `build.bat` |
| `third_party/` | Vendored MinHook, used by the build |
| `docs/` | `TECHNICAL_NOTES.md`: architecture, engine addresses, approaches that failed and why |
| `release/` | End-user install notes, OpenXR loader, its license |
| `runtime/` | A starting `outlast2vr.ini` |
| `builds/` | A prebuilt `dxgi.dll` (and its `.map`) |

## License

GPL-3.0 - see [LICENSE](LICENSE). Copyright (C) 2026 Halcyon.

Vendored dependencies keep their own licenses: MinHook (BSD-2-Clause, `third_party/minhook`).
The shipped `openxr_loader.dll` is the Khronos OpenXR loader (Apache-2.0, license in
`release/`). `src/ol2_xr_types.h` restates OpenXR 1.0 ABI types from the Khronos OpenXR headers
(Apache-2.0).

The engine hook layout is ported from the Outlast (1) VR mod
([jrush64/outlast-vr-mod](https://github.com/jrush64/outlast-vr-mod)).
