# Motion CLI

<p align="center">
  <img src="motion_logo.png?v=1.4.0" alt="Motion CLI Logo" width="380">
</p>

<h1 align="center">Motion CLI</h1>

<p align="center">
  <strong>Native, hardware-accelerated live wallpaper engine and terminal manager for Windows 10 & 11.</strong><br>
  Zero Chromium. Zero Electron. Zero webview overhead.<br>
  Built in C++17 with Direct3D 11 and Microsoft Media Foundation.
</p>

<p align="center">
  <a href="https://github.com/RealTenzo/motioncli/releases"><img src="https://img.shields.io/badge/version-1.4.0-blue?style=flat-square" alt="Version"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-lightgrey?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/license-MIT-green?style=flat-square" alt="License">
  <img src="https://img.shields.io/badge/language-C%2B%2B17-00599C?style=flat-square" alt="C++17">
  <a href="https://github.com/RealTenzo/motioncli/stargazers"><img src="https://img.shields.io/github/stars/RealTenzo/motioncli?style=flat-square&color=yellow" alt="GitHub Stars"></a>
</p>

---

Motion CLI is a minimal, keyboard-driven live wallpaper application designed to replace heavy Chromium/.NET-based wallpaper engines. It runs as a detached native Windows process that renders directly to the desktop background behind your icons using the Windows `WorkerW` hierarchy and native GPU video decoding.

## Benchmarks & Resource Footprint

Measurements taken during active 1080p60 H.264 video wallpaper playback on Windows 11 (Intel Core i7 / NVIDIA RTX 3060).

| Engine | Technology Stack | Working Set (RAM) | Active GPU Usage | Paused GPU / CPU |
| :--- | :--- | :--- | :--- | :--- |
| **Wallpaper Engine** | Chromium Embedded Framework (CEF) | ~200–450 MB | ~5%–12% | Idle webview overhead |
| **Lively Wallpaper** | .NET 8 / C# + WebView2 | ~180–350 MB | ~6%–15% | Runtime memory retention |
| **Motion CLI** | **Native C++17 + Direct3D 11 / MediaEngine** | **~10–20 MB (target)** | **~8–10% (reported after optimization)** | **Optimized idle behavior** |

- Video frames are decoded directly into DXGI swap chain surfaces via native hardware decoders where supported (NVDEC, Intel QuickSync, AMD VCN).
- Designed to minimize per-frame memory allocations and avoid JavaScript runtime garbage collection overhead.

*Actual resource usage varies depending on hardware, wallpaper resolution, video codec, and playback state.*

---

## Core Capabilities

- **DirectX 11 Desktop Canvas**: Attaches directly to the Windows `Progman` / `WorkerW` window hierarchy via Win32 messaging (`0x052C`), placing video rendering behind desktop icons without capturing focus or stealing input.
- **Hardware Decode Acceleration**: Utilizes Windows Media Foundation (`IMFMediaEngine`) with DXGI device management for GPU-accelerated video presentation.
- **Smart Auto-Pause Engine**: Monitors foreground windows and their states. Automatically pauses playback when games or applications are maximized, fullscreen, or focused according to the selected mode.
- **Multi-Monitor Layouts**: Supports desktop spanning across displays or distinct per-monitor wallpaper assignments with independent playback controls.
- **Keyboard-Driven Terminal UI (TUI)**: Fast in-terminal navigation using WASD or arrow keys with real-time ASCII/ANSI previews, search filtering, and configuration menus.
- **System Tray Integration**: Native lightweight Win32 system tray notification icon with instant pause/resume, audio muting, auto-pause mode switching, and double-click TUI summoning.
- **Online Library & Local Importer**: Browse and stream from catalog libraries such as MoeWalls or use local `.mp4`, `.mov`, and `.wmv` files.
- **Background Autostart**: One-key registry autostart toggle (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`) with detached headless engine launch.

---

## Roadmap & Contributor Goals

Here is what I want to work on next, in priority order. Motion CLI is an independent project, so contributions, testing, ideas, and even sharing the project help a lot.

### Performance

- [x] **Optimize GPU usage**: Reduced reported GPU usage from around 30–40% to 8–10%.
- [x] **Optimize RAM usage**: Memory optimization work targeting a footprint of around 10–20 MB.
- [x] **Automatic idle optimization**: Implemented automatic resource optimization during idle or paused playback.
- [ ] **Further performance improvements**: Continue reducing CPU, GPU, and memory usage without compromising playback quality.

### Development Priorities

- [ ] **Native C++ UI (Highest Priority)**: Replace the current terminal-based interface with a proper native C++ UI. Keep it lightweight, responsive, and consistent with the rest of the application. No Electron, Chromium, or WebView runtime.
- [ ] **Support more Windows versions**: Expand compatibility beyond Windows 10 and Windows 11 where technically possible. Investigate older Windows APIs and implement compatibility handling where practical.
- [ ] **Build our own wallpaper library and API**: Develop an independent wallpaper catalog and our own API for browsing, searching, retrieving, and managing wallpaper metadata. Reduce reliance on third-party catalogs over time.

### Community Goals

- [ ] **Reach 20 GitHub stars**: A small milestone, but it would mean a lot! If Motion CLI is useful to you, consider giving the repository a star.
- [ ] **Free promotion**: Help spread the word through YouTube videos, social media, developer communities, or recommendations. I'm building this on a budget of basically zero, so free exposure is greatly appreciated.

### Want to Contribute?

You don't have to tackle an entire feature to help. Bug fixes, Windows compatibility testing, performance improvements, UI development, documentation, and useful ideas are all welcome.

Open an issue to discuss a feature or submit a pull request. If you cannot contribute code, sharing the project is just as appreciated.

Repository: https://github.com/RealTenzo/motioncli

---

## Installation

### 1. Universal Web Installer (Recommended)

Download and run [`MotionCLI-Installer.exe`](https://github.com/RealTenzo/motioncli/releases/latest/download/MotionCLI-Installer.exe) from the latest release.

- Automatically places the binary in `%LOCALAPPDATA%\Programs\MotionCLI`
- Creates Start Menu and Desktop shortcuts
- Appends `motioncli` to your user `PATH` for direct terminal access

### 2. Standalone Portable

Download [`motioncli_portable.exe`](https://github.com/RealTenzo/motioncli/releases/latest/download/motioncli_portable.exe).

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

Configurable through **Settings > Performance > Auto-pause mode** or directly from the **System Tray Menu**.

| Mode | Behavior | Recommended Use Case |
| :--- | :--- | :--- |
| **Maximized + Fullscreen** *(Default)* | Keeps playing during normal multitasking; pauses when any window is maximized or fullscreen. | Optimal balance of aesthetics and GPU saving |
| **Fullscreen Only** | Keeps playing behind maximized windows; pauses strictly during fullscreen games or media. | Multi-monitor stations and transparent terminal themes |
| **Foreground Focused** | Pauses whenever any non-desktop window has focus. | Maximum battery saving on laptops |
| **Never Pause** | Unconditional playback regardless of window states. | Showcase displays and dedicated wallpaper rigs |

---

## Architecture & Internals

```text
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

- **WorkerW Injection**: Sends message `0x052C` to `Progman` to spawn an invisible worker window between the icon view window (`SHELLDLL_DefView`) and the desktop background, hosting the Direct3D swap chain.
- **Occlusion Detection**: Uses `GetForegroundWindow`, monitor geometry checks, and desktop shell state detection via Win32 APIs.
- **WinHTTP Pipeline**: Direct asynchronous HTTP client handling updates and catalog streaming with redirect following and TLS 1.2+ security.

---

## Building from Source

### Prerequisites

- Windows 10 (Build 19041+) or Windows 11
- Visual Studio 2022 (Community or higher) with:
  - **Desktop development with C++**
  - **C++ CMake tools for Windows**
  - **Windows 10/11 SDK**

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
│   ├── main.cpp            # Entry point and CLI argument routing
│   ├── app/                # Application state machine, menus, and update UX
│   ├── core/               # Engine controller, Direct3D 11 backend, autostart
│   ├── net/                # WinHTTP client, catalog downloader, updater
│   ├── tui/                # Virtual terminal rendering, ANSI buffers, image viewer
│   └── util/               # Minimal JSON parser, UTF-8/wide string helpers
├── installer/              # Standalone interactive installer and NSIS scripts
└── resources/              # Icons (.ico), application manifests, and .rc scripts
```

---

## Credits & Disclaimer

- Wallpapers are indexed from [MoeWalls](https://moewalls.com). All video and artwork rights remain with their respective artists and creators. Motion CLI is an independent, non-commercial client and is not affiliated with MoeWalls.

### DMCA & Content Removal

If you are an artist, animator, content creator, or representative from MoeWalls and wish to request the removal of any content, stream link, or indexed wallpaper, **please reach out to me directly on Discord (`@tenzo_aoki`) or open a GitHub Issue before filing a formal DMCA takedown**.

I respect all intellectual property and will remove requested listings and work to resolve the issue as quickly as possible.

---

## License

Distributed under the [MIT License](LICENSE). Copyright © 2026 tenzo.
