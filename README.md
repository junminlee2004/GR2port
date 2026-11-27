<a href="https://recomplauncher.com/game/gr2port"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

<!--
SPDX-FileCopyrightText: 2026 GR2port
SPDX-License-Identifier: GPL-2.0-or-later
-->
<h1 align="center">
  <b>GR2port - a genuine native PC port for Gravity Rush 2</b>
</h1>

**No CPU emulation, no GPU emulation, no GPU command translation, no shader recompiler, no reading and/or writing PM4:** GR2port executes the x86-64 instructions in the Gravity Rush 2 game executable natively on PC (for CPUs that don't suppport SSE4a, AVX2, BMI1, F16C, MOVBE, those instructions are patched AOT with equivalent instructions compatible for that CPU by using trampolines - this is NOT CPU emulation since the executable is still x86-64 and is executed natively, and the overhead is incomparable to a JIT patcher or completely emulating the PS4 CPU or recompiling the game executable - the one exception is macOS, which requires statically recompiling the game's x86-64 executable to arm64). **The game's graphics were completely rewritten for PC** via a native Vulkan renderer (the game’s render passes have been rebuilt as its own Vulkan code: `glass.cpp, shadows.cpp, contours.cpp, deformers.cpp, enemies.cpp, world.cpp` etc.) in `src/graphics/gr2port` that runs **handwritten (`src/graphics/gr2port/shaders`) GLSL shaders** that recreate visuals produced by the original GCN shaders. 

<b>
  
To reiterate, the game's render passes are **NOT EMULATED OR STATICALLY RECOMPILED IN ANY SHAPE OR FORM** - they were completely rewritten.
Each frame, GR2port takes the game's scene at the start of its frame graph and draws it on its own render thread, so it never builds or translates per-draw PS4 GPU commands. The 2D screen drawing (menus, HUD, text, movies and photos) is read the same way, from the game's own 2D draw items, and drawn by the same renderer. **The CPU overhead of the native renderer was measured as only 0.8ms in frame time**. 

<b>

**As a disclaimer, this port does not rewrite the entire source code of the game (just the graphics source code). Since the game's executable already contains x86-64 code, a developmental shortcut was made to natively execute the code in the game's executable directly.  It does, however, port the graphics by reverse engineering each graphical pass and rewriting them natively for Vulkan** (this is not static recompilation). The original game logic is patched via a runtime memory patcher for PC enhancements and to prevent the game from wasting resources building PS4 GPU Commands (unused by the native renderer). Thus, this project is a fully native runtime that runs Gravity Rush 2 by natively executing original code and porting (rewriting not recompiling AOT/JIT) the game's graphics renderer.

<b>
  
**All shader pipelines are precompiled** at boot and stored on disk so there is no shader stutter during gameplay. A runtime translates the firmware libraries the game calls on the PS4's operating system into native PC operating system solutions (little to no performance overhead like WINE).

<img width="1904" height="1200" alt="ep03_5_12" src="https://github.com/user-attachments/assets/39858c3f-7500-4adb-ab9b-ecb0614aa6f8" /></b>
Screenshot © Sony Interactive Entertainment LLC. Not covered by this project's GPL 2.0 license.
</b>

**Check out the following playlist if your dump is in .PKG format:**
[https://www.youtube.com/watch?v=O8z6QPOaGDg&list=PLOgHncbXc__M&pp=sAgC](https://www.youtube.com/watch?v=rZ5jZToJVHE&list=PLOgHncbXc__M&index=9)

**Join the GR2fork discord server** (new) here for extended support. Anyone who wants to lurk for general game discussion surrounding gravity rush 1 and 2 are also welcome -> https://discord.gg/hnHRpzxGzG

**Support Me Here** for more features and projects in the Future: https://ko-fi.com/junminlee2004

-------
<img width="256" height="256" alt="20261025_030106_001" src="https://github.com/user-attachments/assets/5f28141c-570b-4010-9cd6-4ddb573217d6" /><img width="256" height="256" alt="20261025_031040_001" src="https://github.com/user-attachments/assets/fb09aaee-6adb-415c-9206-d292dd9e8cb5" /><img width="256" height="256" alt="20260705_103536_001" src="https://github.com/user-attachments/assets/b2f5e6f9-75f1-4548-9c19-e61deb38d46f" /><br>

<img width="1197" height="666" alt="race" src="https://github.com/user-attachments/assets/8e46b729-03e9-4ca0-aa66-34a25c9b7489" />

**Screenshots © Sony Interactive Entertainment LLC. Not covered by this project's GPL 2.0 license.**

**System Requirements:**
Generally, a GPU with **Vulkan 1.3** support is required. For NVIDIA GPUs, a GPU architecture equivalent to or newer than Maxwell is required. For AMD GPUs, a GPU architecture equivalent to or newer than Polaris is required.

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
- Support for 2 GB VRAM GPUs
- Support for older/weaker GPU architectures with fewer features (Intel iris Xe)
- Support for Intel CPUs as old as Sandy Bridge (no AVX2, BMI1, F16C, MOVBE)
- Support for AMD CPUs as old as Bulldozer (no AVX2, BMI1, F16C, MOVBE) 
- Fully game region-agnostic (Asia, Europe, USA, Japan regions all supported); **update v1.11 is required**.
- Launcher GUI contains all 31 console region languages, fully navigable via controller input.
- Native mouse input (controls camera)
- Fully bindable gamepad and keyboard bindings and button prompts
- in game sliders for special attack and panther mode controller sensitivity
- more customizable audio volume sliders
- photo mode camera roll and tilt support
- Fully customizable graphics presets
- graphics settings to hide HUD, gravity meter, and crosshair
- In-game exit to desktop button below return to title
- Graphics Settings page in the pause menu's System Menu, with changes applied live where the renderer allows
- Field of view setting: 80% to 150% of the game's own angle
- Right stick tilts menus, the title screen and comics when the pad has no gyro (or there is no pad)
- Trophy list in game (F1, or L3 + R3 on a pad)
- Verify Game Files under Setup > Paths: reads the four archives in full and compares them with a good dump
- A dump that differs from the good 1.11 dump is refused at start, and the message names the files. Loose-file dumps (extracted archives) are accepted but not compared
- Steam Deck preset
- A fully native renderer built from the ground up pass by pass, delivering impeccable performance, graphical accuracy, and stability
- 30-360fps supported, with gameplay timing the same at every frame rate
- 544p-8k resolution supported
- Ultrawide, 4:3, 3:2, and 16:10 supported
- HDR support
- Feature-complete online mode
- Feature-complete photo mode, photo export, and in-game photo gallery
- extra photo mode features (Hide UI)
- All GR2fork patches
- GR2fork Halloween items
- FSR 3.1 + FSR 4.1 + DLSS support (Windows + Linux) + XeSS support (Windows Only)
- FSR 3.1 Frame Generation
- NVIDIA Reflex Low Latency + AMD Antilag 2 + generic device agnostic low latency mode
- Keyboard, XBOX, NINTENDO and STEAM (Steam Deck, Steam Controller) button prompts for GR2
- Auto update checker on launcher startup
- f12 to screenshot

<img width="789" height="523" alt="image" src="https://github.com/user-attachments/assets/38b3f128-b28f-4862-b502-2a0ce746af1e" /> <b>

<b>
  
> [!NOTE]
> You can launch Gravity Rush 2 without the launcher by using the -nolauncher launch option.



# Keyboard and Mouse + Controller Mappings

| Button | Function |
|-------------|-------------|
F10 | FPS Counter
Ctrl+F10 | Video Debug Info
F11 | Fullscreen
F1 | Trophy list

> [!NOTE]
> Xbox and DualShock controllers work out of the box. The trophy list is L3 + R3 on a pad. Keys and buttons can be changed under Setup > Mouse & Keyboard > Key Bindings and Setup > Controller > Button Bindings.

## Controls

The mouse turns the camera (native mouse input). Keys can be changed in the launcher under Setup > Mouse & Keyboard > Key Bindings, and controller buttons under Setup > Controller > Button Bindings. There, Hide UI, Open Collection and Open Picture Roll take a combination of two buttons held down together, or a back paddle; they work only while the camera is out, and their prompts show the buttons they are on.

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
| Hide UI (Photo Mode) | L3 + R2 | H |
| Open Collection (Photo Mode) | L3 + L1 | [ |
| Open Picture Roll (Photo Mode) | L3 + R1 | ] |
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

If you want to contribute, please feel free to do so by reporting issues or submitting pull requests :)

# Special Thanks

- [**ShadPS4**](https://github.com/shadps4-emu/shadps4): An open source PS4 emulator written in C++. ShadPS4 along with my fork, GR2fork, provided the basic structure for this codebase, tooling necessary for research and development, and served as reference for this project's native execution of x64 code of PS4 binaries and graphical output.


