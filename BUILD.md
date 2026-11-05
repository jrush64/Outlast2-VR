# Building Outlast 2 VR

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 (Community, Professional, Enterprise, or the standalone Build Tools)
  with the **"Desktop development with C++"** workload installed. `build.bat` finds it
  automatically via `vswhere.exe`.
- Nothing else. MinHook is vendored in `third_party/`. OpenXR is used through hand-written ABI
  types in `src/ol2_xr_types.h`, so no OpenXR SDK is needed.

## Build

```
scripts\build.bat
```

Output: `builds\dxgi.dll` (and `builds\dxgi.map`, useful for resolving crash addresses).

The build is a single `cl.exe` invocation (no solution, no CMake). It compiles the mod sources
together with the vendored MinHook sources and links the CRT statically (`/MT`), so the DLL needs
no separate VC++ redistributable in the game folder. The real system `dxgi.dll`, the OpenXR
loader and `d3dcompiler_47.dll` (part of Windows) are all loaded at runtime with `LoadLibrary`.

## Deploying

Copy these files into `<Outlast 2 install>\Binaries\Win64\`, next to `Outlast2.exe`, while the
game is closed:

- `builds\dxgi.dll` (just built)
- `release\openxr_loader.dll`: an x64 build of the official
  [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK), not built by this repo
- `runtime\outlast2vr.ini` (set `EnableXR=1` for the headset; `0` runs flat)

The log is written to `%LOCALAPPDATA%\Outlast2VR\Outlast2VR_Log.txt`. The expected log lines
are listed in `docs/TECHNICAL_NOTES.md`.

## Third-party components

| Component | Location | License |
|---|---|---|
| MinHook | `third_party/minhook` | BSD 2-Clause (see its `LICENSE.txt`) |
| OpenXR loader | `release/openxr_loader.dll` (prebuilt binary) | Apache 2.0 (`release/openxr_loader.LICENSE.txt`) |
