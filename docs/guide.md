# HyperVR Guide

Immediate-mode OpenXR/Vulkan VR engine. You own game logic; HyperVR owns stereo submit, Vulkan recording, mesh residency, and XR input.

## Mental model

| Layer | You write | HyperVR does |
|-------|-----------|--------------|
| Game logic | Python or C++ | — |
| Draw list | `draw_mesh` / `submit_draws` | Queues GPU work |
| Stereo | — | Per-eye view/proj from OpenXR |
| Present | — | XR swapchains + `xrEndFrame` |

Loop vocabulary (Hyperlite-shaped):

```
poll_events → begin_frame → draw… → end_frame
```

`end_frame` **is** present. There is no separate desktop present in v1.

## Requirements

1. **Vulkan SDK** with `VULKAN_SDK` set (installer usually does this).
2. **OpenXR runtime** with a headset connected (SteamVR OpenXR, etc.).
3. **VS 2022** + **CMake 3.24+**.
4. Python 3.10+ only if using bindings.

## Build

```powershell
.\scripts\install.ps1              # configure + build xr_demo
.\scripts\install.ps1 -Python      # also build + copy hypervr.pyd into user site-packages
```

`-Python` prefers a full CPython install under `%LOCALAPPDATA%\Programs\Python` (needs development headers for the extension build). Pass `-PythonExe path\to\python.exe` to target a specific interpreter.

Manual:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target xr_demo
```

Shaders compile to `engine/shaders/*.spv` at build time via `glslangValidator`.

## Native demo

```powershell
.\build\bin\Release\xr_demo.exe
```

Prints backend/runtime name and periodic frame timings (`wait`, `record`, `submit`, `total` ms).

## Python API

### Engine

```python
engine = hypervr.Engine("My App")
engine.is_running()
engine.poll_events()
engine.begin_frame()
engine.end_frame()
engine.delta_time()
engine.timings()       # (wait_ms, record_ms, submit_ms, total_ms)
engine.backend_name()
```

### Meshes & draws

```python
cube = engine.create_cube()
left = engine.create_hand(hypervr.HAND_LEFT)   # (palm, thumb, index, pinky, side)
right = engine.create_hand(hypervr.HAND_RIGHT)
engine.draw_hand(left)
engine.draw_hand(right)
```

Hands are **dark grey**, three digits (thumb / index / pinky). `draw_hand` parents them to the grip pose and curls:

| Input | Finger |
|-------|--------|
| Trigger (top) | Index |
| Grip / squeeze (bottom) | Pinky |
| A / X | Thumb |

```python
mesh = engine.load_mesh(vertices, indices)  # vertex = (px,py,pz,r,g,b)
engine.draw_mesh(mesh, model16, r, g, b, a)
```

`model16` is a **column-major** 4×4 (16 floats). Prefer `submit_draws` when issuing many draws from Python.

### Input

```python
pose = engine.controller_pose(hypervr.HAND_RIGHT)  # list of 16 or None
down = engine.button_down(hypervr.HAND_RIGHT, hypervr.BUTTON_TRIGGER)
```

Constants: `HAND_LEFT`, `HAND_RIGHT`, `BUTTON_TRIGGER`, `BUTTON_GRIP`, `BUTTON_MENU`, `BUTTON_A_X`, `BUTTON_B_Y`.

Simple controller profile maps select→trigger and menu→menu. Grip/A/X/B/Y may stay false until a richer profile is suggested.

## What the engine owns

- OpenXR instance/session/spaces/swapchains/frame sync
- Vulkan device (via `XR_KHR_vulkan_enable2`) and stereo command recording
- Mesh upload and draw-list execution
- Action sync into compact pose/button state
- Frame timings

## What you own

- Simulation / gameplay
- Which meshes to draw and their transforms
- Content loading beyond the built-in cube

## Coordinate system

OpenXR world/view space is **+X right, +Y up, −Z forward**. Poses returned by `controller_pose` use that convention 1:1.

Vulkan clip space is **+Y down**, so the engine builds eye projections with the Khronos `GRAPHICS_VULKAN` form from `xr_linear.h` (`tanAngleDown - tanAngleUp`). Do not flip controller or model Y yourself for “Vulkan” — the projection already accounts for it.

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `xrGetSystem` fails | Start the OpenXR runtime; wake the headset |
| Missing `XR_KHR_vulkan_enable2` | Update the runtime / GPU drivers |
| Shader load failed | Build so `mesh.*.spv` exist under `engine/shaders` |
| Python import fails | Re-run `install.ps1 -Python` with the same interpreter |
| Validation layer crash in Debug | Ensure Vulkan SDK layers are installed; try Release |

## Out of scope (v1)

Scene graph, full rigid-body physics engine, UI toolkit, desktop mirror window, macOS/Linux.

## Acoustics (Python + Spherion)

The `python/` tree vendors NULLSIGHT's geometric acoustics pipeline and drives **Spherion** for HOA/HRTF output. See [`python/README_AUDIO.md`](../python/README_AUDIO.md). Entry point: `python/main.py`.
