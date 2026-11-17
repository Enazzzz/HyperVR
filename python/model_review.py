"""
Automated player-model review: desktop mode, locked camera shots, BMP dumps.

Usage:
	python python/model_review.py
	.\\scripts\\run_model_review.ps1

Writes screenshots under artifacts/model_review/<timestamp>/.
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

# OpenXR: identity looks −Z. yaw/pitch match desktop free-cam:
#   fwd_x = -sin(yaw)*cos(pitch), fwd_y = sin(pitch), fwd_z = -cos(yaw)*cos(pitch)
PI = math.pi

# Standing avatar aim point (head ~1.55, belly ~1.25, hands ~0.9 in inspect pose).
LOOK_BODY = (0.0, 1.20, 0.0)
LOOK_FACE = (0.0, 1.52, -0.05)
LOOK_HANDS = (0.0, 0.95, -0.42)
LOOK_HAND_L = (-0.42, 0.95, -0.42)
LOOK_HAND_R = (0.42, 0.95, -0.42)


def mat_translation(x: float, y: float, z: float) -> list[float]:
	"""Column-major translation matrix."""
	return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1]


def look_yaw_pitch(cx: float, cy: float, cz: float, tx: float, ty: float, tz: float) -> tuple[float, float]:
	"""Camera yaw/pitch so the view looks from (cx,cy,cz) toward (tx,ty,tz)."""
	dx = tx - cx
	dy = ty - cy
	dz = tz - cz
	horiz = math.sqrt(dx * dx + dz * dz)
	# Match OpenXrContext free-cam: yaw from atan2(-fwd_x, -fwd_z).
	yaw = math.atan2(-dx, -dz) if horiz > 1.0e-6 else 0.0
	pitch = math.atan2(dy, horiz) if (horiz + abs(dy)) > 1.0e-6 else 0.0
	return yaw, pitch


def shot(name: str, x: float, y: float, z: float, target: tuple[float, float, float]) -> tuple[str, float, float, float, float, float]:
	"""Build a named camera shot that looks at a world target."""
	yaw, pitch = look_yaw_pitch(x, y, z, target[0], target[1], target[2])
	return (name, x, y, z, yaw, pitch)


# name, x, y, z, yaw, pitch — all aim at the standing avatar.
SHOTS: list[tuple[str, float, float, float, float, float]] = [
	shot("01_front", 0.0, 1.35, -2.60, LOOK_BODY),
	shot("02_front_close", 0.0, 1.40, -1.40, LOOK_BODY),
	shot("03_face_close", 0.0, 1.55, -0.90, LOOK_FACE),
	shot("04_three_quarter_l", -1.50, 1.40, -1.70, LOOK_BODY),
	shot("05_three_quarter_r", 1.50, 1.40, -1.70, LOOK_BODY),
	shot("06_side_l", -2.30, 1.35, 0.0, LOOK_BODY),
	shot("07_side_r", 2.30, 1.35, 0.0, LOOK_BODY),
	shot("08_back", 0.0, 1.35, 2.60, LOOK_BODY),
	shot("09_hands_front", 0.0, 0.95, -1.35, LOOK_HANDS),
	shot("10_hands_close_l", -0.70, 1.05, -0.95, LOOK_HAND_L),
	shot("11_hands_close_r", 0.70, 1.05, -0.95, LOOK_HAND_R),
	shot("12_top_down", 0.0, 3.20, -0.40, LOOK_BODY),
	shot("13_low_angle", 0.0, 0.35, -2.20, LOOK_BODY),
]


def render_settled(engine: hypervr.Engine, room, left_arm, right_arm, frames: int = 4) -> None:
	"""Draw a few frames so the mirror readback is fresh for the locked camera."""
	for _ in range(frames):
		engine.poll_events()
		engine.begin_frame()
		engine.draw_room(room)
		# No foreground cube — it was blocking hands in review shots.
		engine.draw_hand(left_arm, False)
		engine.draw_hand(right_arm, False)
		engine.end_frame()


def main() -> int:
	out_root = _ROOT / "artifacts" / "model_review"
	stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
	out_dir = out_root / stamp
	out_dir.mkdir(parents=True, exist_ok=True)

	print("model_review: starting desktop engine...")
	engine = hypervr.Engine("HyperVR-ModelReview", desktop_mode=True)
	print("backend:", engine.backend_name())
	if not engine.desktop_mirror_active():
		print("model_review: mirror window failed to open", file=sys.stderr)
		return 1

	room = engine.create_room(12.0, 12.0, 7.0)
	left_arm = engine.create_hand(hypervr.HAND_LEFT)
	right_arm = engine.create_hand(hypervr.HAND_RIGHT)

	# Warm up one frame before locking cameras.
	render_settled(engine, room, left_arm, right_arm, frames=2)

	manifest = out_dir / "manifest.txt"
	lines: list[str] = []
	ok_count = 0
	for name, x, y, z, yaw, pitch in SHOTS:
		engine.set_desktop_camera(x, y, z, yaw, pitch)
		render_settled(engine, room, left_arm, right_arm, frames=3)
		path = out_dir / f"{name}.bmp"
		ok = engine.save_desktop_screenshot(str(path))
		status = "ok" if ok else "FAIL"
		if ok:
			ok_count += 1
		line = f"{status}\t{path.name}\tx={x:.2f} y={y:.2f} z={z:.2f} yaw={yaw:.3f} pitch={pitch:.3f}"
		lines.append(line)
		print(line)
		time.sleep(0.02)

	manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
	# Also write a stable "latest" pointer directory via a text file.
	(out_root / "LATEST.txt").write_text(str(out_dir) + "\n", encoding="utf-8")
	print(f"model_review: saved {ok_count}/{len(SHOTS)} shots -> {out_dir}")
	return 0 if ok_count == len(SHOTS) else 2


if __name__ == "__main__":
	raise SystemExit(main())
