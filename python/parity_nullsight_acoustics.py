"""Parity check: HyperVR acoustics vs live NULLSIGHT acoustics (same inputs → same links).

Run:
  python python/parity_nullsight_acoustics.py
"""

from __future__ import annotations

import importlib.util
import math
import sys
import types
from dataclasses import fields
from pathlib import Path

_HV = Path(__file__).resolve().parent
_NS = Path(r"c:\Robotics\newer-non-robotics\nullsight")


def _load_package(name: str, root: Path) -> types.ModuleType:
	"""Load a top-level package directory under an isolated module name."""
	# Ensure package root is importable for its internal absolute imports.
	root_s = str(root)
	if root_s not in sys.path:
		sys.path.insert(0, root_s)
	return importlib.import_module(name)


def _link_dict(link) -> dict:
	"""Flatten an AcousticLink (and nested taps) to comparable primitives."""
	out = {}
	for f in fields(link):
		val = getattr(link, f.name)
		if f.name in ("reflection_taps", "diffraction_extra"):
			out[f.name] = [
				{
					"gain": round(t.gain, 6),
					"delay_s": round(t.delay_s, 6),
					"lpf_hz": round(t.lpf_hz, 3),
					"bounce_order": t.bounce_order,
					"arrival_dir": tuple(round(c, 6) for c in t.arrival_dir),
					"band_energy": tuple(round(c, 6) for c in t.band_energy),
					"is_diffuse": t.is_diffuse,
				}
				for t in val
			]
		elif isinstance(val, float):
			out[f.name] = round(val, 6)
		else:
			out[f.name] = val
	return out


def _build_room_hv():
	"""HyperVR box room in shared meter space."""
	if str(_HV) not in sys.path:
		sys.path.insert(0, str(_HV))
	from room import box_room

	return box_room(12.0, 12.0, 7.0, name="hypervr", shell_material="concrete")


def _build_room_ns(hv_room):
	"""Clone HyperVR room fields into a NULLSIGHT RoomData instance."""
	# NULLSIGHT room module must win on sys.path when constructing.
	sys.path.insert(0, str(_NS))
	from room import RoomData as NsRoomData  # type: ignore

	return NsRoomData(
		environment_edges=list(hv_room.environment_edges),
		occluder_hulls=list(hv_room.occluder_hulls),
		bounds=hv_room.bounds,
		occluders=list(hv_room.occluders),
		collidables=list(hv_room.collidables),
		boundaries=list(hv_room.boundaries),
		spawn=hv_room.spawn,
		name=hv_room.name,
		occluder_materials=list(hv_room.occluder_materials),
		shell_material=hv_room.shell_material,
		floor_y=hv_room.floor_y,
		ceiling_y=hv_room.ceiling_y,
	)


def main() -> int:
	"""Compare HyperVR vs NULLSIGHT AcousticSystem outputs."""
	print("NULLSIGHT acoustics parity check")
	print(f"  HyperVR python: {_HV}")
	print(f"  NULLSIGHT root: {_NS}")

	# --- Byte identity of physics modules ---
	print("\n== Byte identity (acoustics/*.py, math3d, occlusion) ==")
	mismatched = []
	for p in sorted((_NS / "acoustics").glob("*.py")):
		a = p.read_bytes()
		b = (_HV / "acoustics" / p.name).read_bytes()
		if a != b:
			mismatched.append(p.name)
			print(f"  FAIL  {p.name}")
		else:
			print(f"  PASS  {p.name}")
	for name in ("math3d.py", "occlusion.py"):
		if (_NS / name).read_bytes() != (_HV / name).read_bytes():
			mismatched.append(name)
			print(f"  FAIL  {name}")
		else:
			print(f"  PASS  {name}")

	# --- Numerical link parity ---
	print("\n== Numerical AcousticLink parity ==")
	# Load HyperVR acoustics first (its path already preferred).
	sys.path = [str(_HV)] + [p for p in sys.path if p not in (str(_HV), str(_NS))]
	from acoustics.system import AcousticSystem as HvSystem
	from acoustics.types import AcousticSource as HvSource
	from camera import Camera as HvCamera

	hv_room = _build_room_hv()
	hv_sys = HvSystem()
	listener = HvCamera(eye=(0.0, 1.6, 0.0), yaw=0.0, pitch=0.0)
	hv_sources = [
		HvSource(source_id=1, position=(4.5, 1.2, 4.5), directivity="omni"),
		HvSource(
			source_id=2,
			position=(-3.0, 1.0, 2.0),
			directivity="cardioid",
			directivity_axis=(1.0, 0.0, 0.0),
			speaker_radius_m=0.08,
		),
	]
	# Force immediate trace.
	hv_links = hv_sys.update(hv_sys.snapshot(hv_room, None, hv_sources, listener), 0.15)

	# Now load NULLSIGHT acoustics under its own tree.
	# Wipe acoustics modules so re-import resolves to NULLSIGHT.
	for key in list(sys.modules):
		if key == "acoustics" or key.startswith("acoustics.") or key in (
			"math3d",
			"occlusion",
			"room",
			"camera",
		):
			del sys.modules[key]
	sys.path = [str(_NS)] + [p for p in sys.path if p not in (str(_HV), str(_NS))]

	from acoustics.system import AcousticSystem as NsSystem  # type: ignore
	from acoustics.types import AcousticSource as NsSource  # type: ignore
	from camera import Camera as NsCamera  # type: ignore

	ns_room = _build_room_ns(hv_room)
	ns_sys = NsSystem()
	ns_cam = NsCamera.__new__(NsCamera) if False else None
	# NULLSIGHT Camera is a real class — build a duck with required API.
	class _Cam:
		def __init__(self):
			self.yaw = 0.0
			self.pitch = 0.0

		def eye_position(self):
			return (0.0, 1.6, 0.0)

	ns_cam = _Cam()
	ns_sources = [
		NsSource(source_id=1, position=(4.5, 1.2, 4.5), directivity="omni"),
		NsSource(
			source_id=2,
			position=(-3.0, 1.0, 2.0),
			directivity="cardioid",
			directivity_axis=(1.0, 0.0, 0.0),
			speaker_radius_m=0.08,
		),
	]
	ns_links = ns_sys.update(ns_sys.snapshot(ns_room, None, ns_sources, ns_cam), 0.15)

	link_fails = 0
	for sid in (1, 2):
		a = _link_dict(hv_links[sid])
		b = _link_dict(ns_links[sid])
		if a != b:
			link_fails += 1
			print(f"  FAIL  source {sid} links differ")
			for k in sorted(set(a) | set(b)):
				if a.get(k) != b.get(k):
					print(f"         {k}: HV={a.get(k)}  NS={b.get(k)}")
		else:
			print(f"  PASS  source {sid} AcousticLink identical ({a['path_mode']})")

	# Occluded case
	slab = ((1.5, 0.0, 1.5), (2.5, 3.0, 3.5))
	# Rebuild HV room with occluder — need HV imports again.
	for key in list(sys.modules):
		if key == "acoustics" or key.startswith("acoustics.") or key in (
			"math3d",
			"occlusion",
			"room",
			"camera",
		):
			del sys.modules[key]
	sys.path = [str(_HV)] + [p for p in sys.path if p not in (str(_HV), str(_NS))]
	from acoustics.system import AcousticSystem as HvSystem2
	from acoustics.types import AcousticSource as HvSource2
	from camera import Camera as HvCamera2
	from room import box_room

	hv_occ = box_room(12, 12, 7, extra_occluders=[slab], extra_materials=["concrete"])
	hv_sys2 = HvSystem2()
	hv_l2 = hv_sys2.update(
		hv_sys2.snapshot(
			hv_occ,
			None,
			[HvSource2(source_id=1, position=(4.5, 1.2, 4.5))],
			HvCamera2(eye=(0.0, 1.6, 0.0)),
		),
		0.15,
	)[1]

	for key in list(sys.modules):
		if key == "acoustics" or key.startswith("acoustics.") or key in (
			"math3d",
			"occlusion",
			"room",
			"camera",
		):
			del sys.modules[key]
	sys.path = [str(_NS)] + [p for p in sys.path if p not in (str(_HV), str(_NS))]
	from acoustics.system import AcousticSystem as NsSystem2  # type: ignore
	from acoustics.types import AcousticSource as NsSource2  # type: ignore
	from room import RoomData as NsRoomData2  # type: ignore

	ns_occ = NsRoomData2(
		environment_edges=[],
		occluder_hulls=[],
		bounds=hv_occ.bounds,
		occluders=list(hv_occ.occluders),
		collidables=list(hv_occ.collidables),
		boundaries=list(hv_occ.boundaries),
		spawn=hv_occ.spawn,
		name=hv_occ.name,
		occluder_materials=list(hv_occ.occluder_materials),
		shell_material=hv_occ.shell_material,
		floor_y=hv_occ.floor_y,
		ceiling_y=hv_occ.ceiling_y,
	)
	ns_sys2 = NsSystem2()

	class _Cam2:
		yaw = 0.0
		pitch = 0.0

		def eye_position(self):
			return (0.0, 1.6, 0.0)

	ns_l2 = ns_sys2.update(
		ns_sys2.snapshot(
			ns_occ,
			None,
			[NsSource2(source_id=1, position=(4.5, 1.2, 4.5))],
			_Cam2(),
		),
		0.15,
	)[1]

	a = _link_dict(hv_l2)
	b = _link_dict(ns_l2)
	if a != b:
		link_fails += 1
		print("  FAIL  occluded source links differ")
		for k in sorted(set(a) | set(b)):
			if a.get(k) != b.get(k):
				print(f"         {k}: HV={a.get(k)}  NS={b.get(k)}")
	else:
		print(f"  PASS  occluded AcousticLink identical ({a['path_mode']})")

	# Bridge helper parity: effective obstruction + early taps
	print("\n== Spherion bridge mapping helpers ==")
	for key in list(sys.modules):
		if key == "acoustics" or key.startswith("acoustics.") or key in (
			"math3d",
			"occlusion",
			"room",
			"camera",
			"hypervr_audio",
		):
			del sys.modules[key]
	sys.path = [str(_HV)] + [p for p in sys.path if p not in (str(_HV), str(_NS))]
	from hypervr_audio import HyperVRAudio
	from acoustics.types import AcousticLink, ReflectionTap

	# Instantiate without device for helper methods.
	bridge = HyperVRAudio(12, 12, 7, enabled=False)
	# Use a synthetic link matching what tracer would emit.
	fake = AcousticLink(
		source_id=9,
		direct_gain=0.0,
		direct_lpf_hz=400.0,
		transmission_gain=0.2,
		transmission_lpf_hz=800.0,
		reflection_gain=0.5,
		reflection_delay_s=0.02,
		reflection_lpf_hz=10000.0,
		reflection_taps=(
			ReflectionTap(gain=0.5, delay_s=0.02, lpf_hz=10000.0, bounce_order=1),
			ReflectionTap(gain=0.3, delay_s=0.04, lpf_hz=8000.0, bounce_order=2),
		),
		diffraction_gain=0.15,
		diffraction_delay_s=0.01,
		diffraction_lpf_hz=1200.0,
		path_mode="reflected",
		reverb_send=0.4,
		rt60_s=1.5,
		reverb_predelay_s=0.02,
		structure_borne_gain=0.1,
		structure_borne_lpf_hz=450.0,
		structure_borne_delay_s=0.005,
	)
	g, lpf = bridge._effective_obstruction(fake)
	taps = bridge._combined_early_taps(fake)
	# Recompute with inlined NULLSIGHT formulas (copied).
	ns_gain = max(fake.direct_gain, fake.transmission_gain)
	candidates = (
		(fake.direct_gain, fake.direct_lpf_hz),
		(fake.transmission_gain, fake.transmission_lpf_hz),
	)
	ns_lpf = max(candidates, key=lambda item: item[0])[1]
	if fake.direct_gain > 0.05 and fake.transmission_gain > 0.05:
		ns_lpf = min(fake.direct_lpf_hz, fake.transmission_lpf_hz)
	if ns_gain < 1e-4:
		ns_lpf = 20_000.0
	if (g, lpf) != (ns_gain, ns_lpf):
		print(f"  FAIL  _effective_obstruction HV=({g},{lpf}) NS=({ns_gain},{ns_lpf})")
		link_fails += 1
	else:
		print(f"  PASS  _effective_obstruction → ({g}, {lpf})")
	if len(taps) < 3:
		print(f"  FAIL  expected specular+diffraction+structure taps, got {len(taps)}")
		link_fails += 1
	else:
		print(f"  PASS  _combined_early_taps → {len(taps)} taps (incl. diffraction/structure)")

	print("\n== Summary ==")
	byte_ok = not mismatched
	num_ok = link_fails == 0
	print(f"  byte-identical physics: {'YES' if byte_ok else 'NO — ' + str(mismatched)}")
	print(f"  numerical link parity:  {'YES' if num_ok else f'NO — {link_fails} fails'}")
	return 0 if byte_ok and num_ok else 1


if __name__ == "__main__":
	sys.exit(main())
