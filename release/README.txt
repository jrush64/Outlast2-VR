================================================================================
  OUTLAST 2 VR  (experimental, unfinished)
================================================================================

WHAT YOU NEED

  - Outlast 2 (Steam, PC).
  - A PC VR headset and an OpenXR runtime (SteamVR, Meta Quest Link,
    Virtual Desktop, etc.). Set the runtime you use as the active OpenXR runtime.

WHAT TO EXPECT

  This is a development build, not a playable VR mod.

  Head rotation, the widened field of view and a mono headset image (the
  complete game frame in both eyes) have been tested in a headset. During
  gameplay, however, this build sends an experimental stereo image to the
  headset. That image has only been tested on a monitor, never in a
  headset, and contains only part of the scene. Menus and loading screens
  fall back to the mono image. See README.md for details.

INSTALLING

  1. Close the game.

  2. Find your Outlast 2 folder. In Steam, right click the game, then
     Manage, then Browse local files. From there open  Binaries\Win64.

  3. Copy these files into that folder, next to Outlast2.exe:

         dxgi.dll            (from the builds folder)
         openxr_loader.dll   (from this folder)
         outlast2vr.ini      (from the runtime folder)

  4. Open outlast2vr.ini and set  EnableXR=1  for the headset.
     EnableXR=0 runs on the monitor only, with the stereo test image
     side by side.

  5. Start your VR runtime, then start the game normally from Steam.

USING IT

  F8       turns head tracking on or off.
  F7       flips the turning direction if the world turns the wrong way.
  F6       turns the widened field of view on or off.

  The other F keys are development diagnostics. Leave them alone.

  A log is written to  %LOCALAPPDATA%\Outlast2VR\Outlast2VR_Log.txt

UNINSTALLING

  Delete dxgi.dll, openxr_loader.dll and outlast2vr.ini from Binaries\Win64.

THIRD-PARTY

  openxr_loader.dll is the Khronos OpenXR loader, Apache License 2.0.
  See openxr_loader.LICENSE.txt.
