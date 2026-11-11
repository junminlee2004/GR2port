<!--
SPDX-FileCopyrightText: 2026 GR2port
SPDX-License-Identifier: GPL-2.0-or-later
-->
<h1 align="center">
  <b>GR2port - a genuine native PC port for Gravity Rush 2</b>
</h1>

**No CPU emulation, no GPU emulation, no GPU command translation, no shader recompiler:** GR2port executes the x86-64 instructions in the Gravity Rush 2 game executable natively on PC. **The game's graphics were completely rewritten for PC** via a native Vulkan renderer that runs **handwritten plus recompiled GLSL shaders** that recreate visuals produced by the original GCN shaders. 
Each frame, GR2port takes the game's scene at the start of its frame graph and draws it on its own render thread, so it never builds or translates per-draw PS4 GPU commands. **The CPU overhead of the native renderer was measured as only 0.8ms in frame time**.<b>
<b>
**All shader pipelines are precompiled** at boot and stored on disk so there is no shader stutter during gameplay. A runtime translates the firmware libraries the game calls on the PS4's operating system into native PC solutions (no performance overhead).

<img width="1904" height="1200" alt="ep03_5_12" src="https://github.com/user-attachments/assets/39858c3f-7500-4adb-ab9b-ecb0614aa6f8" /></b>
Screenshot © Sony Interactive Entertainment LLC. Not covered by this project's GPL 2.0 license.
</b>

**Check out the following playlist if your dump is in .PKG format:**
https://www.youtube.com/watch?v=O8z6QPOaGDg&list=PLOgHncbXc__M&pp=sAgC

**Join the GR2fork discord server** (new) here for extended support. Anyone who wants to lurk for general game discussion surrounding gravity rush 1 and 2 are also welcome -> https://discord.gg/hnHRpzxGzG

**Support Me Here** for more features and projects in the Future: https://ko-fi.com/junminlee2004

-------
<img width="256" height="256" alt="20261025_030106_001" src="https://github.com/user-attachments/assets/5f28141c-570b-4010-9cd6-4ddb573217d6" /><img width="256" height="256![Uploading 20260525_051655_001.jpg…]()
" alt="20261025_031040_001" src="https://github.com/user-attachments/assets/fb09aaee-6adb-415c-9206-d292dd9e8cb5" /><img width="256" height="256" alt="20260705_103536_001" src="https://github.com/user-attachments/assets/b2f5e6f9-75f1-4548-9c19-e61deb38d46f" /><br>
Screenshots © Sony Interactive Entertainment LLC. Not covered by this project's GPL 2.0 license.

**System Requirements**

| | 720p 60 (locked) LOW PRESET | 800p 60 (locked) STEAM DECK PRESET | 4K 60 (locked) PS4 ORIGINAL PRESET | 4K 120 (locked) PS4 ORIGINAL PRESET |
|---|---|---|---|---|
| CPU | intel i5 3470 | STEAM DECK | Ryzen 5 1600 | Ryzen 5 3600 |
| GPU | GTX 1050 2 GB | STEAM DECK | RTX 3060 8 GB | RTX 3080 Ti 12 GB |
| RAM | 8 GB | 16 GB | 12 GB | 12 GB |
| Storage | HDD | SSD | HDD | HDD |

> [!NOTE]
> Upscaling is NOT considered for these fps targets but may be used to optionally improve fps further.
> Currently, Windows and Linux are supported. MacOS release is planned.

**Included Features for Gravity Rush 2**
- Fully game region-agnostic (Asia, Europe, USA, Japan regions all supported); **update v1.11 is required**.
- Launcher GUI contains all 31 console region languages.
- Native mouse input (controls camera)
- Fully bindable gamepad and keyboard bindings and button prompts
- Fully customizable graphics presets
- Steam Deck preset
- Impeccable performance, graphical accuracy, and stability
- 30-360fps supported
- 540p-8k resolution supported
- ultrawide and 16:10 supported
- Feature complete online mode
- Feature complete photo mode, photo export and ingame photo gallery
- All GR2fork patches
- GR2fork Halloween items
- FSR 3.1 + FSR 4.1 support (Windows + Linux) + DLSS support (Windows Only)
- Keyboard, XBOX, NINTENDO and STEAM (Steam Deck, Steam Controller) button prompts for GR2

<img width="789" height="523" alt="image" src="https://github.com/user-attachments/assets/38b3f128-b28f-4862-b502-2a0ce746af1e" />


> [!NOTE]
> You can launch Gravity Rush 2 without the launcher by using the -nolauncher launch option.



# Keyboard and Mouse + Controller Mappings

| Button | Function |
|-------------|-------------|
F10 | FPS Counter
Ctrl+F10 | Video Debug Info
F11 | Fullscreen

> [!NOTE]
> Xbox and DualShock controllers work out of the box.

## Controls

The mouse turns the camera (native mouse input). Keys can be changed in Options > Key Bindings.

| Game Function | Controller | Keyboard & Mouse |
|---|---|---|
| Move Forward | Left stick up | W |
| Move Back | Left stick down | S |
| Move Left | Left stick left | A |
| Move Right | Left stick right | D |
| Jump | Cross | Space |
| Normal Attack | Square | Left Mouse Button |
| Special Attack | Triangle | F |
| Stasis Field (Grab) | Circle | R |
| Shift Gravity | R1 | Right Mouse Button |
| Return Gravity | L1 | Q |
| Gravity Slide (Hold) | L2 | Left Shift |
| Evade | See below | E |
| Lunar Style | See below | Mouse Wheel Up |
| Jupiter Style | See below | Mouse Wheel Down |
| Normal Style | See below | Middle Mouse Button |
| Panther Mode (Hold) | Hold the touch pad | C |
| Reset Camera | R3 | None |
| Enter Photo Mode | D-pad up | 1 |
| Display the Map | D-pad right | 2 |
| Initiate Gesture | D-pad down | 3 |
| First-person View | D-pad left | 4 |
| Pause | OPTIONS | Esc |
| Zoom In (Photo Mode) | R2 | None |
| Flick Up (Touch Pad) | Flick the touch pad up | None |
| Flick Down (Touch Pad) | Flick the touch pad down | None |
| Flick Left (Touch Pad) | Flick the touch pad left | Z |
| Flick Right (Touch Pad) | Flick the touch pad right | X |

### Controller buttons that depend on the control type (Options > Controls)

Each key keeps its function in every type.

| Game Function | Type A | Type B | Type C | Type D | Type E |
|---|---|---|---|---|---|
| Evade | R2 | Flick the touch pad | Flick the touch pad | Flick the touch pad | Flick the touch pad |
| Lunar Style | Flick the touch pad up | R2 until Lunar | R2 + left stick up | R2 + right stick up | R2 + triangle |
| Jupiter Style | Flick the touch pad down | R2 until Jupiter | R2 + left stick down | R2 + right stick down | R2 + cross |
| Normal Style | Tap the touch pad | R2 until Normal | R2 | R2 | R2 |



# Contributing

If you want to contribute, please feel free to do so :)

# Special Thanks

- [**ShadPS4**](https://github.com/shadps4-emu/shadps4): An open source PS4 emulator written in C++. ShadPS4 along with my fork, GR2fork, provided the basic structure for this codebase, tooling necessary for research and development, and served as reference for this project's native execution of x64 code of PS4 binaries and graphical output.


