# HyperVR Python acoustics (NULLSIGHT + Spherion)

This tree vendors NULLSIGHT's geometric acoustics middleware **byte-for-byte** and drives **Spherion** for HOA/HRTF output.

## 1:1 with NULLSIGHT

| Path | Parity |
|------|--------|
| `acoustics/*.py` | Identical to `nullsight/acoustics/` |
| `math3d.py`, `occlusion.py` | Identical to NULLSIGHT |
| `camera.py` | Thin alias so `system.py` keeps `from camera import Camera` |
| `room.py` | HyperVR box-room builder; `RoomData` fields used by acoustics match |
| `hypervr_audio.py` | Spherion bridge — same link mapping as `nullsight_audio` (dry/LPF/taps/reverb) |

Verify anytime:

```powershell
python python\parity_nullsight_acoustics.py
python python\smoke_acoustics.py
```

## Layout

| Path | Role |
|------|------|
| `acoustics/` | Path-native occlusion, ISM reflections, UTD diffraction, materials, Sabine/Eyring RT60, modes, portals, radiosity |
| `math3d.py` / `occlusion.py` | Shared math + AABB LOS used by the tracer |
| `room.py` | HyperVR box-room → acoustic `RoomData` + `ListenerPose` |
| `hypervr_audio.py` | OpenXR↔Spherion coords, AcousticSystem tick, Spherion link application |
| `assets/` | `cube_buzz.wav` (HyperVR test emitter) + `subject_003_48k.sofa` (HRTF) |

## Setup

```powershell
cd c:\Robotics\newer-non-robotics\spherion
python -m pip install -e .

cd c:\Robotics\newer-non-robotics\HyperVR
python python\main.py
```

## What runs each frame

1. HMD pose → Spherion listener (`+Z` forward; OpenXR `−Z` is flipped).
2. Room shell + dynamic AABBs (e.g. physics cube) → `AcousticSystem` snapshot.
3. ~10 Hz path trace → smoothed `AcousticLink` (direct / transmission / diffraction / reflection taps / reverb send).
4. Links push dry gain, LPF, extended early taps, Doppler velocity, and late Schroeder RT60 into Spherion.
