"""Smoke check: cube buzz asset, acoustics, HRTF, default playback device.

Run from HyperVR root:
  python python/smoke_acoustics.py
"""

from __future__ import annotations

import math
import struct
import sys
import time
import wave
from pathlib import Path

_ROOT = Path(__file__).resolve().parent
if str(_ROOT) not in sys.path:
	sys.path.insert(0, str(_ROOT))

ASSETS = _ROOT / "assets"
CUBE_WAV = ASSETS / "cube_buzz.wav"
HRTF_SOFA = ASSETS / "subject_003_48k.sofa"

_failures: list[str] = []
_passes: list[str] = []


def _ok(msg: str) -> None:
	"""Record a passing check."""
	_passes.append(msg)
	print(f"  PASS  {msg}")


def _fail(msg: str) -> None:
	"""Record a failing check."""
	_failures.append(msg)
	print(f"  FAIL  {msg}")


def _rms_stereo(block) -> float:
	"""RMS of (N,2) float block."""
	import numpy as np

	arr = np.asarray(block, dtype=np.float64)
	return float(np.sqrt(np.mean(arr * arr)))


def _lr_energy(block) -> tuple[float, float]:
	"""Mean absolute energy for left and right channels."""
	import numpy as np

	arr = np.asarray(block, dtype=np.float64)
	left = arr[:, 0]
	right = arr[:, 1]
	return float(np.mean(np.abs(left))), float(np.mean(np.abs(right)))


def ensure_cube_asset() -> Path:
	"""Ensure HyperVR cube_buzz.wav exists (generate via main helper)."""
	from main import ensure_cube_noise_wav

	return ensure_cube_noise_wav(CUBE_WAV)


def check_assets_on_disk() -> None:
	"""Verify cube buzz + HRTF exist."""
	print("\n== Assets ==")
	path = ensure_cube_asset()
	if not path.is_file() or path.stat().st_size < 1000:
		_fail(f"cube_buzz missing/small: {path}")
	else:
		_ok(f"cube_buzz present ({path.stat().st_size:,} bytes)")

	if not HRTF_SOFA.is_file() or HRTF_SOFA.stat().st_size < 50_000:
		_fail(f"HRTF SOFA missing/small: {HRTF_SOFA}")
	else:
		_ok(f"HRTF SOFA present ({HRTF_SOFA.stat().st_size:,} bytes)")


def check_wav_decode() -> None:
	"""Decode cube_buzz as stereo PCM."""
	print("\n== WAV decode ==")
	path = ensure_cube_asset()
	try:
		with wave.open(str(path), "rb") as wav:
			chans = wav.getnchannels()
			rate = wav.getframerate()
			frames = wav.getnframes()
			width = wav.getsampwidth()
			raw = wav.readframes(min(frames, rate))
		samples = struct.unpack(f"<{len(raw)//2}h", raw)
		peak = max(abs(s) for s in samples) if samples else 0
		if chans != 2 or width != 2 or peak < 1000:
			_fail(f"cube_buzz bad decode chans={chans} width={width} peak={peak}")
		else:
			_ok(f"cube_buzz: {chans}ch {rate}Hz {frames} frames peak={peak}")
	except Exception as exc:  # noqa: BLE001
		_fail(f"cube_buzz decode error: {exc}")


def check_acoustics_pipeline() -> None:
	"""Exercise AcousticSystem clear / occluded / directivity."""
	print("\n== Acoustics pipeline ==")
	from acoustics import AcousticSystem, AcousticSource
	from room import ListenerPose, box_room

	room = box_room(12.0, 12.0, 7.0, shell_material="concrete")
	system = AcousticSystem()
	src_pos = (4.5, 1.2, 4.5)
	sources = [AcousticSource(source_id=1, position=src_pos, directivity="omni")]
	listener = ListenerPose(eye=(0.0, 1.6, 0.0), yaw=0.0, pitch=0.0)
	link = system.update(system.snapshot(room, None, sources, listener), 0.15)[1]
	if link.direct_gain < 0.9:
		_fail(f"clear path direct_gain={link.direct_gain}")
	else:
		_ok(f"clear LOS direct_gain={link.direct_gain:.2f}")
	if len(link.reflection_taps) < 2:
		_fail(f"expected reflection taps, got {len(link.reflection_taps)}")
	else:
		_ok(f"reflection taps={len(link.reflection_taps)}")
	if link.reverb_send <= 0.05 or link.rt60_s <= 0.2:
		_fail(f"late reverb weak send={link.reverb_send} rt60={link.rt60_s}")
	else:
		_ok(f"late reverb send={link.reverb_send:.2f} rt60={link.rt60_s:.2f}s")

	slab = ((1.5, 0.0, 1.5), (2.5, 3.0, 3.5))
	room_occ = box_room(12.0, 12.0, 7.0, extra_occluders=[slab], extra_materials=["concrete"])
	system.clear()
	blocked = system.update(system.snapshot(room_occ, None, sources, listener), 0.15)[1]
	if blocked.direct_gain > 0.5:
		_fail(f"occluder did not block direct (gain={blocked.direct_gain})")
	else:
		_ok(f"occluder blocked direct (mode={blocked.path_mode})")

	from hypervr_audio import head_pose_to_listener, openxr_to_spherion
	import spherion

	pose = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.6, 0, 1]
	lp = head_pose_to_listener(pose)
	if abs(lp.yaw) > 1e-4 or abs(lp.pitch) > 1e-4 or abs(lp.roll) > 1e-4:
		_fail(f"identity OpenXR pose angles expected 0 got yaw={lp.yaw} pitch={lp.pitch} roll={lp.roll}")
	else:
		_ok("OpenXR identity → Spherion yaw=pitch=roll=0")

	# Guide coord table: ahead / hard-right after OpenXR → Spherion Z flip.
	cube = openxr_to_spherion(0.0, 1.2, -1.2)  # OpenXR ahead (−Z)
	rel = (cube[0] - lp.eye[0], cube[1] - lp.eye[1], cube[2] - lp.eye[2])
	ang = spherion.compute_listener_relative_angles(rel[0], rel[1], rel[2], lp.yaw, lp.pitch, 0.0)
	if ang.local_z <= 0.0 or abs(ang.azimuth_rad) > 0.05:
		_fail(f"cube ahead expected; az={ang.azimuth_rad} local_z={ang.local_z}")
	else:
		_ok("OpenXR ahead (−Z) → Spherion ahead (+Z)")

	right = openxr_to_spherion(2.0, 1.6, 0.0)
	ang_r = spherion.compute_listener_relative_angles(
		right[0] - lp.eye[0], right[1] - lp.eye[1], right[2] - lp.eye[2], 0.0, 0.0, 0.0
	)
	if abs(ang_r.azimuth_rad - math.pi / 2.0) > 0.05:
		_fail(f"OpenXR +X should be hard right, az={ang_r.azimuth_rad}")
	else:
		_ok("OpenXR +X → Spherion hard right (az≈π/2)")


def check_cube_offline_render() -> None:
	"""Offline Spherion render of cube_buzz must be audible."""
	print("\n== Cube offline render ==")
	import spherion

	path = ensure_cube_asset()
	hrtf = str(HRTF_SOFA) if HRTF_SOFA.is_file() else ""
	engine = spherion.Engine(sample_rate=48000, order=3, block_size=256, hrtf_path=hrtf)
	engine.set_listener_pose(0.0, 1.6, 0.0, 0.0, 0.0, 0.0)
	obj = engine.add_stereo_object(str(path), loop=True)
	obj.set_position(0.0, 1.0, 1.5)
	obj.set_gain(1.0)
	peak = 0.0
	rms = 0.0
	for _ in range(40):
		block = engine.process_block(256)
		peak = max(peak, float(abs(block).max()))
		rms = max(rms, _rms_stereo(block))
	if peak < 1e-4 or rms < 1e-5:
		_fail(f"cube_buzz silent offline peak={peak} rms={rms}")
	else:
		_ok(f"cube_buzz offline peak={peak:.4f} rms={rms:.5f}")


def check_hrtf() -> None:
	"""SOFA HRTF changes output and gives lateral ILD."""
	print("\n== HRTF ==")
	import numpy as np
	import spherion

	if not HRTF_SOFA.is_file():
		_fail("no SOFA file")
		return
	wav = ensure_cube_asset()
	sofa = str(HRTF_SOFA)

	base = spherion.Engine(sample_rate=48000, order=3, block_size=256, hrtf_path="")
	sofa_eng = spherion.Engine(sample_rate=48000, order=3, block_size=256, hrtf_path=sofa)
	for eng in (base, sofa_eng):
		eng.set_listener_pose(0.0, 1.6, 0.0, 0.0, 0.0, 0.0)
		o = eng.add_stereo_object(str(wav), loop=True)
		o.set_position(0.0, 1.2, 5.0)
		o.set_gain(1.0)
	for _ in range(8):
		base.process_block(256)
		sofa_eng.process_block(256)
	diff = float(np.max(np.abs(base.process_block(1024) - sofa_eng.process_block(1024))))
	if diff < 1e-4:
		_fail(f"SOFA HRTF identical to empty (maxΔ={diff})")
	else:
		_ok(f"SOFA HRTF changes output (maxΔ={diff:.5f})")

	left_eng = spherion.Engine(sample_rate=48000, order=3, block_size=256, hrtf_path=sofa)
	right_eng = spherion.Engine(sample_rate=48000, order=3, block_size=256, hrtf_path=sofa)
	for eng, x in ((left_eng, -4.0), (right_eng, 4.0)):
		eng.set_listener_pose(0.0, 1.6, 0.0, 0.0, 0.0, 0.0)
		o = eng.add_stereo_object(str(wav), loop=True)
		o.set_position(x, 1.2, 2.0)
		o.set_gain(1.0)
	for _ in range(16):
		left_eng.process_block(256)
		right_eng.process_block(256)
	l_L, l_R = _lr_energy(left_eng.process_block(2048))
	r_L, r_R = _lr_energy(right_eng.process_block(2048))
	left_bias = l_L - l_R
	right_bias = r_R - r_L
	print(f"         ILD left-src bias={left_bias:.5f}  right-src bias={right_bias:.5f}")
	if left_bias <= 0.0:
		_fail(f"left-side not left-ear dominant ({left_bias})")
	else:
		_ok(f"left-side left-ear dominant ({left_bias:.5f})")
	if right_bias <= 0.0:
		_fail(f"right-side not right-ear dominant ({right_bias})")
	else:
		_ok(f"right-side right-ear dominant ({right_bias:.5f})")


def check_default_device_playback() -> None:
	"""Open the OS default device and play cube buzz for ~1.25s (must be running)."""
	print("\n== Default device playback ==")
	from hypervr_audio import HyperVRAudio

	wav = ensure_cube_asset()
	audio = HyperVRAudio(12.0, 12.0, 7.0, master_gain=1.0, spatial_gain=3.5, enabled=True)
	if not audio.active:
		_fail(f"HyperVRAudio inactive: {audio.reason}")
		return

	device = audio.playback_device_name
	print(f"         device: {device or '(unnamed default)'}")
	if not audio._world.is_running:  # noqa: SLF001 — intentional smoke probe
		_fail("engine reports not running after start")
		audio.shutdown()
		return
	_ok(f"device started ({device or 'default'})")

	eid = audio.spawn_loop("cube_buzz", 0.0, 1.2, -1.2, gain=1.0, wav_path=wav)
	if eid is None:
		_fail("spawn_loop returned None")
		audio.shutdown()
		return
	_ok(f"spawned cube_buzz id={eid}")

	pose = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.6, 0, 1]
	# Play long enough to hear; also exercise None-head fallback.
	audio.tick(None, 0.05)
	for _ in range(40):
		audio.set_source_position_openxr(eid, 0.0, 1.2, -1.2, 1 / 30)
		audio.tick(pose, 1 / 30)
		time.sleep(1 / 30)

	lines = audio.acoustic_debug_lines()
	print("         debug:", lines)
	if not lines:
		_fail("no acoustic debug lines after tick")
	else:
		_ok(f"acoustic tick ok ({lines[-1] if lines else ''})")

	# Restart should still land on a running default device.
	if not audio.restart_default_device():
		_fail(f"restart_default_device failed: {audio.reason}")
	else:
		_ok(f"restarted device ({audio.playback_device_name or 'default'})")

	audio.shutdown()
	_ok("shutdown clean")


def check_engine_config() -> None:
	"""EngineConfig physics flags."""
	print("\n== EngineConfig flags ==")
	from hypervr_audio import _build_engine_config, _resolve_hrtf

	cfg = _build_engine_config(order=3, block_size=256, hrtf_path=_resolve_hrtf())
	for attr, expected in {
		"enable_hf_air_absorption": True,
		"enable_doppler": True,
		"enable_propagation_delay": True,
		"enable_head_shadow": True,
		"sample_rate_hz": 48_000,
	}.items():
		got = getattr(cfg, attr)
		if got != expected:
			_fail(f"config.{attr}={got!r} expected {expected!r}")
		else:
			_ok(f"config.{attr}={got}")


def main() -> int:
	"""Run the smoke suite."""
	print("HyperVR acoustics / Spherion smoke check")
	print(f"python root: {_ROOT}")
	check_assets_on_disk()
	check_wav_decode()
	check_acoustics_pipeline()
	check_engine_config()
	check_cube_offline_render()
	check_hrtf()
	check_default_device_playback()

	print("\n== Summary ==")
	print(f"  passed: {len(_passes)}")
	print(f"  failed: {len(_failures)}")
	for msg in _failures:
		print(f"   - {msg}")
	return 1 if _failures else 0


if __name__ == "__main__":
	sys.exit(main())
