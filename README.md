<!--
SPDX-FileCopyrightText: 2026 GR2port
SPDX-License-Identifier: GPL-2.0-or-later
-->
<h1 align="center">
  <br>
  <b>GR2port - a genuine native PC port for Gravity Rush 2 on PC</b>
  <br>
</h1>

**No CPU emulation, No GPU emulation, No GPU command translation, No shader recompiler:** GR2port executes the x86-64 instructions in the Gravity Rush 2 game executable natively on PC. **The game's graphics were completely rewritten for PC** via a native Vulkan renderer that runs **handwritten plus recompiled GLSL shaders** that recreate visuals produced by the original GCN shaders. Each frame, GR2port takes the game's scene at the start of its frame graph and draws it on its own render thread, so the per-draw PS4 GPU commands are never built or translated. **The CPU overhead of the native renderer was measured to be only 0.8ms**. **All shader pipelines are precompiled** at boot and stored on disk so there is no shader stutter during gameplay. A runtime translates the firmware libraries on the PS4's Operating System called by the game to native PC solutions (no performance overhead).<br>
<br>

<img width="1904" height="1200" alt="ep03_5_12" src="https://github.com/user-attachments/assets/39858c3f-7500-4adb-ab9b-ecb0614aa6f8" /></b>
</b>

**Check out the following playlist if your dump is in .PKG format:**
https://www.youtube.com/watch?v=O8z6QPOaGDg&list=PLOgHncbXc__M&pp=sAgC

**Join the GR2fork discord server** (new) here for extended support. Anyone who wants to lurk for general game discussion surrounding gravity rush 1 and 2 are also welcome -> https://discord.gg/hnHRpzxGzG

**Support Me Here** for more features and projects in the Future: https://ko-fi.com/junminlee2004

-------
<img width="256" height="256" alt="20261025_030106_001" src="https://github.com/user-attachments/assets/5f28141c-570b-4010-9cd6-4ddb573217d6" /><img width="256" height="256![Uploading 20260525_051655_001.jpg…]()
" alt="20261025_031040_001" src="https://github.com/user-attachments/assets/fb09aaee-6adb-415c-9206-d292dd9e8cb5" /><img width="256" height="256" alt="20260705_103536_001" src="https://github.com/user-attachments/assets/b2f5e6f9-75f1-4548-9c19-e61deb38d46f" />

**System Requirements**

| | 1080p 30 (locked) | 1080p 30 (locked) | 4K 60 (locked) | 4K 120 (locked) |
|---|---|---|---|---|
| CPU | Ryzen 5 1600 | intel i7 4790 | Ryzen 7 5800X | Ryzen 7 9700X |
| GPU | GTX 960 4 GB | RX 570 4 GB | RTX 3060 8 GB | RTX 3080 Ti 12 GB |
| RAM | 12 GB | 12 GB | 12 GB | 12 GB |
| Storage | SSD | SSD | SSD | SSD |

**Included Features for Gravity Rush 2 and Gravity Rush Remastered**
- Flagship performance, graphical accuracy, and stability
- Feature complete fps patches (60fps, 40fps, 90fps, 120fps, 144fps)
- Feature complete resolution patches (240p-8K)
- Feature complete ultrawide patches (21:9, 32:9, 32:10, 16:10...)
- Feature complete online modes
- Feature complete photo mode, photo export and ingame photo gallery
- New performance patches and the ability to disable motion blur
- Infinite Gems patch
- Queen Alua costume swap patch
- Elektricitie costume swap patch
- The Other costume swap patch
- Raven costume swap patch
- Lisa costume swap patch
- Cecie costume swap patch
- Permet costume swap patch
- Kali costume swap patch
- Durga costume swap patch
- skip intro patches
- SSAO - TAA ghosting fix patch
- FSR 3.1 + FSR 4.1 support (Windows + Linux) + DLSS support (Windows Only)
- XBOX, NINTENDO and STEAM (Steam Deck, Steam Controller) button prompts for GR2 and GRR

**Included General QOL features**
- All GR2fork improvements are region agnostic
- massive general performance improvements
- massive performance improvements with precise (offloaded) readbacks
- massive performance improvements with asynchronous readback linear images
- touchpad press, double finger touchpad and single finger touchpad press mapping
- touchpad swipe mapping
- gamepad shake mapping
- hold L3 to convert face buttons to directional touchpad swipes (can be mapped to whatever)

# Keyboard and Mouse Mappings

> [!NOTE]
> Some keyboards may also require you to hold the Fn key to use the F\* keys. Mac users should use the Command key instead of Control, and need to use Command+F11 for full screen to avoid conflicting with system key bindings.

| Button | Function |
|-------------|-------------|
F10 | FPS Counter
Ctrl+F10 | Video Debug Info
F11 | Fullscreen
F12 | Trigger RenderDoc Capture (or game-only screenshot if RenderDoc is unavailable)
Alt+F12 | Capture screenshot including HUD/dialog overlays

> [!NOTE]
> Xbox and DualShock controllers work out of the box.

| Controller button | Keyboard equivalent |
|-------------|-------------|
LEFT AXIS UP | W |
LEFT AXIS DOWN | S |
LEFT AXIS LEFT | A |
LEFT AXIS RIGHT | D |
RIGHT AXIS UP | I |
RIGHT AXIS DOWN | K |
RIGHT AXIS LEFT | J |
RIGHT AXIS RIGHT | L |
TRIANGLE | Numpad 8 or C |
CIRCLE | Numpad 6 or B |
CROSS | Numpad 2 or N |
SQUARE | Numpad 4 or V |
PAD UP | UP |
PAD DOWN | DOWN |
PAD LEFT | LEFT |
PAD RIGHT | RIGHT |
OPTIONS | RETURN |
BACK BUTTON / TOUCH PAD | SPACE |
L1 | Q |
R1 | U |
L2 | E |
R2 | O |
L3 | X |
R3 | M |

Keyboard and mouse inputs can be customized in the settings menu by clicking the Controller button, and further details and help on controls are  also found there. Custom bindings are saved per-game. Inputs support up to three keys per binding, mouse buttons, mouse movement mapped to joystick input, and more.


# Contributing

If you want to contribute, please feel free :)

# Special Thanks

A few noteworthy teams/projects who've helped us along the way are:

- [**Panda3DS**](https://github.com/wheremyfoodat/Panda3DS): A multiplatform 3DS emulator from our co-author wheremyfoodat. They have been incredibly helpful in understanding and solving problems that came up from natively executing the x64 code of PS4 binaries


