# <img src="assets/brand/orbelio-icon.png" width="40" height="40" alt="" /> Orbelio

**English** · [简体中文](README.zh-CN.md)

Orbelio is a Windows viewer for 3D and 4D Gaussian Splatting. It combines a Qt Quick interface with an independent Vulkan renderer. Separating scene data from the rendering implementation lets static models, frame sequences, and continuous-time scenes share one viewport.

[Features](#features) · [Quick start](#quick-start) · [Build from source](#build-from-source) · [Packaging](#packaging)

## Features

- **One viewer for 3D and 4D.** Orbelio supports multiple static model formats, PLY sequences, and continuous-time FDU 4DGS. A 4D scene adds timing information to the same spatial representation and shares the 3D rendering pipeline.
- **Replaceable rendering backend.** The interface and renderer are separate, so another backend can work with the existing viewport.
- **Built for real-time interaction.** Orbelio reuses uploaded scene data when you move the camera or seek through a 4D scene. Rendered images go directly to the interface, reducing per-frame data transfers.

## Quick start

Extract the portable Windows x64 package and run `orbelio_viewer.exe` from the extracted folder. You need a GPU and driver that support Vulkan 1.3. Open a file with **Open**, **Ctrl+O**, or drag and drop.

| To view | Open |
| --- | --- |
| Static 3D model | Standard or compressed PLY, SOG, SPZ, SPLAT, KSPLAT |
| PLY frame sequence | A `.gsseq` manifest |
| FDU 4DGS scene | A `.fdu4d` file |

Drag with the left mouse button to orbit and scroll to zoom. With the viewport focused, press **F** to fit the scene or **R** to reset the camera. Use the timeline to play or seek through sequences and 4D scenes.

## Build from source

Install Git and MSYS2 on Windows, then add the MinGW x64 `bin` directory to your PowerShell PATH. Run the following from the repository root:

```powershell
# Install dependencies on first setup
.\scripts\install-environment.ps1

.\scripts\build.ps1 -Configuration Release
.\scripts\run.ps1 -Configuration Release
```

For debugging, replace `Release` with `Debug` in both commands. Add `-Test` to the build command to run CTest.

## Packaging

```powershell
.\scripts\package-windows.ps1
```

The script creates a portable Windows x64 folder with its runtime dependencies under `out/packages/`. Zip the entire folder for distribution.
