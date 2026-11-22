# <img src="assets/brand/orbelio-icon.png" width="40" height="40" alt="" /> Orbelio

[English](README.md) · **简体中文**

Orbelio 是一个面向 Windows 的 3D 和 4D Gaussian Splatting 查看器，结合 Qt Quick 界面与独立的 Vulkan 渲染器。它将场景数据与渲染实现解耦，让静态模型、逐帧序列和连续时间场景共用同一个视口。

[项目特点](#项目特点) · [快速上手](#快速上手) · [从源码构建](#从源码构建) · [打包](#打包)

## 项目特点

- **统一的 3D 和 4D 查看体验。** 支持多种静态模型格式、PLY 序列和连续时间 FDU 4DGS。4D 场景在同一套空间数据上加入时间信息，复用 3D 的渲染流程。
- **可替换的渲染后端。** 界面与渲染器独立，后端可以替换，现有视口可以继续使用。
- **面向实时交互。** 旋转相机或跳转 4D 时间时，尽量复用已上传的场景数据。渲染结果直接交给界面显示，减少每帧的数据搬运。

## 快速上手

解压 Windows x64 便携包，运行目录中的 `orbelio_viewer.exe`。需要支持 Vulkan 1.3 的 GPU 和驱动。用 **Open**、**Ctrl+O** 或拖放打开文件。

| 想查看 | 打开 |
| --- | --- |
| 静态 3D 模型 | 普通或压缩 PLY、SOG、SPZ、SPLAT、KSPLAT |
| PLY 帧序列 | `.gsseq` 清单 |
| FDU 4DGS 场景 | `.fdu4d` 文件 |

左键拖动环绕，滚轮缩放。视口获得焦点后，按 **F** 适配场景，按 **R** 重置相机。打开序列或 4D 场景后，可用时间轴播放和跳转。

## 从源码构建

在 Windows 上安装 Git 和 MSYS2，将 MinGW x64 的 `bin` 加入当前 PowerShell 的 PATH。然后在仓库根目录运行：

```powershell
# 首次配置依赖
.\scripts\install-environment.ps1

.\scripts\build.ps1 -Configuration Release
.\scripts\run.ps1 -Configuration Release
```

调试时可将构建与启动命令中的 `Release` 改为 `Debug`。构建时加 `-Test` 可运行 CTest。

## 打包

```powershell
.\scripts\package-windows.ps1
```

脚本会在 `out/packages/` 生成带运行依赖的 Windows x64 便携目录。分发时将整个目录压缩成 ZIP。
