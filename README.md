<p align="center">
  <img src="motion_logo.png" alt="Motion CLI" width="400">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-1.0.0-blue?style=flat-square" alt="Version">
  <img src="https://img.shields.io/badge/platform-Windows_10%2F11-lightgrey?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/license-MIT-green?style=flat-square" alt="License">
  <img src="https://img.shields.io/badge/C++-17-blue?style=flat-square" alt="C++17">
  <img src="https://img.shields.io/github/stars/tenzo/motioncli?style=flat-square&color=yellow" alt="Stars">
</p>

<p align="center">
  A super-lightweight live wallpaper engine for Windows.<br>
  Keyboard-navigable terminal UI. No Electron. No browser. Just Media Foundation.
</p>

---

## Why Motion CLI?

| Tool | Per-wallpaper runtime | RAM usage |
|------|----------------------|-----------|
| Wallpaper Engine | Chromium webview | ~200-400 MB |
| Lively Wallpaper | .NET + Chromium | ~150-300 MB |
| **Motion CLI** | Single MFPlay sink | **~80-120 MB** |

One small detached process. Hardware-decoded video. No JS engine, no compositor, no per-frame CPU churn.

## Features

<img src="https://img.shields.io/badge/TUI-WASD%20%2F%20Arrow%20keys-0d1117?style=for-the-badge&labelColor=238636" alt="TUI">
<img src="https://img.shields.io/badge/Library-MoeWalls_1000%2B-0d1117?style=for-the-badge&labelColor=1f6feb" alt="Library">
<img src="https://img.shields.io/badge/Preview-In--console-0d1117?style=for-the-badge&labelColor=8957e5" alt="Preview">
<img src="https://img.shields.io/badge/Cache-Download_1x-0d1117?style=for-the-badge&labelColor=da3633" alt="Cache">
<img src="https://img.shields.io/badge/Per--Monitor-Yes-0d1117?style=for-the-badge&labelColor=f0883e" alt="Per-Monitor">
<img src="https://img.shields.io/badge/Auto--Pause-Fullscreen%20%2F%20Maximized-0d1117?style=for-the-badge&labelColor=3fb950" alt="Auto-Pause">
<img src="https://img.shields.io/badge/Import-MP4%20%2F%20MOV%20%2F%20WMV-0d1117?style=for-the-badge&labelColor=58a6ff" alt="Import">
<img src="https://img.shields.io/badge/Tray-Mute%20%2F%20Stop-0d1117?style=for-the-badge&labelColor=8b949e" alt="Tray">
<img src="https://img.shields.io/badge/Autostart-Registry-0d1117?style=for-the-badge&labelColor=2ea043" alt="Autostart">
<img src="https://img.shields.io/badge/Low--End--Mode-Auto%20tune-0d1117?style=for-the-badge&labelColor=f85149" alt="Low-End">

## Getting Started

### Install

**winget:**
```bash
winget install tenzo.motioncli
```

**Installer:**
Download `motioncli-setup.exe` from [Releases](https://github.com/tenzo/motioncli/releases). Run it, pick your install path, done.

**Portable:**
Download `motioncli.exe` from [Releases](https://github.com/tenzo/motioncli/releases). Run it directly -- no install needed.

### Build from source

**Prerequisites:** Visual Studio 2022+ with CMake support, Windows 10/11 SDK.

**Option A -- Visual Studio:**
1. `File > Open > Folder` and select the `motioncli` directory
2. VS auto-configures CMake -- pick the `motioncli.exe` target
3. Press `F5`

**Option B -- Command line:**
```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable will be at `build/Release/motioncli.exe`.

> Links only against Windows SDK libraries (`winhttp`, `mfplay`, `mf`, `mfplat`, `mfuuid`, `shlwapi`, `ole32`, `shell32`, `comdlg32`, `dxgi`, `windowscodecs`). No third-party dependencies.

## Usage

```
motioncli
```

On first launch a short guided intro walks you through everything.

### Main Menu

```
  Browse Library     ->  online library (search, tabs, preview)
  My Wallpapers      ->  everything you've saved or imported
  Per-monitor setup  ->  assign a different wallpaper to each screen
  Active Wallpaper   ->  stop, restart, mute the current wallpaper
  Settings           ->  performance, wallpapers-per-load, start-on-login
  Quit
```

### Browse Library

| Key | Action |
|-----|--------|
| `W`/`S` or Up/Down | Move |
| `A`/`D` | Switch category tab (anime, games, landscape, ...) |
| `/` | Search anything (naruto, sunset, cyberpunk, ...) |
| `R` | Refresh |
| `Enter` | Open, then **Download & apply** |
| `Esc` | Back |

### Performance Settings

Navigate to **Settings > Performance** to configure:

| Setting | Description |
|---------|-------------|
| **Detect my PC (auto-tune)** | Probes CPU, RAM, and GPU VRAM via DXGI, sets quality automatically |
| **Quality** | Auto / High / Medium / Low |
| **Pause when fullscreen** | Freezes a screen when an app goes fullscreen on it |
| **Pause when maximized** | Also freezes under maximized windows |
| **Pause when app focused** | Freezes whenever any app is focused |
| **Pause on battery** | Saves power when unplugged |
| **Low-end mode** | Aggressive pausing for weaker hardware |
| **Playback speed** | 0.5x to 2.0x |
| **Deep sleep after** | Fully releases resources after N seconds of being paused |

### System Tray

Right-click the tray icon while a wallpaper is live:

```
  Motion CLI -- live wallpaper
  ----------------------------
  Mute audio
  Open Motion CLI...
  ----------------------------
  Stop wallpaper
```

### Start on login

Toggle **Settings > Start on login**. Adds a per-user `Run` registry entry
(`HKCU\...\Run\MotionCLI = "<exe>" --startup`). No service, no admin rights.

### CLI Modes

```
motioncli              # launch the TUI
motioncli --render     # headless renderer (UI-launched)
motioncli --startup    # used by the login auto-start entry
```

## Project Structure

```
motioncli/
  CMakeLists.txt
  motion_logo.png
  src/
    main.cpp                entry point
    app/                    application flow, menus
    core/                   config, hardware, library, wallpaper engine
    net/                    WinHTTP client, file download
    tui/                    terminal, menus, in-console image preview
    util/                   JSON parser/serializer
  resources/                icons, .rc files
  installer/                NSIS installer script
  winget/                   winget manifest files
  public/                   release binaries
```

## Contributing

Contributions are welcome. Fork the repo, make your changes, and open a pull request.

If you fork this project, you **must** disclose that it is a fork:

> "Based on Motion CLI by tenzo (https://github.com/tenzo/motioncli)"

See [LICENSE](LICENSE) for full attribution requirements.

## Credits

Wallpapers are sourced from [MoeWalls](https://moewalls.com). All rights to the wallpapers belong to their respective creators. Motion CLI is an independent client and is not affiliated with MoeWalls.

## License

MIT -- (c) 2026 tenzo. See [LICENSE](LICENSE).

Forks and derivative works must retain attribution to the original source.
