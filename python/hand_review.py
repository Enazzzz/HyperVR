"""
Hand-only visual review: desktop mode, body/arms hidden, orbit screenshots.

Captures open + closed (fist) poses from close range and many angles.

Usage:
	python python/hand_review.py
	.\\scripts\\run_hand_review.ps1

Writes under artifacts/hand_review/<timestamp>/.
"""

from __future__ import annotations

import math
import sys
import time
from datetime import datetime
from pathlib import Path

_PYTHON_ROOT = Path(__file__).resolve().parent
_ROOT = _PYTHON_ROOT.parent
if str(_PYTHON_ROOT) not in sys.path:
	sys.path.insert(0, str(_PYTHON_ROOT))

import hypervr

# Desktop inspect grips sit near (±0.22, 1.20, 0).
HAND_CENTER = (0.0, 1.20, 0.0)
RIGHT_HAND = (0.22, 1.20, 0.0)
LEFT_HAND = (-0.22, 1.20, 0.0)


def look_yaw_pitch(cx: float, cy: float, cz: float, tx: float, ty: float, tz: float) -> tuple[float, float]:
	"""Camera yaw/pitch so the view looks from cam toward target."""
	dx = tx - cx
	dy = ty - cy
	dz = tz - cz
	horiz = math.sqrt(dx * dx + dz * dz)
	yaw = math.atan2(-dx, -dz) if horiz > 1.0e-6 else 0.0
	pitch = math.atan2(dy, horiz) if (horiz + abs(dy)) > 1.0e-6 else 0.0
	return yaw, pitch


def orbit_shot(
	name: str,
	yaw_deg: float,
	pitch_deg: float,
	radius: float,
	target: tuple[float, float, float] = HAND_CENTER,
) -> tuple[str, float, float, float, float, float]:
	"""Place camera on a sphere around target, looking at it."""
	yaw = math.radians(yaw_deg)
	pitch = math.radians(pitch_deg)
	cp = math.cos(pitch)
	sp = math.sin(pitch)
	fwd_x = -math.sin(yaw) * cp
	fwd_y = sp
	fwd_z = -math.cos(yaw) * cp
	x = target[0] - fwd_x * radius
	y = target[1] - fwd_y * radius
	z = target[2] - fwd_z * radius
	look_yaw, look_pitch = look_yaw_pitch(x, y, z, target[0], target[1], target[2])
	return (name, x, y, z, look_yaw, look_pitch)


# Tight orbits — gaps only show up close. Open-hand pass uses these names as-is;
# fist pass prefixes with "fist_".
OPEN_SHOTS: list[tuple[str, float, float, float, float, float]] = [
	orbit_shot("01_front", 180.0, 8.0, 0.28),
	orbit_shot("02_front_macro", 180.0, 10.0, 0.20, RIGHT_HAND),
	orbit_shot("03_cleft_macro", 180.0, 8.0, 0.16, RIGHT_HAND),
	orbit_shot("04_three_quarter_l", 145.0, 12.0, 0.22, RIGHT_HAND),
	orbit_shot("05_three_quarter_r", 215.0, 12.0, 0.22, RIGHT_HAND),
	orbit_shot("06_side_l", 90.0, 5.0, 0.20, RIGHT_HAND),
	orbit_shot("07_side_r", 270.0, 5.0, 0.20, RIGHT_HAND),
	orbit_shot("08_back", 0.0, 8.0, 0.22, RIGHT_HAND),
	orbit_shot("09_top", 180.0, -65.0, 0.20, RIGHT_HAND),
	orbit_shot("10_bottom", 180.0, 50.0, 0.20, RIGHT_HAND),
	orbit_shot("11_thumb_side", 120.0, 5.0, 0.14, RIGHT_HAND),
	orbit_shot("12_pair_close", 180.0, 8.0, 0.35),
	orbit_shot("13_left_macro", 180.0, 10.0, 0.18, LEFT_HAND),
]


def render_settled(
	engine: hypervr.Engine,
	room,
	left_arm,
	right_arm,
	close_fist: bool,
	frames: int = 4,
) -> None:
	"""Draw a few frames so the mirror readback matches the locked camera."""
	for _ in range(frames):
		engine.poll_events()
		engine.begin_frame()
		engine.draw_room(room)
		engine.draw_hand(left_arm, close_fist)
		engine.draw_hand(right_arm, close_fist)
		engine.end_frame()


def capture_pass(
	engine: hypervr.Engine,
	room,
	left_arm,
	right_arm,
	out_dir: Path,
	shots: list[tuple[str, float, float, float, float, float]],
	close_fist: bool,
	name_prefix: str,
) -> tuple[list[str], int]:
	"""Lock cameras and dump BMPs for one open/fist pass."""
	lines: list[str] = []
	ok_count = 0
	for name, x, y, z, yaw, pitch in shots:
		engine.set_desktop_camera(x, y, z, yaw, pitch)
		render_settled(engine, room, left_arm, right_arm, close_fist, frames=3)
		fname = f"{name_prefix}{name}.bmp"
		path = out_dir / fname
		ok = engine.save_desktop_screenshot(str(path))
		status = "ok" if ok else "FAIL"
		if ok:
			ok_count += 1
		line = f"{status}\t{fname}\tfist={int(close_fist)}\tx={x:.2f} y={y:.2f} z={z:.2f}"
		lines.append(line)
		print(line)
		time.sleep(0.02)
	return lines, ok_count


def main() -> int:
	out_root = _ROOT / "artifacts" / "hand_review"
	stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
	out_dir = out_root / stamp
	out_dir.mkdir(parents=True, exist_ok=True)

	print("hand_review: starting desktop engine (hands only, open + fist)...")
	engine = hypervr.Engine("HyperVR-HandReview", desktop_mode=True)
	print("backend:", engine.backend_name())
	if not engine.desktop_mirror_active():
		print("hand_review: mirror window failed to open", file=sys.stderr)
		return 1

	engine.set_avatar_visible(False)
	engine.set_arms_visible(False)

	room = engine.create_room(12.0, 12.0, 7.0)
	left_arm = engine.create_hand(hypervr.HAND_LEFT)
	right_arm = engine.create_hand(hypervr.HAND_RIGHT)

	render_settled(engine, room, left_arm, right_arm, False, frames=2)

	all_lines: list[str] = []
	ok_total = 0
	shot_total = 0

	lines, ok = capture_pass(engine, room, left_arm, right_arm, out_dir, OPEN_SHOTS, False, "open_")
	all_lines.extend(lines)
	ok_total += ok
	shot_total += len(OPEN_SHOTS)

	lines, ok = capture_pass(engine, room, left_arm, right_arm, out_dir, OPEN_SHOTS, True, "fist_")
	all_lines.extend(lines)
	ok_total += ok
	shot_total += len(OPEN_SHOTS)

	(out_dir / "manifest.txt").write_text("\n".join(all_lines) + "\n", encoding="utf-8")
	(out_root / "LATEST.txt").write_text(str(out_dir) + "\n", encoding="utf-8")
	print(f"hand_review: saved {ok_total}/{shot_total} shots -> {out_dir}")
	return 0 if ok_total == shot_total else 2


if __name__ == "__main__":
	raise SystemExit(main())
