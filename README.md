# HyperVR

Immediate-mode **OpenXR + Vulkan** VR renderer — Hyperlite’s loop philosophy rebuilt for stereo 3D.

**Windows only (v1)** · **C++ engine** · **Python bindings** · **Not on PyPI** (local install)

## Requirements

| Component | Notes |
|-----------|--------|
| Windows 10/11 | OpenXR + Vulkan |
| [Vulkan SDK](https://vulkan.lunarg.com/) | Headers, loader, `glslangValidator` |
| OpenXR runtime | e.g. SteamVR, or another OpenXR-compatible runtime |
| VR headset | Active and discoverable by the runtime |
| Visual Studio 2022 | C++ desktop workload |
| CMake 3.24+ | On `PATH` |
| Python 3.10+ | Optional; for bindings |

## Quick start (native demo)

```powershell
cd C:\path\to\HyperVR
.\scripts\install.ps1
.\build\bin\Release\xr_demo.exe
```

Put the headset on — you should see a spinning cube. Right-controller grip shows a small marker cube (turns greener with trigger on simple controller bindings).

## Python

```powershell
.\scripts\install.ps1 -Python
python python\main.py
```

```python
import hypervr

engine = hypervr.Engine()
cube = engine.create_cube()
while engine.is_running():
    engine.poll_events()
    engine.begin_frame()
    engine.draw_mesh(cube, model_matrix_16_floats, 1, 0.7, 0.2, 1)
    engine.end_frame()
```

## Layout

| Path | Purpose |
|------|---------|
| `engine/` | C++ core (Vulkan stereo + OpenXR) |
| `apps/xr_demo/` | Native validation demo |
| `bindings/python/` | `hypervr` module |
| `docs/guide.md` | Full guide |

## License

MIT
