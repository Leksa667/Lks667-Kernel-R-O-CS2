# Lks667 Kernel RO CS2

![C++](https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=cplusplus)
![Windows](https://img.shields.io/badge/Windows-x64-0078D4?style=for-the-badge&logo=windows11)
![Build](https://img.shields.io/badge/build-Release-2EA44F?style=for-the-badge)
![Offsets](https://img.shields.io/badge/offsets-auto--update-brightgreen?style=for-the-badge)
![Author](https://img.shields.io/badge/by-Leksa667-8A2BE2?style=for-the-badge)

An experimental Windows project combining a native user interface, a GDI overlay, and a kernel component. It is designed for local sessions and offline testing environments.

> [!WARNING]
> This repository is provided for educational purposes and local testing only. Using it on public, competitive, or anti-cheat-protected servers may violate the game's terms of service. You are solely responsible for how you use it.

## Highlights

- Automatic offset updates after CS2 updates.
- Version-aware offset cache based on the `client.dll` fingerprint.
- Automatic schema and offset dump whenever the cached version becomes outdated.
- Compact dark interface toggled with `F3`.
- Native x64 user-mode application and kernel component.
- Config profiles automatically discovered from the Documents folder.


<img width="1763" height="724" alt="image" src="https://github.com/user-attachments/assets/ce6b4b9d-ad7d-4722-a5f9-7ba850c887d7" />


## Features

### Interface

- Compact dark menu toggled with `F3`.
- Organized Aim, ESP, Misc, and Config tabs.
- Configuration profiles stored in `Documents\Lks667\Configs`.
- Automatic discovery and loading of available `.cfg` files.

### Player ESP

- Box ESP.
- Thin, adaptive skeleton.
- Compact head ESP.
- Player names.
- Red health bars and blue armor bars.
- Separate colors for visible and hidden targets.
- Team check and visible-only filtering.

### World ESP

- Weapons and equipment that are actually dropped on the ground.
- Ground grenades without displaying active or thrown projectiles.
- Planted bomb and C4 carrier identification.
- Unified explosion and defuse timer bar.
- Defuse kit detection and timing.

### Aim

- Independent enable switch.
- Configurable aim key.
- Adjustable FOV and customizable FOV circle.
- Adjustable smoothing.
- Team and visibility filtering.

## Automatic Offset Updates

Lks667 does not rely on a permanently hardcoded offset list. On startup, it fingerprints the current `client.dll` and checks the local offset cache.

- If the fingerprint matches, the cached offsets are loaded immediately.
- If CS2 has been updated and the fingerprint has changed, the project automatically analyzes the current modules and schemas again.
- The refreshed offsets are saved to `lks_offsets.dat` for faster future launches.

This auto-update system concerns game offsets. New application releases and source-code updates are still installed manually from the repository.

## Project Structure

```text
Lks_CS2_KM_UM/
|-- Lks_UM/                    # Application, interface, and overlay
|   |-- include/cs2dumper/     # Schema and offset analysis
|   `-- third_party/kdmapper/  # Third-party mapping dependency
|-- Lks_KernelDriver/          # Kernel component and shared protocol
|-- x64/Release/               # Release build outputs
|-- Lks_KernelDriver.sln
`-- README.md
```

## Build Requirements

- Windows 10 or Windows 11 x64.
- Visual Studio 2022 with Desktop development with C++.
- MSVC v143.
- Windows SDK and Windows Driver Kit (WDK).
- The `Release | x64` configuration is recommended.

## Building

1. Open `Lks_Kernel.sln` with Visual Studio 2022.
2. Select `Release` and `x64`.
3. Build the `Lks_KernelDriver` project.
4. Open `Lks_UM/Lks_UM.vcxproj`.
5. Build it using `Release | x64`.
6. Check `x64/Release` for:
   - `Lks_KernelDriver.sys`
   - `Lks_UM.exe`

## Local Usage

1. Keep `Lks_UM.exe` and `Lks_KernelDriver.sys` in the same directory.
2. Start a local test session.
3. Run the application as administrator.
4. Press `F3` to show or hide the menu.
5. Close the application normally to trigger the intended resource cleanup.

The manually mapped kernel image is not fully released until the next reboot. This prevents unsafe kernel-memory cleanup while an execution path could still reference the image.

## Configuration Profiles

Profiles are stored in:

```text
Documents\Lks667\Configs
```

Profile names can be entered directly in the menu. The `.cfg` extension is added automatically when omitted.

## Dependencies and Credits

- `kdmapper` is a third-party component stored in `Lks_UM/third_party/kdmapper`. Its original license is included in that directory.
- The `cs2dumper` module is isolated in `Lks_UM/include/cs2dumper` and retains its project information.
- Application files carrying the `By Leksa667 - 12/08/2026` header belong to the Lks667 project.

## Author

Developed by **Leksa667** — August 12, 2026.
