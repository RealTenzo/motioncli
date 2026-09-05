<p align="center">
  <img src="motion_logo.png?v=1.3.5" alt="Motion CLI Logo" width="380">
</p>

<h1 align="center">Motion CLI</h1>

<p align="center">
  <strong>Native, hardware-accelerated live wallpaper engine and terminal manager for Windows 10 & 11.</strong><br>
  Zero Chromium. Zero Electron. Zero webview overhead.<br>
  Built in C++17 with Direct3D 11 and Microsoft Media Foundation.
</p>

<p align="center">
  <a href="https://github.com/RealTenzo/motioncli/releases"><img src="https://img.shields.io/badge/version-1.3.5-blue?style=flat-square" alt="Version"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-lightgrey?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/license-MIT-green?style=flat-square" alt="License">
  <img src="https://img.shields.io/badge/language-C%2B%2B17-00599C?style=flat-square" alt="C++17">
  <a href="https://github.com/RealTenzo/motioncli/stargazers"><img src="https://img.shields.io/github/stars/RealTenzo/motioncli?style=flat-square&color=yellow" alt="GitHub Stars"></a>
</p>

---

Motion CLI is a minimal, keyboard-driven live wallpaper application designed to replace heavy Chromium/.NET-based wallpaper engines. It runs as a detached native Windows process that renders directly to the desktop background behind your icons using Windows `WorkerW` hierarchy and native GPU video decoding.

## Benchmarks & Resource Footprint

Measurements taken during active 1080p60 H.264 video wallpaper playback on Windows 11 (Intel Core i7 / NVIDIA RTX 3060):

| Engine | Technology Stack | Working Set (RAM) | Active GPU Usage | Paused GPU / CPU |
| :--- | :--- | :--- | :--- | :--- |
| **Wallpaper Engine** | Chromium Embedded Framework (CEF) | ~200 – 450 MB | ~5% – 12% | Idle webview overhead |
| **Lively Wallpaper** | .NET 8 / C# + WebView2 | ~180 – 350 MB | ~6% – 15% | Runtime memory retention |
| **Motion CLI** | **Native C++17 + Direct3D 11 / MediaEngine** | **~35 – 70 MB** | **< 1% (NVDEC / VCN)** | **0% CPU / 0% GPU** |

* Video frames are decoded directly into DXGI swap chain surfaces via native hardware decoders (NVDEC, Intel QuickSync, AMD VCN).
* Zero per-frame memory allocations or JavaScript runtime garbage collection pauses.

---

## Core Capabilities

- **DirectX 11 Desktop Canvas**: Attaches directly to the Windows `Progman` / `WorkerW` split window layer via undocumented Win32 messaging (`0x052C`), placing fluid video rendering behind desktop icons without capturing focus or stealing input.
- **Hardware Decode Acceleration**: Utilizes Windows Media Foundation (`IMFMediaEngine`) with DXGI device management for direct-to-GPU video frame presentation.
- **Smart Auto-Pause Engine**: Dynamic window occlusion checks monitor foreground processes. Automatically pauses playback when games or applications are maximized, fullscreen, or focused to eliminate performance loss during gaming and compilation.
- **Multi-Monitor Layouts**: Supports desktop spanning across displays or distinct per-monitor wallpaper assignments with independent playback controls.
- **Keyboard-Driven Terminal UI (TUI)**: Fast in-terminal navigation using WASD or arrow keys with real-time ASCII/ANSI previews, search filtering, and configuration menus.
- **System Tray Integration**: Native lightweight Win32 system tray notification icon with instant pause/resume, audio muting, auto-pause mode switching, and double-click TUI summoning.
- **Online Library & Local Importer**: Browse and stream from catalog libraries (such as MoeWalls) or drop in local `.mp4`, `.mov`, and `.wmv` files.
- **Background Autostart**: One-key registry autostart toggle (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`) with detached headless engine launch.

---

## Installation

### 1. Universal Web Installer (Recommended)
Download and run [**`MotionCLI-Installer.exe`**](https://github.com/RealTenzo/motioncli/releases/latest/download/MotionCLI-Installer.exe) from the latest release.
- Automatically places the binary in `%LOCALAPPDATA%\Programs\MotionCLI`
- Creates Start Menu and Desktop shortcuts
- Appends `motioncli` to your user `PATH` for direct terminal access

### 2. Standalone Portable
Download [**`motioncli_portable.exe`**](https://github.com/RealTenzo/motioncli/releases/latest/download/motioncli_portable.exe).
- Self-contained single executable
- Stores configuration and downloaded wallpapers relative to the executable directory
- No installation or administrative privileges required

---

## Quick Start & CLI Usage

Open PowerShell, Command Prompt, or Windows Terminal:

```bash
# Launch interactive terminal UI
motioncli

# Headless renderer mode (used internally or by autostart)
motioncli --render
```

### Navigation Keys
| Key | Action |
| :--- | :--- |
| `↑` / `W` | Move selection up |
| `↓` / `S` | Move selection down |
| `Enter` / `Space` | Select / toggle / apply |
| `Esc` / `Q` | Back to previous menu / Cancel |
| `/` | Filter search queries |

---

## Auto-Pause Modes

Configurable through **Settings > Performance > Auto-pause mode** or directly from the **System Tray Menu**:

| Mode | Behavior | Recommended Use Case |
| :--- | :--- | :--- |
| **Maximized + Fullscreen** *(Default)* | Keeps playing during normal multitasking; pauses when any window is maximized or fullscreen. | Optimal balance of aesthetics and GPU saving |
| **Fullscreen Only** | Keeps playing behind maximized windows; pauses strictly during fullscreen games or media. | Multi-monitor stations and transparent terminal themes |
| **Foreground Focused** | Pauses whenever any non-desktop window has focus. | Maximum battery saving on laptops |
| **Never Pause** | Unconditional playback regardless of window states. | Showcase displays and dedicated wallpaper rigs |

---

## Architecture & Internals

```
┌─────────────────────────────────────────────────────────────┐
│                         motioncli                           │
│  ┌──────────────────────┐         ┌──────────────────────┐  │
│  │   Interactive TUI    │         │  Tray Menu Listener  │  │
│  │  (WASD / Arrow Keys) │         │   (Shell_NotifyIcon) │  │
│  └──────────┬───────────┘         └──────────┬───────────┘  │
│             │                                │              │
│             ▼                                ▼              │
│  ┌───────────────────────────────────────────────────────┐  │
│  │                   Config & State                      │  │
│  │   (config.json · library.json · Win32 Named Mutex)    │  │
│  └──────────────────────────┬────────────────────────────┘  │
└─────────────────────────────┼───────────────────────────────┘
                              │ IPC / Event Flags
                              ▼
┌─────────────────────────────────────────────────────────────┐
│               motioncli --render (Detached)                 │
│                                                             │
│   Windows Desktop (Progman) ──> Spawn WorkerW Layer         │
│                                           │                 │
│   IMFMediaEngine ──(Hardware Decoded)───> │ DXGI Swapchain  │
│   (NVDEC / QuickSync / VCN)               ▼ Render Output   │
│                                      Behind Desktop Icons   │
└─────────────────────────────────────────────────────────────┘
```

- **WorkerW Injection**: Sends message `0x052C` to `Progman` to spawn an invisible worker window between the icon view window (`SHELLDLL_DefView`) and the desktop background, hosting the Direct3D swapchain.
- **Occlusion Detection**: Uses `GetForegroundWindow`, monitor geometry checks, and desktop shell state detection via Win32 APIs without poll-heavy hooks.
- **WinHTTP Pipeline**: Direct asynchronous HTTP client handling updates and catalog streaming with redirect following and TLS 1.2+ security.

---

## Building from Source

### Prerequisites
* Windows 10 (Build 19041+) or Windows 11
* Visual Studio 2022 (Community or higher) with:
  * **Desktop development with C++**
  * **C++ CMake tools for Windows**
  * **Windows 10/11 SDK**

### 1-Click Release Build
Run the provided build batch script:
```cmd
build_release.bat
```
Build artifacts will be compiled and packaged into the `dist/` directory:
- `dist\motioncli_portable.exe`
- `dist\MotionCLI-Installer.exe`
- `dist\version.json`

### Manual CMake Build
```cmd
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

---

## Repository Structure

```text
motioncli/
├── CMakeLists.txt          # Root CMake build configuration
├── build_release.bat       # Automated release compilation and packager
├── version.json            # Version tracking & manifest endpoint
├── src/
│   ├── main.cpp            # Entry point & CLI argument routing
│   ├── app/                # Application state machine, menus, and update UX
│   ├── core/               # Engine controller, Direct3D 11 backend, autostart
│   ├── net/                # WinHTTP client, catalog downloader, updater
│   ├── tui/                # Virtual terminal rendering, ANSI buffers, image viewer
│   └── util/               # Minimal JSON parser, UTF-8/wide string helpers
├── installer/              # Standalone interactive installer & NSIS scripts
└── resources/              # Icons (.ico), application manifests, and .rc scripts
```

---

## Credits & Disclaimer

* Wallpapers are indexed from [MoeWalls](https://moewalls.com). All video and artwork rights remain with their respective artists and creators. Motion CLI is an independent, non-commercial client and is not affiliated with MoeWalls.

### DMCA & Content Removal
If you are an artist, animator, content creator, or representative from MoeWalls and wish to request the removal of any content, stream link, or indexed wallpaper, **please reach out to me directly on Discord (`@tenzo_aoki`) or open a GitHub Issue before filing a formal DMCA takedown**. 

I respect all intellectual property and will immediately remove the requested listings and resolve the issue as fast as possible.

---

## License

Distributed under the [MIT License](LICENSE). Copyright © 2026 tenzo.
