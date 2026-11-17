"""
HyperVR main loop — room + procedural IK arms + physics cube (grab / throw).

Spatial audio: NULLSIGHT acoustics pipeline + Spherion HOA/HRTF (see hypervr_audio).
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

# HyperVR/python on path for acoustics + Spherion bridge.
_PYTHON_ROOT = Path(__file__).resolve().parent
if str(_PYTHON_ROOT) not in sys.path:
	sys.path.insert(0, str(_PYTHON_ROOT))

import hypervr
from hypervr_audio import HyperVRAudio


def ensure_cube_noise_wav(path: Path) -> Path:
	"""Write a soft HyperVR-only stereo loop for the test cube.

	Warm low sine with gentle harmonics (no harsh buzz / noise bed).
	"""
	import struct
	import wave

	path.parent.mkdir(parents=True, exist_ok=True)
	sample_rate = 48_000
	duration_s = 2.0
	n = int(sample_rate * duration_s)
	# Soft "object presence" pad — fundamental + quiet octave/fifth.
	f0 = 164.81  # E3
	amp = 0.11
	frames: list[int] = []
	for i in range(n):
		t = i / sample_rate
		# Seamless loop fades at the seam.
		env = 1.0
		edge = 0.04
		if t < edge:
			env = t / edge
		elif t > duration_s - edge:
			env = (duration_s - t) / edge
		# Slow amplitude breathe so it feels alive without being noisy.
		breathe = 0.92 + 0.08 * math.sin(2.0 * math.pi * 0.35 * t)
		# Slight stereo chorus via tiny L/R detune.
		phase_l = 2.0 * math.pi * f0 * t
		phase_r = 2.0 * math.pi * (f0 * 1.0015) * t
		tone_l = (
			0.70 * math.sin(phase_l)
			+ 0.18 * math.sin(2.0 * phase_l)
			+ 0.08 * math.sin(3.0 * phase_l)
			+ 0.04 * math.sin(2.0 * math.pi * (f0 * 1.5) * t)
		)
		tone_r = (
			0.70 * math.sin(phase_r)
			+ 0.18 * math.sin(2.0 * phase_r)
			+ 0.08 * math.sin(3.0 * phase_r)
			+ 0.04 * math.sin(2.0 * math.pi * (f0 * 1.5) * t)
		)
		sample_l = max(-1.0, min(1.0, amp * env * breathe * tone_l))
		sample_r = max(-1.0, min(1.0, amp * env * breathe * tone_r))
		frames.append(int(sample_l * 32767.0))
		frames.append(int(sample_r * 32767.0))

	with wave.open(str(path), "w") as wav:
		wav.setnchannels(2)
		wav.setsampwidth(2)
		wav.setframerate(sample_rate)
		wav.writeframes(struct.pack(f"<{len(frames)}h", *frames))
	return path


def mat_translation(x: float, y: float, z: float) -> list[float]:
	"""Column-major translation matrix."""
	return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1]


def mat_scale(s: float) -> list[float]:
	"""Uniform scale matrix."""
	return [s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1]


def mat_mul(a: list[float], b: list[float]) -> list[float]:
	"""Multiply two column-major 4x4 matrices."""
	out = [0.0] * 16
	for col in range(4):
		for row in range(4):
			out[col * 4 + row] = sum(a[k * 4 + row] * b[col * 4 + k] for k in range(4))
	return out


def mat_invert_rigid(m: list[float]) -> list[float]:
	"""Inverse of a rigid (rotation + translation) transform."""
	out = [0.0] * 16
	out[0], out[1], out[2] = m[0], m[4], m[8]
	out[4], out[5], out[6] = m[1], m[5], m[9]
	out[8], out[9], out[10] = m[2], m[6], m[10]
	out[15] = 1.0
	out[12] = -(out[0] * m[12] + out[4] * m[13] + out[8] * m[14])
	out[13] = -(out[1] * m[12] + out[5] * m[13] + out[9] * m[14])
	out[14] = -(out[2] * m[12] + out[6] * m[13] + out[10] * m[14])
	return out


def mat_distance(a: list[float], b: list[float]) -> float:
	"""Distance between two transform origins."""
	dx = a[12] - b[12]
	dy = a[13] - b[13]
	dz = a[14] - b[14]
	return math.sqrt(dx * dx + dy * dy + dz * dz)


def mat_origin(m: list[float]) -> tuple[float, float, float]:
	"""Translation of a matrix."""
	return m[12], m[13], m[14]


def mat_rotation(m: list[float]) -> list[float]:
	"""3x3 rotation block as a Mat4 (scale stripped)."""
	sx = math.sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2])
	if sx < 1e-4:
		sx = 1.0
	out = mat_scale(1.0)
	out[0], out[1], out[2] = m[0] / sx, m[1] / sx, m[2] / sx
	out[4], out[5], out[6] = m[4] / sx, m[5] / sx, m[6] / sx
	out[8], out[9], out[10] = m[8] / sx, m[9] / sx, m[10] / sx
	return out


def v_len(v: tuple[float, float, float]) -> float:
	"""Vector length."""
	return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def integrate_orientation(orient: list[float], ang: tuple[float, float, float], dt: float) -> list[float]:
	"""Apply world angular velocity to an orientation matrix."""
	wx, wy, wz = ang
	w = math.sqrt(wx * wx + wy * wy + wz * wz)
	if w < 1e-5:
		return orient
	ax, ay, az = wx / w, wy / w, wz / w
	angle = w * dt
	c = math.cos(angle)
	s = math.sin(angle)
	t = 1.0 - c
	r = mat_scale(1.0)
	r[0] = t * ax * ax + c
	r[1] = t * ax * ay + s * az
	r[2] = t * ax * az - s * ay
	r[4] = t * ax * ay - s * az
	r[5] = t * ay * ay + c
	r[6] = t * ay * az + s * ax
	r[8] = t * ax * az + s * ay
	r[9] = t * ay * az - s * ax
	r[10] = t * az * az + c
	return mat_mul(r, orient)


class CubeBody:
	"""Rigid cube with gravity, room collisions, grab, and throw."""

	def __init__(self, half: float = 0.075) -> None:
		self.half = half
		self.pos = [0.0, half + 0.02, -1.2]
		self.vel = [0.0, 0.0, 0.0]
		self.ang_vel = [0.0, 0.0, 0.0]
		self.orient = mat_scale(1.0)
		self.held_by: int | None = None
		self.grab_offset = mat_scale(1.0)
		self.last_grip = (0.0, 0.0, 0.0)
		self.have_last_grip = False

	def model_matrix(self) -> list[float]:
		"""World model matrix."""
		return mat_mul(
			mat_translation(self.pos[0], self.pos[1], self.pos[2]),
			mat_mul(self.orient, mat_scale(self.half * 2.0)),
		)

	def from_pose(self, pose: list[float]) -> None:
		"""Sync center + orientation from a world pose."""
		self.pos = [pose[12], pose[13], pose[14]]
		self.orient = mat_rotation(pose)


def v_dot(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
	"""Dot product."""
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def v_cross(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
	"""Cross product."""
	return (
		a[1] * b[2] - a[2] * b[1],
		a[2] * b[0] - a[0] * b[2],
		a[0] * b[1] - a[1] * b[0],
	)


def v_add(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
	"""Vector add."""
	return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def v_scale(a: tuple[float, float, float], s: float) -> tuple[float, float, float]:
	"""Vector scale."""
	return (a[0] * s, a[1] * s, a[2] * s)


def obb_support(
	center: list[float],
	orient: list[float],
	half: float,
	direction: tuple[float, float, float],
) -> tuple[float, float, float]:
	"""Farthest OBB corner along direction (real cube shape, not a sphere)."""
	axes = (
		(orient[0], orient[1], orient[2]),
		(orient[4], orient[5], orient[6]),
		(orient[8], orient[9], orient[10]),
	)
	point = (center[0], center[1], center[2])
	for axis in axes:
		s = 1.0 if v_dot(axis, direction) >= 0.0 else -1.0
		point = v_add(point, v_scale(axis, half * s))
	return point


def resolve_obb_plane(
	cube: CubeBody,
	normal: tuple[float, float, float],
	plane: float,
	restitution: float,
	linear_friction: float,
	angular_friction: float,
) -> bool:
	"""Push oriented cube out of one room plane (n·x >= plane)."""
	support = obb_support(cube.pos, cube.orient, cube.half, v_scale(normal, -1.0))
	penetration = plane - v_dot(support, normal)
	if penetration <= 0.0:
		return False

	cube.pos = [
		cube.pos[0] + normal[0] * penetration,
		cube.pos[1] + normal[1] * penetration,
		cube.pos[2] + normal[2] * penetration,
	]

	vel = (cube.vel[0], cube.vel[1], cube.vel[2])
	vn = v_dot(vel, normal)
	if vn < 0.0:
		e = 0.0 if abs(vn) < 0.8 else restitution
		vel = v_add(vel, v_scale(normal, -vn * (1.0 + e)))

	vn_after = v_dot(vel, normal)
	tangent = v_scale(v_add(vel, v_scale(normal, -vn_after)), linear_friction)
	vel = v_add(v_scale(normal, vn_after), tangent)
	cube.vel = [vel[0], vel[1], vel[2]]
	cube.ang_vel = [a * angular_friction for a in cube.ang_vel]
	return True


def nearest_up_axis(orient: list[float]) -> tuple[tuple[float, float, float], float]:
	"""Return (upward-pointing cube axis, |alignment| with world up)."""
	up = (0.0, 1.0, 0.0)
	axes = (
		(orient[0], orient[1], orient[2]),
		(orient[4], orient[5], orient[6]),
		(orient[8], orient[9], orient[10]),
	)
	best_abs = 0.0
	best = (0.0, 1.0, 0.0)
	for axis in axes:
		d = v_dot(axis, up)
		ad = abs(d)
		if ad > best_abs:
			best_abs = ad
			best = axis if d >= 0.0 else v_scale(axis, -1.0)
	return best, best_abs


def rotate_orient_towards(orient: list[float], frm: tuple[float, float, float], to: tuple[float, float, float]) -> list[float]:
	"""One-shot rotate so frm maps onto to."""
	fl = v_len(frm)
	tl = v_len(to)
	if fl < 1e-6 or tl < 1e-6:
		return orient
	frm = v_scale(frm, 1.0 / fl)
	to = v_scale(to, 1.0 / tl)
	cos_a = max(-1.0, min(1.0, v_dot(frm, to)))
	axis = v_cross(frm, to)
	axis_len = v_len(axis)
	if axis_len < 1e-6:
		return orient
	axis = v_scale(axis, 1.0 / axis_len)
	angle = math.acos(cos_a)
	return integrate_orientation(orient, v_scale(axis, angle), 1.0)


def settle_cube_on_floor(cube: CubeBody, dt: float) -> None:
	"""Gentle tip while tilted; hard flatten when close (no shake motor)."""
	up = (0.0, 1.0, 0.0)
	face, alignment = nearest_up_axis(cube.orient)

	if alignment > 0.97:
		cube.orient = rotate_orient_towards(cube.orient, face, up)
		cube.ang_vel = [0.0, 0.0, 0.0]
		return

	if alignment > 0.88:
		cube.orient = rotate_orient_towards(cube.orient, face, up)
		cube.ang_vel = [a * 0.4 for a in cube.ang_vel]
		return

	contact = obb_support(cube.pos, cube.orient, cube.half, (0.0, -1.0, 0.0))
	r = (contact[0] - cube.pos[0], contact[1] - cube.pos[1], contact[2] - cube.pos[2])
	torque = v_scale(v_cross(r, v_scale(up, 9.81)), 18.0 * dt)
	cube.ang_vel = [cube.ang_vel[i] + torque[i] for i in range(3)]
	spin = v_len(tuple(cube.ang_vel))
	if spin > 10.0:
		cube.ang_vel = [a * (10.0 / spin) for a in cube.ang_vel]


def integrate_cube(cube: CubeBody, dt: float, half_w: float, half_d: float, height: float) -> None:
	"""Integrate free cube with OBB vs room-plane contacts."""
	if dt <= 1e-5 or dt > 0.1:
		return

	gravity = -9.81
	restitution = 0.25
	air_drag = 0.15
	ang_drag = 1.2
	sleep_speed = 0.06

	cube.vel[1] += gravity * dt
	damp_air = math.exp(-air_drag * dt)
	cube.vel = [cube.vel[0] * damp_air, cube.vel[1] * damp_air, cube.vel[2] * damp_air]
	damp_ang = math.exp(-ang_drag * dt)
	cube.ang_vel = [a * damp_ang for a in cube.ang_vel]

	cube.pos = [cube.pos[i] + cube.vel[i] * dt for i in range(3)]
	cube.orient = integrate_orientation(cube.orient, tuple(cube.ang_vel), dt)

	floor_lin = math.exp(-6.0 * dt)
	floor_ang = math.exp(-2.5 * dt)
	wall_lin = math.exp(-1.5 * dt)
	wall_ang = math.exp(-1.5 * dt)

	on_floor = resolve_obb_plane(cube, (0.0, 1.0, 0.0), 0.0, restitution, floor_lin, floor_ang)
	resolve_obb_plane(cube, (0.0, -1.0, 0.0), -height, restitution, wall_lin, wall_ang)
	resolve_obb_plane(cube, (1.0, 0.0, 0.0), -half_w, restitution, wall_lin, wall_ang)
	resolve_obb_plane(cube, (-1.0, 0.0, 0.0), -half_w, restitution, wall_lin, wall_ang)
	resolve_obb_plane(cube, (0.0, 0.0, 1.0), -half_d, restitution, wall_lin, wall_ang)
	resolve_obb_plane(cube, (0.0, 0.0, -1.0), -half_d, restitution, wall_lin, wall_ang)

	if on_floor:
		settle_cube_on_floor(cube, dt)
		resolve_obb_plane(cube, (0.0, 1.0, 0.0), 0.0, 0.0, floor_lin, floor_ang)

	_, alignment = nearest_up_axis(cube.orient)
	lowest = obb_support(cube.pos, cube.orient, cube.half, (0.0, -1.0, 0.0))
	if on_floor and alignment > 0.97 and lowest[1] <= 2e-3 and v_len(tuple(cube.vel)) < sleep_speed:
		cube.vel = [0.0, 0.0, 0.0]
		cube.ang_vel = [0.0, 0.0, 0.0]
		cube.pos[1] += -lowest[1]


def update_grab(engine: hypervr.Engine, hand: int, cube: CubeBody, grab_radius: float) -> None:
	"""Start/continue/release a grip-grab; release imparts throw velocity."""
	grip = engine.controller_pose(hand)
	if grip is None:
		if cube.held_by == hand:
			cube.held_by = None
			cube.have_last_grip = False
		return

	squeezing = engine.button_down(hand, hypervr.BUTTON_GRIP)
	grip_pos = mat_origin(grip)
	dt = max(engine.delta_time(), 1e-4)

	if cube.held_by == hand:
		if not squeezing:
			if cube.have_last_grip:
				cube.vel = [
					(grip_pos[0] - cube.last_grip[0]) / dt,
					(grip_pos[1] - cube.last_grip[1]) / dt,
					(grip_pos[2] - cube.last_grip[2]) / dt,
				]
				cube.ang_vel = [cube.vel[2] * 2.0, -cube.vel[0] * 2.0, 0.0]
			cube.held_by = None
			cube.have_last_grip = False
			return

		held = mat_mul(grip, cube.grab_offset)
		cube.from_pose(held)
		cube.vel = [0.0, 0.0, 0.0]
		cube.ang_vel = [0.0, 0.0, 0.0]
		if cube.have_last_grip:
			cube.vel = [
				(grip_pos[0] - cube.last_grip[0]) / dt,
				(grip_pos[1] - cube.last_grip[1]) / dt,
				(grip_pos[2] - cube.last_grip[2]) / dt,
			]
		cube.last_grip = grip_pos
		cube.have_last_grip = True
		return

	if squeezing and mat_distance(grip, cube.model_matrix()) <= grab_radius:
		cube.held_by = hand
		cube.grab_offset = mat_mul(mat_invert_rigid(grip), cube.model_matrix())
		cube.vel = [0.0, 0.0, 0.0]
		cube.ang_vel = [0.0, 0.0, 0.0]
		cube.last_grip = grip_pos
		cube.have_last_grip = True
		cube.from_pose(mat_mul(grip, cube.grab_offset))


def main() -> None:
	import argparse
	import os

	parser = argparse.ArgumentParser(description="HyperVR room + IK arms + physics cube")
	parser.add_argument(
		"--desktop",
		action="store_true",
		help="Run without a headset (desktop mirror + keyboard). Also set HYPERVR_DESKTOP=1.",
	)
	args = parser.parse_args()
	desktop = bool(args.desktop or os.environ.get("HYPERVR_DESKTOP", "").strip() in ("1", "true", "True", "yes"))

	engine = hypervr.Engine("HyperVR", desktop_mode=desktop)
	print("backend:", engine.backend_name())
	if desktop or "Desktop" in engine.backend_name():
		print("Desktop: drag to look, WASD moves where you're looking, Space/Ctrl up/down, Esc quit.")
	print("You spawn inside a 12x12m room (7m ceiling). Push off the floor/walls with your hands to move (Gorilla Tag style).")
	print("Squeeze grip near the cube to grab it; release to throw.")
	print("Acoustics: the cube emits a test buzz (Spherion + NULLSIGHT path tracing).")

	room_w, room_d, room_h = 12.0, 12.0, 7.0
	room = engine.create_room(room_w, room_d, room_h)  # (mesh, width, depth, height)
	cube_mesh = engine.create_cube()
	left_arm = engine.create_hand(hypervr.HAND_LEFT)
	right_arm = engine.create_hand(hypervr.HAND_RIGHT)

	cube = CubeBody(half=0.075)
	grab_radius = 0.15 + 0.04

	# Spatial audio: one loud emitter on the physics cube (HyperVR-only test loop).
	audio = HyperVRAudio(room_w, room_d, room_h, shell_material="concrete", master_gain=0.85, spatial_gain=1.4)
	cube_sound_id: int | None = None
	if audio.active:
		cube_wav = ensure_cube_noise_wav(_PYTHON_ROOT / "assets" / "cube_buzz.wav")
		cube_sound_id = audio.spawn_loop(
			"cube_buzz",
			cube.pos[0],
			cube.pos[1],
			cube.pos[2],
			gain=0.28,
			directivity="omni",
			wav_path=cube_wav,
		)
		print(
			"audio: cube buzz attached on",
			audio.playback_device_name or "default device",
			f"(id={cube_sound_id})",
		)
	else:
		print(f"audio: inactive ({audio.reason})")

	try:
		while engine.is_running():
			engine.poll_events()
			engine.begin_frame()
			dt = engine.delta_time()

			engine.draw_room(room)

			update_grab(engine, hypervr.HAND_LEFT, cube, grab_radius)
			update_grab(engine, hypervr.HAND_RIGHT, cube, grab_radius)

			if cube.held_by is None:
				integrate_cube(cube, dt, room[1] * 0.5, room[2] * 0.5, room[3])

			held = cube.held_by is not None
			engine.draw_mesh(
				cube_mesh,
				cube.model_matrix(),
				0.35 if held else 1.0,
				0.85 if held else 0.55,
				0.40 if held else 0.20,
				1.0,
			)

			engine.draw_hand(left_arm, cube.held_by == hypervr.HAND_LEFT)
			engine.draw_hand(right_arm, cube.held_by == hypervr.HAND_RIGHT)

			# Follow the cube. Listener pose/velocity → Spherion via tick (Z-flip + radians).
			if audio.active and cube_sound_id is not None:
				head = engine.head_pose()
				audio.set_source_position_openxr(
					cube_sound_id, cube.pos[0], cube.pos[1], cube.pos[2], dt
				)
				audio.tick(head, dt)

			engine.end_frame()
	finally:
		audio.shutdown()


if __name__ == "__main__":
	main()
