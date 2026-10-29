# Third-party notices

SwanStationUWP is GPL-3.0-or-later. It is built on, and ships, the work below. Each component keeps its own licence.

## Emulation core shipped

SwanStationUWP ships **one** emulation core:

| Core | Upstream | Licence | Notes |
| --- | --- | --- | --- |
| SwanStation (libretro) | [libretro/swanstation](https://github.com/libretro/swanstation), a fork of [DuckStation](https://github.com/stenzek/duckstation) | GPL-3.0 (`third_party/swanstation/LICENSE`) | PlayStation 1. Built from source (`swanstation_libretro.dll`) with our changes in `tools/patches/` |

No other core is included, and no non-commercial-licensed core is shipped. SwanStationUWP is free and has no paid
features, ads or in-app purchases.

## Libraries and code in the package

| Component | Use | Licence |
| --- | --- | --- |
| [Mesa](https://www.mesa3d.org/) (the Dozen driver, Vulkan on Direct3D 12) and [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) (`dxil.dll`) | DLLs fetched by `tools/fetch-mesa.ps1` with pinned hashes and packaged with the app | MIT (Mesa); LLVM Release License (DXC) |
| [C++/WinRT](https://github.com/microsoft/cppwinrt) (NuGet `Microsoft.Windows.CppWinRT`) | the UWP layer (`src/platform/uwp`) | MIT |
| [rcheevos](https://github.com/RetroAchievements/rcheevos) | RetroAchievements client (`third_party/rcheevos`) | MIT |
| [stb_image, stb_truetype](https://github.com/nothings/stb) | cover decoding, font rendering (`third_party/stb`) | Public domain / MIT |
| [miniz](https://github.com/richgel999/miniz) | zip reading (`third_party/miniz`) | MIT |
| [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki | the phone page QR code (`third_party/qrcodegen`) | MIT |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Vulkan declarations (`third_party/vulkan-headers`) | Apache-2.0 / MIT |

## Fonts and icons (`assets/fonts`)

| Font | Licence |
| --- | --- |
| [Inter](https://github.com/rsms/inter), Chakra Petch, IBM Plex Sans, IBM Plex Mono, Fraunces, Work Sans | SIL Open Font License 1.1 (`OFL.txt`, `OFL-theme-fonts.txt`) |
| [Noto Sans JP](https://github.com/notofonts/noto-cjk), subset by `tools/make_jp_font.py` | SIL Open Font License 1.1 (`OFL-NotoSansJP.txt`) |
| [Tabler Icons](https://tabler.io/icons), subset by `tools/make_icon_font.py` | MIT (`LICENSE-tabler-icons.txt`) |

## Shaders

The shaders are converted for Direct3D 11 by the scripts in `tools/` (`make-d3d-chains.py`, `make-d3d-shaders.ps1`).

| Shader | Use | Licence |
| --- | --- | --- |
| [lcd3x](https://github.com/libretro/slang-shaders) by Gigaherz | the LCD3x shader | Public domain |
| [crt-royale](https://github.com/libretro/slang-shaders) (crt-royale-fast) by TroggleMonkey, adapted by Hyllian | CRT Royale Fast (`third_party/crt-royale`) | GPL-2.0-or-later (`third_party/crt-royale/LICENSE.TXT`) |
| [crt-royale](https://github.com/libretro/slang-shaders) by TroggleMonkey (the full preset, with his MIT-licensed blur and gamma includes) | CRT Royale (`third_party/crt-shaders`) | GPL-2.0-or-later (`third_party/crt-shaders/crt/shaders/crt-royale/LICENSE.TXT`), MIT (the includes) |
| [newpixie-crt](https://github.com/libretro/slang-shaders) by Mattias Gustavsson, adapted to slang by hunterk | CRT Newpixie | MIT or Unlicense, as the shader's own header says |
| [crt-hyllian](https://github.com/libretro/slang-shaders) and its NTSC-Adaptive-Lite pass, by Hyllian (the latter based on Themaister's NTSC shader) | CRT Hyllian NTSC | MIT (Hyllian's headers) |
| [crt-mattias](https://www.shadertoy.com/view/lsB3DV) by Mattias, adapted to slang in libretro's slang-shaders | CRT Mattias | see the libretro/slang-shaders repository (licence to be confirmed) |
| [crt-lottes](https://github.com/libretro/slang-shaders) by Timothy Lottes | CRT Lottes | Public domain |
| [crt-pi](https://github.com/libretro/glsl-shaders) by davej | CRT Pi | GPL-2.0-or-later |
| [zfast_lcd](https://github.com/libretro/glsl-shaders) by Greg Hogan (SoltanGris42) | zfast LCD | GPL-2.0-or-later |
| [NTSC shaders](https://github.com/libretro/slang-shaders) (ntsc/shaders/maister) by Hans-Kristian Arntzen (Themaister) | the four NTSC presets (`third_party/ntsc`) | see the libretro/slang-shaders repository (licence to be confirmed) |
| [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (EASU and RCAS), in the libretro port (`edge-smoothing/fsr`) | the FSR 1 shader (`third_party/fsr`) | MIT (AMD's headers), Unlicense (the port) |
| [MMPX](https://casual-effects.com/research/McGuire2021PixelArt/) by Morgan McGuire and Mara Gagiu, and MMPX 3.0 and 3.2 by [crashGG](https://github.com/crashGG) | the MMPX texture filters | MIT |

## Downloaded at run time, not included

| Source | What |
| --- | --- |
| [xlenore/psx-covers](https://github.com/xlenore/psx-covers) and [libretro-thumbnails](https://github.com/libretro-thumbnails) | covers, title screens and gameplay pictures |
| [libretro-database](https://github.com/libretro/libretro-database) and DuckStation's [chtdb](https://github.com/duckstation/chtdb) | cheat codes (the copyright of the codes is held by their authors) |
| [DuckStation's game database](https://github.com/stenzek/duckstation) (CC BY-NC-ND 4.0) | game information and known fixes |
| [The Bezel Project](https://github.com/thebezelproject) | game artwork borders |
| [RetroAchievements](https://retroachievements.org) | achievement sets and accounts, used online (terms of retroachievements.org) |

No BIOS, game, key or copyrighted data is included.

## History

SwanStationUWP comes from SwanStationPS5, a fork of [PSXS5](https://github.com/SynoPiia/PSXS5) by SynoPiia. The PS5
app (the jailbreak tooling, the PS5 SDK, RADV, libnfs, the NTFS reader, the disc daemon and the frame interpolation) is
not part of this app; its notices stay with that version: <https://github.com/darkxex/SwanStationPS5/releases>.

## 3D jewel case

This work is based on "Hypnagogia 無限の夢 Boundless Dreams Jewel Case"
(https://sketchfab.com/3d-models/hypnagogia-boundless-dreams-jewel-case-20e4780167b6441fb364060c79870378) by
sodaraptor (https://sketchfab.com/sodaraptor), licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/).
The model and its textures were adapted: the printed area of the lid (its art and the black banner) is cut out, so each
game's own cover shows in its place, and the titles and the PAL box on the edges were removed; the rest of the case (the
plastic, the ribs, the black edges) is the model's.
