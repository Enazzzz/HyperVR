"""Spherion bridge for HyperVR — NULLSIGHT acoustics pipeline + HOA/HRTF output.

OpenXR world space is Y-up with −Z forward. Spherion / NULLSIGHT use +Z forward.
All acoustics math and Spherion placement happen in Spherion space; convert at the
HyperVR boundary via ``openxr_to_spherion`` / ``head_pose_to_listener``.
"""

from __future__ import annotations

import math
import sys
from dataclasses import dataclass
from pathlib import Path

from acoustics.directivity import world_dir_to_listener_angles
from acoustics.materials import band_energy_to_three_band_gains
from acoustics.modes import modal_resonator_params, modes_from_boundaries
from acoustics.reverb_zones import reverb_zone_from_geometry, wet_level_from_rt60
from acoustics.system import AcousticSystem
from acoustics.types import AcousticLink, AcousticSource, ReflectionTap
from room import ListenerPose, RoomData, box_room

# Package root (python/) so assets resolve whether run as script or module.
_PYTHON_ROOT = Path(__file__).resolve().parent
_ASSETS_DIR = _PYTHON_ROOT / "assets"
DEFAULT_HRTF_FILENAME = "subject_003_48k.sofa"


def openxr_to_spherion(x: float, y: float, z: float) -> tuple[float, float, float]:
	"""Map OpenXR meters (Y-up, −Z forward) → Spherion (+Z forward).

	Spherion docs (Pattern E): Unity +Z needs no conversion; OpenXR/−Z engines
	must negate Z so yaw=0 still looks along +Z and +X stays right.
	"""
	return (x, y, -z)


def spherion_to_openxr(x: float, y: float, z: float) -> tuple[float, float, float]:
	"""Inverse of ``openxr_to_spherion``."""
	return (x, y, -z)


def _normalize3(x: float, y: float, z: float) -> tuple[float, float, float]:
	"""Return a unit vector; fallback to +Z when degenerate."""
	mag = math.sqrt(x * x + y * y + z * z)
	if mag < 1e-9:
		return (0.0, 0.0, 1.0)
	inv = 1.0 / mag
	return (x * inv, y * inv, z * inv)


def head_pose_to_listener(pose: list[float]) -> ListenerPose:
	"""Convert a column-major OpenXR mid-eye pose matrix to a Spherion listener.

	Feeds ``sync_listener(eye, yaw, pitch, roll)`` with **radians** (Spherion guide:
	never degrees). OpenXR forward is −Z of the orientation basis; after the Z flip,
	yaw=0 looks down +Z (NULLSIGHT convention).
	"""
	# Eye translation (world meters, OpenXR).
	ex, ey, ez = pose[12], pose[13], pose[14]
	# OpenXR basis columns: X=right, Y=up, Z=back; forward = −Z.
	fx, fy, fz = -pose[8], -pose[9], -pose[10]
	ux, uy, uz = pose[4], pose[5], pose[6]
	# Into Spherion (+Z forward, +X right).
	sx, sy, sz = openxr_to_spherion(ex, ey, ez)
	fwd_x, fwd_y, fwd_z = _normalize3(*openxr_to_spherion(fx, fy, fz))
	up_x, up_y, up_z = _normalize3(*openxr_to_spherion(ux, uy, uz))

	horiz = math.hypot(fwd_x, fwd_z)
	yaw = math.atan2(fwd_x, fwd_z)
	pitch = math.atan2(fwd_y, max(horiz, 1e-8))

	# Roll about view forward: compare actual up to the zero-roll up from yaw/pitch.
	# right_ref = normalize(world_up × forward); up_ref = forward × right_ref.
	world_up = (0.0, 1.0, 0.0)
	right_ref = _normalize3(
		world_up[1] * fwd_z - world_up[2] * fwd_y,
		world_up[2] * fwd_x - world_up[0] * fwd_z,
		world_up[0] * fwd_y - world_up[1] * fwd_x,
	)
	up_ref = _normalize3(
		fwd_y * right_ref[2] - fwd_z * right_ref[1],
		fwd_z * right_ref[0] - fwd_x * right_ref[2],
		fwd_x * right_ref[1] - fwd_y * right_ref[0],
	)
	# atan2( tilt toward right, alignment with unrolled up ).
	roll = math.atan2(
		up_x * right_ref[0] + up_y * right_ref[1] + up_z * right_ref[2],
		up_x * up_ref[0] + up_y * up_ref[1] + up_z * up_ref[2],
	)
	return ListenerPose(eye=(sx, sy, sz), yaw=yaw, pitch=pitch, roll=roll)


def aabb_from_cube_openxr(
	pos: list[float],
	half: float,
) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
	"""Axis-aligned bounds of a cube in Spherion space (from OpenXR center)."""
	cx, cy, cz = openxr_to_spherion(pos[0], pos[1], pos[2])
	return (
		(cx - half, cy - half, cz - half),
		(cx + half, cy + half, cz + half),
	)


@dataclass
class SpatialLoop:
	"""Runtime handle for one looping spatial source."""

	entity_id: int
	asset_key: str
	base_x: float
	base_y: float
	base_z: float
	base_gain: float
	vx: float = 0.0
	vy: float = 0.0
	vz: float = 0.0
	directivity: str = "omni"
	# Radiation axis in **Spherion** space (+Z forward); spawn_loop converts from OpenXR.
	directivity_axis: tuple[float, float, float] = (0.0, 0.0, 1.0)
	speaker_radius_m: float = 0.0
	last_x: float = 0.0
	last_y: float = 0.0
	last_z: float = 0.0


def _resolve_hrtf(filename: str = DEFAULT_HRTF_FILENAME) -> str:
	"""Return absolute HRTF path when the SOFA file is present."""
	path = _ASSETS_DIR / filename
	return str(path) if path.is_file() else ""


def _build_engine_config(*, order: int, block_size: int, hrtf_path: str) -> object:
	"""Build a Spherion EngineConfig with NULLSIGHT-style physics flags."""
	import spherion

	config = spherion.EngineConfig()
	config.sample_rate_hz = 48_000
	config.order = order
	config.block_size = block_size
	config.hrtf_path = hrtf_path
	config.max_audible_distance_m = 80.0
	config.distance_rolloff = 1
	config.reference_distance_m = 1.0
	config.min_distance_m = 0.5
	config.enable_hf_air_absorption = True
	# Doppler needs source + listener world velocities (m/s) each frame — see tick().
	config.enable_doppler = True
	config.enable_propagation_delay = True
	config.enable_head_shadow = True
	config.use_atmospheric_conditions = False
	config.atmospheric_temperature_c = 20.0
	config.atmospheric_relative_humidity_percent = 50.0
	# Point-like diegetic SFX: guide says disable stereo-width injection into ACN1.
	config.preserve_stereo_width = False
	return config


def _apply_room_acoustics(engine: object, room: RoomData) -> tuple[float, float]:
	"""Configure late-reverb bus + modal resonators from Sabine/Eyring geometry."""
	engine.set_use_atmospheric_conditions(False)
	shell = getattr(room, "shell_material", "concrete") or "concrete"
	zone = reverb_zone_from_geometry(
		room.name,
		room.boundaries,
		material_name=shell,
		door_open_frac=0.0,
		occluders=room.occluders,
		occluder_materials=room.occluder_materials,
	)
	wet = 0.0
	setter = getattr(engine, "set_late_reverb", None)
	if setter is not None:
		if not zone.enabled:
			setter(zone.rt60_s, 0.0)
			wet = 0.0
		else:
			# Match NULLSIGHT nullsight_audio._apply_room_acoustics wet floor.
			wet = max(wet_level_from_rt60(zone.rt60_s), 0.35 if zone.enabled else 0.0)
			setter(zone.rt60_s, wet)

	modal_setter = getattr(engine, "set_late_reverb_modal_resonators", None)
	if modal_setter is not None:
		if zone.enabled and zone.rt60_s > 0.05:
			modes, f_s = modes_from_boundaries(
				room.boundaries, rt60_s=zone.rt60_s, max_freq_hz=250.0
			)
			freqs, gains, qs = modal_resonator_params(modes, schroeder_hz=f_s)
			modal_setter(freqs, gains, qs)
		else:
			modal_setter([], [], [])
	return (zone.rt60_s if zone.enabled else 0.0), wet


class HyperVRAudio:
	"""Owns Spherion AudioWorld + AcousticSystem; syncs each VR frame."""

	def __init__(
		self,
		room_width: float,
		room_depth: float,
		room_height: float,
		*,
		shell_material: str = "concrete",
		order: int = 3,
		block_size: int = 256,
		master_gain: float = 1.0,
		spatial_gain: float = 2.0,
		enabled: bool = True,
	) -> None:
		self._world = None
		self._reason: str | None = None
		self._spatial_gain = spatial_gain
		self._loops: list[SpatialLoop] = []
		self._acoustic = AcousticSystem()
		self._acoustic_links: dict[int, AcousticLink] = {}
		self._obstruction_gain: dict[int, float] = {}
		self._listener_yaw_rad = 0.0
		self._listener_pitch_rad = 0.0
		self._listener_roll_rad = 0.0
		# Previous eye (Spherion m) for listener velocity when Doppler is on.
		self._last_listener_eye: tuple[float, float, float] | None = None
		self._listener_vx: tuple[float, float, float] = (0.0, 0.0, 0.0)
		self._last_live_rt60_s = -1.0
		self._last_live_wet = -1.0
		self._room_w = room_width
		self._room_d = room_depth
		self._room_h = room_height
		self._shell_material = shell_material
		# Base empty room in Spherion space (OpenXR Z flipped → same extents).
		self._room = box_room(
			room_width,
			room_depth,
			room_height,
			name="hypervr",
			shell_material=shell_material,
		)

		if not enabled:
			self._reason = "disabled"
			return

		try:
			import spherion  # noqa: F401
		except ImportError:
			self._reason = "spherion not installed (pip install -e ../spherion)"
			return

		hrtf_path = _resolve_hrtf()
		try:
			config = _build_engine_config(order=order, block_size=block_size, hrtf_path=hrtf_path)
			self._world = spherion.AudioWorld(config=config)
			self._world.set_master_gain(master_gain)
			rt60_s, wet = _apply_room_acoustics(self._world.engine, self._room)
			self._last_live_rt60_s = rt60_s
			self._last_live_wet = wet
			# Seed listener at standing eye height so sound is audible before HMD pose arrives.
			self._world.sync_listener(0.0, 1.6, 0.0, 0.0, 0.0)
			self._world.start()
			device = getattr(self._world, "playback_device_name", "") or ""
			print(f"audio: playing on default device: {device or '(unnamed)'}")
		except Exception as exc:  # noqa: BLE001 — device / HRTF failures are non-fatal
			self._world = None
			self._reason = f"audio init failed: {exc}"

	@property
	def active(self) -> bool:
		"""True when Spherion is running."""
		return self._world is not None and self._world.is_running

	@property
	def reason(self) -> str | None:
		"""Why audio is inactive, if any."""
		return self._reason

	def spawn_loop(
		self,
		asset_key: str,
		x_openxr: float,
		y_openxr: float,
		z_openxr: float,
		*,
		gain: float = 0.35,
		directivity: str = "omni",
		directivity_axis_openxr: tuple[float, float, float] = (0.0, 0.0, -1.0),
		speaker_radius_m: float = 0.0,
		wav_path: str | Path | None = None,
	) -> int | None:
		"""Spawn a looping spatial source at an OpenXR world position.

		``wav_path`` wins when set; otherwise ``assets/<asset_key>.wav`` is used.
		"""
		if self._world is None:
			return None
		path = Path(wav_path) if wav_path is not None else (_ASSETS_DIR / f"{asset_key}.wav")
		if not path.is_file():
			return None
		sx, sy, sz = openxr_to_spherion(x_openxr, y_openxr, z_openxr)
		axis = openxr_to_spherion(*directivity_axis_openxr)
		entity_id = self._world.spawn(
			str(path),
			sx,
			sy,
			sz,
			loop=True,
			gain=gain * self._spatial_gain,
		)
		self._loops.append(
			SpatialLoop(
				entity_id=entity_id,
				asset_key=asset_key,
				base_x=sx,
				base_y=sy,
				base_z=sz,
				base_gain=gain,
				directivity=directivity,
				directivity_axis=axis,
				speaker_radius_m=speaker_radius_m,
				last_x=sx,
				last_y=sy,
				last_z=sz,
			)
		)
		return entity_id

	def set_source_position_openxr(self, entity_id: int, x: float, y: float, z: float, dt: float) -> None:
		"""Move a source; estimates velocity for Doppler with spike rejection."""
		if self._world is None or not self._world.contains(entity_id):
			return
		sx, sy, sz = openxr_to_spherion(x, y, z)
		loop = self._loop_by_id(entity_id)
		if loop is not None and dt > 1e-5:
			vx = (sx - loop.last_x) / dt
			vy = (sy - loop.last_y) / dt
			vz = (sz - loop.last_z) / dt
			# Reject frame-time spikes (teleports / hitch) that click Doppler.
			speed = math.sqrt(vx * vx + vy * vy + vz * vz)
			if speed > 12.0:
				vx = vy = vz = 0.0
			else:
				# Light exponential smooth on velocity.
				a = 0.25
				loop.vx = loop.vx * (1.0 - a) + vx * a
				loop.vy = loop.vy * (1.0 - a) + vy * a
				loop.vz = loop.vz * (1.0 - a) + vz * a
			loop.last_x, loop.last_y, loop.last_z = sx, sy, sz
			loop.base_x, loop.base_y, loop.base_z = sx, sy, sz
		self._world.set_position(entity_id, sx, sy, sz)
		if loop is not None:
			self._set_source_velocity(entity_id, loop.vx, loop.vy, loop.vz)

	@property
	def playback_device_name(self) -> str:
		"""Opened default playback endpoint name."""
		if self._world is None:
			return ""
		return str(getattr(self._world, "playback_device_name", "") or "")

	def restart_default_device(self) -> bool:
		"""Stop and reopen the OS default playback device (picks up VR headset switch)."""
		if self._world is None:
			return False
		try:
			self._world.stop()
			self._world.start()
			device = self.playback_device_name
			print(f"audio: restarted on default device: {device or '(unnamed)'}")
			return self._world.is_running
		except Exception as exc:  # noqa: BLE001
			self._reason = f"audio device restart failed: {exc}"
			return False

	def tick(
		self,
		head_pose_openxr: list[float] | None,
		dt: float,
		*,
		extra_occluders_openxr: list[tuple[list[float], float, str]] | None = None,
		listener_velocity_openxr: tuple[float, float, float] | None = None,
	) -> None:
		"""Sync listener, rebuild room occluders, trace acoustics, push links to Spherion.

		``extra_occluders_openxr`` entries are ``(center_xyz, half_extent, material)``.
		When ``head_pose_openxr`` is None (pre-tracking), a standing fallback listener is used
		so the cube emitter stays audible on the default device.

		Listener pose/velocity are pushed in **Spherion space** per the guide:
		``sync_listener(eye, yaw, pitch, roll)`` with radians, and
		``sync_listener_velocity`` whenever Doppler is enabled.
		"""
		if self._world is None:
			return

		if head_pose_openxr is not None:
			listener = head_pose_to_listener(head_pose_openxr)
		else:
			# OpenXR mid-eye not ready yet — keep emitting relative to room spawn.
			listener = ListenerPose(eye=(0.0, 1.6, 0.0), yaw=0.0, pitch=0.0, roll=0.0)

		self._listener_yaw_rad = listener.yaw
		self._listener_pitch_rad = listener.pitch
		self._listener_roll_rad = listener.roll
		self._world.sync_listener(
			listener.eye[0],
			listener.eye[1],
			listener.eye[2],
			listener.yaw,
			listener.pitch,
			listener.roll,
		)

		# Doppler: world-space listener velocity in m/s (Spherion axes).
		# Prefer explicit OpenXR velocity when provided; else differentiate eye pose
		# (head_pose already includes locomotion offset — full listener motion).
		vx = vy = vz = 0.0
		if listener_velocity_openxr is not None:
			vx, vy, vz = openxr_to_spherion(*listener_velocity_openxr)
		elif self._last_listener_eye is not None and dt > 1e-5:
			lx, ly, lz = self._last_listener_eye
			vx = (listener.eye[0] - lx) / dt
			vy = (listener.eye[1] - ly) / dt
			vz = (listener.eye[2] - lz) / dt
			speed = math.sqrt(vx * vx + vy * vy + vz * vz)
			# Reject hitch / teleport spikes (same cap as source velocity).
			if speed > 12.0:
				vx = vy = vz = 0.0
			else:
				# Light smooth so block-rate Doppler doesn't zipper on tracking jitter.
				a = 0.35
				prev = getattr(self, "_listener_vx", (0.0, 0.0, 0.0))
				vx = prev[0] * (1.0 - a) + vx * a
				vy = prev[1] * (1.0 - a) + vy * a
				vz = prev[2] * (1.0 - a) + vz * a
		self._listener_vx = (vx, vy, vz)
		self._last_listener_eye = listener.eye
		vel_setter = getattr(self._world, "sync_listener_velocity", None)
		if vel_setter is not None:
			vel_setter(vx, vy, vz)

		# Rebuild room with dynamic AABBs (e.g. physics cube) each tick.
		extras = []
		mats = []
		for center, half, material in extra_occluders_openxr or []:
			extras.append(aabb_from_cube_openxr(center, half))
			mats.append(material)
		self._room = box_room(
			self._room_w,
			self._room_d,
			self._room_h,
			name="hypervr",
			shell_material=self._shell_material,
			extra_occluders=extras,
			extra_materials=mats,
		)

		sources = self._all_sources()
		snapshot = self._acoustic.snapshot(self._room, None, sources, listener)
		links = self._acoustic.update(snapshot, dt)
		self.apply_acoustic_links(links)

	def _all_sources(self) -> list[AcousticSource]:
		"""Build AcousticSource list from active loops."""
		sources: list[AcousticSource] = []
		for loop in self._loops:
			x, y, z = loop.base_x, loop.base_y, loop.base_z
			if self._world is not None and self._world.contains(loop.entity_id):
				ent = self._world.get_entity(loop.entity_id)
				x, y, z = ent.x, ent.y, ent.z
			sources.append(
				AcousticSource(
					source_id=loop.entity_id,
					position=(x, y, z),
					enabled=True,
					velocity=(loop.vx, loop.vy, loop.vz),
					directivity=loop.directivity,
					directivity_axis=loop.directivity_axis,
					speaker_radius_m=loop.speaker_radius_m,
				)
			)
		return sources

	def apply_acoustic_links(self, links: dict[int, AcousticLink]) -> None:
		"""Push path-native dry gain, LPF, early taps, and reverb send to Spherion."""
		if self._world is None:
			return

		seen: set[int] = set()
		min_predelay = None
		live_rt60 = None
		for source_id, link in links.items():
			seen.add(source_id)
			self._acoustic_links[source_id] = link
			gain, lpf_hz = self._effective_obstruction(link)
			self._obstruction_gain[source_id] = gain
			if link.reverb_predelay_s > 0.0:
				if min_predelay is None or link.reverb_predelay_s < min_predelay:
					min_predelay = link.reverb_predelay_s
			if link.rt60_s > 0.0:
				if live_rt60 is None or link.rt60_s > live_rt60:
					live_rt60 = link.rt60_s
			loop = self._loop_by_id(source_id)
			if loop is None:
				continue
			self._world.set_gain(source_id, loop.base_gain * self._spatial_gain)
			self._set_dry_path_gain(source_id, gain)
			self._set_obstruction_lowpass(source_id, lpf_hz)
			self._set_reflection_taps(source_id, link)
			send = link.reverb_send
			combined = self._combined_early_taps(link)
			cluster = list(link.reflection_taps) + list(link.diffraction_extra)
			if len(cluster) > len(combined):
				kept_ids = {(round(t.delay_s, 4), round(t.gain, 3)) for t in combined}
				leftover = 0.0
				for tap in cluster:
					key = (round(tap.delay_s, 4), round(tap.gain, 3))
					if key not in kept_ids:
						leftover += tap.gain * tap.gain
				send = min(0.65, send + 0.12 * math.sqrt(max(0.0, leftover)))
			self._set_reverb_send(source_id, send)
			self._set_source_velocity(source_id, loop.vx, loop.vy, loop.vz)

		if min_predelay is not None:
			setter = getattr(self._world, "set_late_reverb_predelay_s", None)
			if setter is not None:
				setter(min(0.25, max(0.0, min_predelay)))

		if live_rt60 is not None:
			self._push_live_rt60(live_rt60)

		for source_id in list(self._obstruction_gain):
			if source_id not in seen:
				del self._obstruction_gain[source_id]
				self._acoustic_links.pop(source_id, None)

	def acoustic_debug_lines(self) -> list[str]:
		"""Short HUD-style lines for console / overlay."""
		lines: list[str] = []
		if self._last_live_rt60_s > 0.0:
			lines.append(f"RT60 {self._last_live_rt60_s:.2f}s  WET {self._last_live_wet:.2f}")
		for loop in self._loops:
			link = self._acoustic_links.get(loop.entity_id)
			if link is None:
				continue
			label = loop.asset_key[:8].upper()
			lines.append(
				f"{label} {link.path_mode.upper()} "
				f"G{link.direct_gain:.2f}/{link.transmission_gain:.2f} "
				f"D{link.diffraction_gain:.2f} "
				f"R{link.reflection_gain:.2f}x{len(link.reflection_taps)} "
				f"V{link.reverb_send:.2f}"
			)
		return lines[:8]

	def shutdown(self) -> None:
		"""Stop the audio device."""
		if self._world is not None:
			try:
				self._world.stop()
			except RuntimeError:
				pass
			self._world = None
		self._acoustic.clear()

	def _push_live_rt60(self, rt60_s: float) -> None:
		"""Update Spherion late-reverb when Sabine RT60 changes."""
		if self._world is None:
			return
		wet = max(wet_level_from_rt60(rt60_s), 0.35)
		if abs(rt60_s - self._last_live_rt60_s) < 0.02 and abs(wet - self._last_live_wet) < 0.02:
			return
		setter = getattr(self._world, "set_late_reverb", None)
		if setter is not None:
			setter(rt60_s, wet)
			self._last_live_rt60_s = rt60_s
			self._last_live_wet = wet

	def _effective_obstruction(self, link: AcousticLink) -> tuple[float, float]:
		"""Dry-path gain from direct + transmission only (never diffraction)."""
		gain = max(link.direct_gain, link.transmission_gain)
		candidates = (
			(link.direct_gain, link.direct_lpf_hz),
			(link.transmission_gain, link.transmission_lpf_hz),
		)
		loudest = max(candidates, key=lambda item: item[0])
		lpf_hz = loudest[1]
		if link.direct_gain > 0.05 and link.transmission_gain > 0.05:
			lpf_hz = min(link.direct_lpf_hz, link.transmission_lpf_hz)
		if gain < 1e-4:
			lpf_hz = 20_000.0
		return gain, lpf_hz

	def _combined_early_taps(self, link: AcousticLink) -> list[ReflectionTap]:
		"""Merge specular + diffracted + structure-borne taps (max 8)."""
		max_taps = 8
		specular = list(link.reflection_taps)
		diff_taps: list[ReflectionTap] = []
		if link.diffraction_gain > 0.02 and link.diffraction_delay_s >= 0.0:
			diff_taps.append(
				ReflectionTap(
					gain=link.diffraction_gain,
					delay_s=min(0.35, link.diffraction_delay_s),
					lpf_hz=link.diffraction_lpf_hz,
					bounce_order=0,
					is_diffuse=True,
				)
			)
		for extra in link.diffraction_extra:
			if extra.gain > 0.02:
				diff_taps.append(extra)
		if link.structure_borne_gain > 0.02:
			diff_taps.append(
				ReflectionTap(
					gain=min(0.35, link.structure_borne_gain),
					delay_s=min(0.12, max(0.0, link.structure_borne_delay_s)),
					lpf_hz=link.structure_borne_lpf_hz,
					bounce_order=0,
					arrival_dir=(0.0, -1.0, 0.0),
					band_energy=(1.0, 0.7, 0.35, 0.15, 0.05, 0.02),
					is_diffuse=True,
				)
			)

		reserve = min(len(diff_taps), 3)
		max_specular = max(1, max_taps - reserve)
		ordered = sorted(specular, key=lambda t: -t.gain)
		merged: list[ReflectionTap] = []
		for tap in ordered:
			duplicate = False
			for index, existing in enumerate(merged):
				if abs(existing.delay_s - tap.delay_s) <= 0.0015:
					g = (existing.gain * existing.gain + tap.gain * tap.gain) ** 0.5
					merged[index] = ReflectionTap(
						gain=min(1.2, g),
						delay_s=min(existing.delay_s, tap.delay_s),
						lpf_hz=max(existing.lpf_hz, tap.lpf_hz),
						bounce_order=min(existing.bounce_order, tap.bounce_order),
						arrival_dir=existing.arrival_dir,
						band_energy=existing.band_energy,
						is_diffuse=existing.is_diffuse or tap.is_diffuse,
					)
					duplicate = True
					break
			if not duplicate:
				merged.append(tap)
			if len(merged) >= max_specular:
				break

		for diff_tap in diff_taps:
			placed = False
			for index, existing in enumerate(merged):
				if abs(existing.delay_s - diff_tap.delay_s) <= 0.0015:
					g = (existing.gain * existing.gain + diff_tap.gain * diff_tap.gain) ** 0.5
					merged[index] = ReflectionTap(
						gain=min(1.2, g),
						delay_s=min(existing.delay_s, diff_tap.delay_s),
						lpf_hz=max(existing.lpf_hz, diff_tap.lpf_hz),
						bounce_order=0,
						arrival_dir=diff_tap.arrival_dir,
						band_energy=diff_tap.band_energy,
						is_diffuse=True,
					)
					placed = True
					break
			if not placed and len(merged) < max_taps:
				merged.append(diff_tap)

		merged.sort(key=lambda t: t.delay_s)
		return merged[:max_taps]

	def _loop_by_id(self, entity_id: int) -> SpatialLoop | None:
		"""Find SpatialLoop by entity id."""
		for loop in self._loops:
			if loop.entity_id == entity_id:
				return loop
		return None

	def _set_obstruction_lowpass(self, entity_id: int, cutoff_hz: float) -> None:
		"""Push obstruction LPF cutoff."""
		if self._world is None:
			return
		setter = getattr(self._world, "set_obstruction_lowpass", None)
		if setter is not None:
			setter(entity_id, cutoff_hz)

	def _set_dry_path_gain(self, entity_id: int, gain: float) -> None:
		"""Push dry-path (LOS/transmission) gain."""
		if self._world is None:
			return
		setter = getattr(self._world, "set_dry_path_gain", None)
		if setter is not None:
			setter(entity_id, gain)

	def _set_reflection_taps(self, entity_id: int, link: AcousticLink) -> None:
		"""Push extended early-reflection taps when Spherion supports them."""
		if self._world is None:
			return
		extended = getattr(self._world, "set_reflection_taps_extended", None)
		multi = getattr(self._world, "set_reflection_taps", None)
		taps = self._combined_early_taps(link)
		if extended is not None and taps:
			gains: list[float] = []
			delays: list[float] = []
			lpfs: list[float] = []
			azimuths: list[float] = []
			elevations: list[float] = []
			band_low: list[float] = []
			band_mid: list[float] = []
			band_high: list[float] = []
			for tap in taps:
				az, el = world_dir_to_listener_angles(
					tap.arrival_dir,
					self._listener_yaw_rad,
					self._listener_pitch_rad,
				)
				bl, bm, bh = band_energy_to_three_band_gains(tap.band_energy)
				gains.append(tap.gain)
				delays.append(tap.delay_s)
				lpfs.append(tap.lpf_hz)
				azimuths.append(az)
				elevations.append(el)
				band_low.append(bl)
				band_mid.append(bm)
				band_high.append(bh)
			extended(
				entity_id,
				gains,
				delays,
				lpfs,
				azimuths,
				elevations,
				band_low,
				band_mid,
				band_high,
			)
			return
		if multi is not None and taps:
			multi(entity_id, [t.gain for t in taps], [t.delay_s for t in taps], [t.lpf_hz for t in taps])
			return
		if taps:
			best = max(taps, key=lambda t: t.gain)
			setter = getattr(self._world, "set_reflection_tap", None)
			if setter is not None:
				setter(entity_id, best.gain, best.delay_s, best.lpf_hz)

	def _set_reverb_send(self, entity_id: int, send: float) -> None:
		"""Push late-reverb wet send."""
		if self._world is None:
			return
		setter = getattr(self._world, "set_reverb_send", None)
		if setter is not None:
			setter(entity_id, send)

	def _set_source_velocity(self, entity_id: int, vx: float, vy: float, vz: float) -> None:
		"""Push Doppler velocity."""
		if self._world is None:
			return
		setter = getattr(self._world, "set_velocity", None)
		if setter is not None:
			setter(entity_id, vx, vy, vz)


def ensure_python_path() -> None:
	"""Put ``HyperVR/python`` on ``sys.path`` so acoustics imports resolve."""
	root = str(_PYTHON_ROOT)
	if root not in sys.path:
		sys.path.insert(0, root)
