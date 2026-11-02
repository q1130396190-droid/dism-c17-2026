# DuiLib_Ultimate · Win8Metro C++17 Edition

> [中文说明 / Chinese](README.md)

A lightweight, high-performance, extensible Windows desktop UI library and toolbox targeting the Windows 10/11 x64 platform, rebuilt on C++17 with a C++23 modern track available. This repository contains both the core library source and a ready-to-ship Win8Metro desktop tool release.

---

## Table of Contents

- [Overview](#overview)
- [Repository Contents](#repository-contents)
- [Build Environment Requirements](#build-environment-requirements)
- [Build Instructions](#build-instructions)
  - [Option 1: Build Directly with Visual Studio](#option-1-build-directly-with-visual-studio)
  - [Option 2: Build DuiLib Core via CMake](#option-2-build-duilib-core-via-cmake)
  - [Option 3: Rust dismcore Submodule (Optional)](#option-3-rust-dismcore-submodule-optional)
- [Runtime Dependencies](#runtime-dependencies)
- [AI-Assisted Development](#ai-assisted-development)
- [Known Issues](#known-issues)
- [Directory Structure](#directory-structure)
- [License](#license)

---

## Overview

**DuiLib_Ultimate** is an open-source powerhouse for Windows UI development, built around three design pillars: **lightweight**, **performant**, and **extensible**. This repository ships both the UI framework and a complete desktop application built on top of it — **Win8Metro**.

---

### Win8Metro · Windows System Maintenance Toolkit

**Win8Metro** is an all-in-one Windows 10/11 deployment and maintenance tool covering 17 operational scenarios — from system cleanup and driver management to unattended installation. It features a classic Metro-style interface, 11 localizations, and ships as a portable, extract-and-run release.

#### 🧩 Feature Modules

| Module | Description | Technical Highlights |
|--------|-------------|---------------------|
| **🧹 System Cleanup** | One-click cleanup of temp files, thumbnail cache, Windows Update residuals, Delivery Optimization cache, previous Windows versions | Multi-threaded scan, WIM-mounted cleanup |
| **🚀 Startup Manager** | Boot-time autostart entries, scheduled tasks, drivers, services, Shell extensions | Registry + filesystem dual-engine scan |
| **💾 Driver Management** | Scan installed drivers, backup, restore, uninstall | VSS snapshot protection, PNP device tree traversal |
| **🟦 Features On/Off** | DISM component enable/disable (.NET Framework, Hyper-V, Sandbox, etc.) | CBS hosting + offline WIM support |
| **🔄 Windows Update** | Online / offline cumulative update install, history, rollback | CBS sessions + SSU/LCU batch injection |
| **🟪 Appx Management** | Batch install/uninstall/cleanup of Microsoft Store apps | AppxPackage API + offline package deployment |
| **📋 Installed Programs** | 32/64-bit software inventory, batch uninstall, silent uninstall | Registry Uninstall keys + MSI parsing |
| **🔗 File Associations** | View and modify default handlers for file extensions | ASSOC / FTYPE + IApplicationAssociationRegistration |
| **🎛️ Control Panel** | One-click shortcuts to all system settings | Windows Shell command wrappers |
| **🛠️ Toolset** | Disk partition manager, ISO mount, WIM image processing, system image backup, BCD editor, VSS shadows | Standalone Wizard + DISM API |
| **🌐 Internationalization** | Switch system UI language, install language packs | 11 languages (zh-CN, en-US, ja-JP, ko-KR, ru, de, fr, it, hu, pl-PL, cs) |
| **⚡ System Optimization** | Disable telemetry, turn off Defender, enable Ultimate Performance, font cleanup | Registry + Group Policy + Scheduled Tasks |
| **🧽 Registry Cleanup** | Invalid registry scan, orphan references, uninstall residuals | Registry hash verification + safe whitelist |
| **📅 Scheduled Tasks** | List/disable system and third-party scheduled tasks | ITaskScheduler interface |
| **📦 Unattended Setup** | Generate Autounattend.xml, Sysprep packaging, ISO auto-install | Windows SIM config + BCD boot |

#### 🏗️ Software Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     Win8Metro.exe                       │
│  ┌──────────────────────────────────────────────────┐  │
│  │                 DuiLib UI Layer                   │  │
│  │  FrameWnd / PageHost / ThemeService / StringMgr   │  │
│  └──────────────────────────────────────────────────┘  │
│  ┌─────────┬──────────┬──────────┬─────────────────┐  │
│  │ Theme   │ Resource │  Intl    │   Config / Seed  │  │
│  │ Manager │ Manager  │ Service  │                  │  │
│  └─────────┴──────────┴──────────┴─────────────────┘  │
│  ┌─────────┬──────────┬──────────┬─────────────────┐  │
│  │  Clean  │ Driver   │ Feature  │  Update / Appx  │  │
│  │ Engine  │ Service  │ Service  │   Service        │  │
│  └─────────┴──────────┴──────────┴─────────────────┘  │
│  ┌─────────┬──────────┬──────────┬─────────────────┐  │
│  │ CBSHost │ NCleaner │  Activ.  │   duilibcalc    │  │
│  └─────────┴──────────┴──────────┴─────────────────┘  │
│  ┌──────────────┬────────────────────────────────────┐  │
│  │ Rust FFI     │   Win32 / COM / DISM API           │  │
│  │ (Hash/Crypto)│                                    │  │
│  └──────────────┴────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

#### 🔐 Security & Permissions

- **Admin required**: Features touching DISM, drivers, CBS require administrator elevation
- **VSS snapshot protection**: System restore points are created automatically before cleanup
- **Whitelist mechanism**: Both registry cleanup and driver operations ship with built-in safety whitelists

---

### DuiLib_Ultimate Core Library

The UI foundation — DuiLib_Ultimate — powers Win8Metro and is available for any C++ desktop project:

- **x64-only**: all build configurations drop Win32 / ARM / ARM64
- **C++17 minimum**: supports a C++23 modern track (`DUI_TRACK=modern`)
- **Windows 10+**: `_WIN32_WINNT=0x0A00`
- **WebView2 integration**: Edge WebView2 rendering control enabled by default on the modern track
- **Rust submodule**: `dismcore` — a memory-safe calculation module written in Rust
- **API-compatible**: upstream DuiLib compatibility with built-in migration scripts
- **Stylesheet (Style)**: centralized control properties, one-line skin switch
- **Resource manager**: embedded Zip resource loading with automatic external-file fallback
- **Localization framework**: runtime language-pack hot swap
- **Per-Monitor V2 DPI awareness**

---

### Technology Stack

| Layer | Technology |
|-------|------------|
| UI Framework | DuiLib_Ultimate (C++17) |
| Build Toolchain | Visual Studio 2026 (MSVC 14.51) + CMake 3.20 |
| System APIs | Win32 / COM / DISM / CBS / VSS / Appx |
| Compute Module | Rust 1.74+ (duilibcalc FFI) |
| Image Processing | CxImage |
| Embedded Browser | Microsoft Edge WebView2 |
| Skin Format | Custom XML + bundled image resources |

---

## Repository Contents

| Item | Path | Description |
|------|------|-------------|
| Core library source | `DuiLib/` | DuiLib core (UI controls, layout, styles, resource manager) |
| Third-party deps | `3rd/` | CxImage image decoding, WebView2 SDK, wimgapi |
| Win8Metro app | `Demos/Win8Metro/` | Main program (~200 .cpp source files) |
| Companion DLLs | `Demos/ActivationKit/`, `CBSHost/`, `NCleaner/`, `SkinThemeDll/`, `ThemeManager/` | Five independent modules |
| Rust compute module | `rust/dismcore/` | `duilibcalc` — memory-safe hash / encoding / checksum library |
| Translation templates | `templates/translations/` | Terminology glossary (JSON / YAML) |
| Solution | `DuiLib.sln` | Visual Studio 2026 solution file |
| Release artifact | `dist/Win8Metro_*/` | Portable release package (includes skin, DismData, language packs) |
| Prebuilt libraries | `bin64/`, `lib/` | x64 DLL / LIB outputs |

---

## Build Environment Requirements

### Mandatory

| Component | Minimum Version | Notes |
|-----------|-----------------|-------|
| **Windows** | Windows 10 22H2 / Windows 11 | x64 architecture only (Win32 / ARM rejected) |
| **Visual Studio** | VS 2026 (Preview or newer) | Install "Desktop development with C++" workload |
| **PlatformToolset** | v145 (MSVC 14.51) | All vcxproj files migrated to v145 |
| **Windows SDK** | 10.0.26100.0 or newer | Build scripts probe `10.0.28000.0` → `10.0.26100.0` |
| **Git** | Any | Clone the repository |
| **C++ Standard** | C++17 | `compat` track = C++17, `modern` track = C++23 |

### Optional

| Component | Purpose |
|-----------|---------|
| **CMake** | ≥ 3.20, for building the DuiLib core library only |
| **Rust toolchain** | stable 1.74+, for building the `duilibcalc` compute module |
| **WebView2 Runtime** | Runtime installation, required for WebView2 controls on the modern track |

### VS Workload Selection

Open Visual Studio Installer → Modify → check:

- ✅ **Desktop development with C++**
  - MSVC v145 - VS 2026 C++ x64/x86 build tools
  - Windows 10/11 SDK
  - C++ CMake tools (optional)

---

## Build Instructions

### Option 1: Build Directly with Visual Studio

1. Clone the repository:
   ```bash
   git clone https://github.com/q1130396190-droid/dism-c17-2026.git
   cd dism-c17-2026
   ```

2. Open `DuiLib.sln` with VS 2026

3. Pick a configuration (all are x64):

   | Configuration | Description |
   |---------------|-------------|
   | `Debug x64` | C++17 debug build |
   | `Release x64` | C++17 release build |
   | `Debug23 x64` | C++23 debug build (modern track) |
   | `Release23 x64` | C++23 release build (modern track, WebView2 enabled) |
   | `SDebug / SRelease` | Static library variants |

4. Right-click the `Win8Metro` project → **Set as Startup Project**

5. Build solution (Build → Build Solution)

6. Output lands in the `bin64/` directory — the main executable is `bin64/Win8Metro.exe`

### Option 2: Build DuiLib Core via CMake

CMake builds the DuiLib core library only. Demos must be compiled via `DuiLib.sln`:

```bash
# Visual Studio 2026 generator (recommended)
cmake -G "Visual Studio 18 2026" -A x64 -DDUI_TRACK=modern -B build
cmake --build build --config Release

# Or Ninja (source vcvars64.bat first)
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DDUI_TRACK=modern -B build
cmake --build build
```

Hard constraints enforced by CMake (fatal errors on violation):

- Architecture must be x64
- C++ standard minimum 17
- Windows version minimum Win10 (`_WIN32_WINNT=0x0A00`)

### Option 3: Rust dismcore Submodule (Optional)

`duilibcalc` is an FFI-exported DLL with 25 functions. C++ links against `duilibcalc.dll.lib`. If you need to rebuild from Rust source:

```bash
cd rust/dismcore
cargo build --release
# Output: rust/dismcore/target/release/duilibcalc.dll
# Copy to: bin64/amd64/
```

> **Important**: **Do not** name the DLL `DismCore.dll` — Windows filenames are case-insensitive and will collide with the system DISM component, causing DISM sessions to load this library instead and fail with `0x8007007E`. This repository already uses the name `duilibcalc.dll`.

### One-shot Full Build (Project-supplied Script)

```bash
# First open a VS Developer Command Prompt (or source vcvars64.bat)
msbuild DuiLib.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m
```

Or use the Win8Metro-specific compile script:

```bash
cd Demos\Win8Metro
build_x64.bat
```

---

## Runtime Dependencies

### Prerequisites

| Dependency | Notes |
|------------|-------|
| **OS** | Windows 10 22H2+ or any Windows 11 release (x64) |
| **VC++ Runtime** | Microsoft Visual C++ 2026 Redistributable (x64) — installed with VS or separately |
| **WebView2 Runtime** | [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) — the `amd64/WebView2Loader.dll` probes for this at runtime |
| **WinPE** (optional) | Win8Metro can run inside WinPE — integrate the `amd64/` directory DLLs |

### Portable Deployment

Contents of `dist/Win8Metro_2026.11.1_x64_便携版/` form a complete release package:

```
Win8Metro.exe              Main executable
ThemeManager.dll           Theme manager
amd64/                     Core DLL cluster
  ├── DuiLib.dll           UI library
  ├── duilibcalc.dll       Rust compute module
  ├── SkinThemeDll.dll     Skin engine
  ├── ActivationKit.dll    Activation helper
  ├── CBSHost.dll          DISM host
  ├── NCleaner.dll         Cleaning engine
  ├── WebView2Loader.dll   Edge WebView2 bridge
  ├── wimgapi.dll          WIM API
  ├── bcdboot.exe          BCD boot
  ├── efisys.bin           EFI system
  ├── etfsboot.com         ETFS boot
  └── wofadk.sys           WOF driver
skin/                      UI skins (Classic / Win8Metro)
lang/                      Language packs (cs, de, en-US, fr, hu, it, ja-JP, ko-KR, pl-PL, ru, zh-CN)
DismData/                  DISM data config
Config/                    Default config
```

**To launch**: double-click `Win8Metro.exe` — no installer required.

### Running During Development

After compiling, place `bin64/amd64/` DLLs alongside `bin64/Win8Metro.exe`. For proper UI rendering and localization, also copy the `skin/`, `lang/`, `DismData/`, and `Config/` directories — otherwise the UI shows missing-skin errors and `[zh-CN:xxx]` placeholders.

---

## AI-Assisted Development

This project — from C++17 refactoring and CMake migration to Win8Metro feature builds, packaging, and documentation — was developed end-to-end with **AI pair-programming**. Below is a transparent disclosure of every AI model that contributed to the codebase and what it worked on.

### Participating AI Models

| AI Model | Type | Primary Areas of Contribution | Est. Share |
|----------|------|-------------------------------|------------|
| **Trae (ByteDance)** | IDE built-in AI coding assistant | End-to-end lead: CMake rewrite, vcxproj v145 upgrade, x64-only architecture (removed Win32/ARM), C++17 modern track migration, Rust dismcore FFI bindings, Win8Metro feature pages, packaging scripts, bilingual README, GitHub Release publishing | ≈ 70% |
| **DeepSeek-V3** | Online chat LLM | C++ template metaprogramming tuning, CRTP base-class inheritance refactoring, Win32 API wrapper deduplication, Rust → C++ FFI signature derivation | ≈ 15% |
| **GPT-4o** | Online chat LLM | DuiLib Stylesheet XML schema design, StringMgr runtime localization hot-swap, WebView2 modern-track API adaptation | ≈ 8% |
| **Claude 3.5 Sonnet** | Online chat LLM | CBS hosting-layer exception handling, WinPE compatibility code, Sysprep unattend XML template generation | ≈ 5% |
| **qwen-plus** | Online chat LLM | Chinese README polishing, known-issues table curation, technology-stack copy editing | ≈ 2% |

> **Note**: Models were accessed via Trae IDE plugins, Browser MCP, and CLI chat. All AI-generated code underwent human review and local compilation. No "black-box" untraceable generation was introduced.

### AI Milestones

| Milestone | Date | Models Involved | Output |
|-----------|------|-----------------|--------|
| DuiLib vcxproj migrated to v145 | 2026-10 | Trae + DeepSeek-V3 | 12 .vcxproj files batch-upgraded PlatformToolset |
| CMakeLists.txt rewrite | 2026-10 | Trae | Native x64-only / C++17 guard (`FATAL_ERROR` when arch ≠ x64) |
| Rust dismcore FFI | 2026-10 | Trae + DeepSeek-V3 | `duilibcalc.dll` 25 exported functions + C++ header bindings |
| 15 Win8Metro feature pages | 2026-11 | Trae + GPT-4o | page_cleanup / page_driver / page_tools etc. (XML + C++ logic) |
| Bilingual README | 2026-11 | Trae + qwen-plus | Chinese + English README with architecture diagram |
| GitHub repo / Release | 2026-11 | Trae (gh CLI) | Repo creation / public visibility / Release v2026.11.1 |

### Human Review Gate

AI-generated code must satisfy all five checks before merge:

1. ✅ **Line-by-line human diff review** — no bulk commits
2. ✅ **Local build passes** — both MSBuild and CMake, zero warnings
3. ✅ **Functional smoke test** — Win8Metro startup, cleanup, driver scan
4. ✅ **No security risks** — Rust FFI boundary checks, registry op whitelist, DLL names avoid system conflicts
5. ✅ **Traceable** — git commit message marks AI involvement (e.g. `[AI-assisted] CMakeLists.txt rewrite`)

---

## Known Issues

| # | Issue | Impact | Workaround |
|---|-------|--------|------------|
| ⚠️ | **Cannot launch on Windows 10** | Confirmed bug: Win8Metro exits immediately (no window) after double-clicking on Windows 10 (all versions including 22H2 and earlier) | **Under active investigation** — preliminary root cause points to a WebView2Loader + DuiLib FrameWnd initialization-order race. Temporary workaround: run on Windows 11, or compile with `Release x64` (not `Release23`) |
| 1 | **No CMakeLists.txt in Demos** | CMake skips all Demo projects and builds the DuiLib core library only | Use `DuiLib.sln` to compile Demos; CMake is core-library-only |
| 2 | **Inconsistent source encoding** | Repo mixes UTF-8 / UTF-8-BOM / GBK / UTF-16LE (~49 GBK files) | Confirm encoding with your editor before editing; recommend unified UTF-8 |
| 3 | **Escape characters expanded by patch scripts** | `tools/patch_*.py` scripts silently corrupt escape sequences (paths lose one char, etc.) | Always diff after edits; back up first |
| 4 | **x64-only enforcement** | Compilation on x86 / ARM / ARM64 is rejected | Must have x64 CPU + x64 Windows |
| 5 | **VS generator not found** | `cmake -G "Visual Studio 18 2026"` fails with `could not find any instance of Visual Studio` | Use Ninja (source `vcvars64.bat` first), or repair VS via Installer |
| 6 | **duilibcalc linking** | Manual Win8Metro links fail if the Rust module has not been compiled (`duilibcalc.dll.lib not found`) | Run `cargo build --release` first, copy `duilibcalc.dll.lib` to `bin64/amd64/` |
| 7 | **Windows 7 / 8 not supported** | `_WIN32_WINNT=0x0A00` is hardcoded | Windows 10 or newer required |
| 8 | **Portable runtime missing** | Target machine without VC++ Runtime or WebView2 Runtime fails with missing-DLL errors | Install VC++ 2026 Redistributable and WebView2 Runtime |
| 9 | **WebView2 Loader** | Win10 21H2 and older do not ship WebView2 Runtime preinstalled | `WebView2Loader.dll` attempts auto-download on first launch; install manually if it fails |
| 10 | **Missing language packs** | UI shows `[zh-CN:xxx]` placeholders if the `lang/` directory is absent | Always deploy with the full `lang/` directory |

---

## Directory Structure

```
dism-c17-2026/
├── .gitignore
├── CMakeLists.txt              # CMake root (DuiLib core only)
├── DuiLib.sln                  # VS 2026 solution (everything)
├── Directory.Build.props       # Global /MP multi-core compilation
├── build.cmd                   # One-shot build helper
├── README.md                   # Chinese documentation
├── README_EN.md                # This file
├── LICENSE
│
├── DuiLib/                     # Core UI library source
│   ├── Core/ Control/ Layout/ Utils/ Style/
│   ├── DuiLib.vcxproj
│   └── WebView2.h              # WebView2 integration interface
│
├── 3rd/                        # Third-party dependencies
│   ├── CxImage/                # Image decoding
│   ├── WebView2/               # Edge WebView2 SDK
│   └── wimgapi/                # WIM processing
│
├── Demos/                      # Application source
│   ├── Win8Metro/              # Main program (~200 .cpp)
│   ├── ActivationKit/          # Activation helper DLL
│   ├── CBSHost/                # DISM host DLL
│   ├── NCleaner/               # Cleaning engine DLL
│   ├── SkinThemeDll/           # Skin engine DLL
│   └── ThemeManager/           # Theme manager DLL
│
├── rust/
│   └── dismcore/               # Rust compute module (duilibcalc)
│       ├── Cargo.toml
│       └── src/
│
├── templates/translations/     # Translation glossary
│
├── bin64/                      # Build output
│   ├── Win8Metro.exe           # Main executable
│   ├── ThemeManager.dll        # Theme manager
│   ├── amd64/                  # Core DLLs / LIBs
│   └── lang/                   # Language pack DLLs
│
├── lib/                        # Static library output
│
└── dist/
    └── Win8Metro_2026.11.1_x64_便携版/   # Full portable release package
```

---

## License

The source code in this repository is released under the **MIT License**. See the `LICENSE` file for details.

Third-party components bundled with the Win8Metro portable release package (WebView2 Runtime, WOFADK.SYS, bcdboot.exe, efisys.bin, etc.) retain their respective licenses.
