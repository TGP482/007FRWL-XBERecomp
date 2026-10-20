# 007: From Russia with Love - XBE Recomp
An unofficial native PC port of the original Xbox version of 007: From Russia with Love, with extended graphics options, keyboard and mouse support, and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation
The latest version of [007FRWL-XBERecomp](https://github.com/TGP482/007FRWL-XBERecomp/releases) can be found in the Releases page.

- Place your game files in the `assets` folder. This can be an ISO, or the extracted data files, of the USA or Europe release.
- Run `007FRWL.exe`. The first start compiles the game's code for your PC, this only happens once.
- Recomp Settings can be adjusted in the in-game options menu while the game is running, or through the Settings overlay, which is opened with <kbd>F10</kbd> by default.

## Features
### Display
- **Monitor** - Picks the monitor the game shows on, for Fullscreen and Borderless (and where Windowed opens). Lists every connected monitor by its Windows number and desktop resolution.
- **Display Mode** - Switch between Fullscreen, Borderless and Windowed.
- **Resolution** - Lists every mode your monitor reports and defaults to your desktop resolution, replacing the original 640x480 resolution.
- **Render Resolution** - Renders the game at 20% to 200% of the output resolution, allowing supersampling or downscaling independent of the window size.
- **Field of View** - Sets the horizontal field of view during gameplay from 45 to 140 degrees, with 65 being the game's default. Cutscenes and scopes keep the original.
- **Max Frame Rate** - Caps the frame rate to your monitor's refresh rate or a custom value, or removes the cap entirely.
- **V-Sync** - Reduces screen tearing.
- **HDR Output** - Enables HDR10 output on displays with Windows HDR turned on, detected automatically. Highlights extend up to the display's peak brightness.
- **FPS Counter** - Displays the frame rate on screen.
- **FMV Scale** - Choose between Fill, which zooms the 4:3 FMVs to fill widescreen displays, and 4:3, which keeps them at their original aspect ratio.

### Graphics
- **Multisample Antialiasing (MSAA)** - Enables 2x, 4x or 8x MSAA to smooth jagged edges while preserving a sharp image.
- **Subpixel Morphological Antialiasing (SMAA)** - Enables SMAA to smooth jagged edges across the whole frame with a softer image. SMAA also smooths alpha-tested edges such as fences and foliage, and can be combined with MSAA.
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures.
- **Draw Distance** - Increases the level of detail and draw distances past the game's values: 2x, 4x or 8x.
- **Ambient Occlusion** - Adds screen space ambient occlusion, creating soft contact shadows where surfaces meet. Choose between SSAO and GTAO.

### Input
- **Mouse and Keyboard** - Full keyboard and mouse support with raw input, adjustable sensitivity, inverted look and rebindable controls.
- **Controller** - Controller with SDL3 support, buttons can be rebound from the Controller page. Defaults match the Xbox layout.
- **Free Aim** - Adds the rocket launcher's over the shoulder aim to every gun while aim is held, instead of locking onto a target.

### General
- **Skip Intro** - Skips the intro logos.
- **Skippable Cutscenes** - Allows the in game cutscenes to be skipped with ESC or the Start button.
- **Skip Pop Ups** - Choose which pop-ups are skipped: Objectives, the objective messages shown on screen, Tips, the pop-ups that interrupt gameplay such as tutorials, attaché cases, schematics and keys, or All.
- **Autosave** - Saves the game automatically at the end of every level.
- **Persistent Bodies** - Prevents the bodies of killed enemies, and the guns and shields they drop, from disappearing.
- **Inventory Pause** - Toggles the inventory pause.
- **Pause On Focus Loss** - Pauses the game and its sound while the window is in the background or minimized.
- **Master Volume** - Adjusts the volume of all game audio.
- **Music Volume** - Adjusts the volume of the music.

### Miscellaneous
- **Boot Splash** - Place a `splash.png` (or `.bmp`) beside the exe to display a splash screen at launch.
- **Custom Menu Cursor** - Place a `cursor.png` (or `cursor.dds`) beside the exe to replace the Windows arrow in the game's menus.

## System Requirements
### Minimum
- **OS**
  - Windows 10 64-bit
- **CPU** with 2 cores and support for SSE4.2 and POPCNT
  - Intel Nehalem (Intel Core 1st Generation)
  - AMD Bulldozer (AMD FX series)
- **GPU** with support for Direct3D 12
  - NVIDIA GeForce GT 630 (Kepler)
  - AMD Radeon HD 7750
  - Intel HD Graphics 510 (Skylake)
- **Memory**
  - 4 GB
- **Storage**
  - 5 GB for the game files, saves and shader cache

## Building
### 1. Clone the Repository
Clone this repository using [Git](https://git-scm.com/).
```
git clone https://github.com/TGP482/007FRWL-XBERecomp.git
```

### 2. Add the Required Game Files
Place your extracted game data files in the `assets` folder. `default.xbe` from the USA, Europe or Asia release must be there, with the game's folders beside it.

> [!NOTE]
> Building does not support the ISO format, so the files must be extracted. ISOs only work for playing.

### 3. Install Dependencies
Install [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) and select the following **Workloads** and **Individual components**:
- Desktop development with C++
- C++ Clang tools for Windows
- C++ CMake tools for Windows

> [!TIP]
> `build.bat` offers to install the Clang and CMake components for you if they're missing.

### 4. Build the Project
- Run `build.bat`. The first build recompiles the entire game and can take 30 to 90 minutes on weaker systems.
- When it finishes, run `out\build\release\007FRWL.exe`.

## Credits
- [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
