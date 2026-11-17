"""
Convert a binary STL mesh to a welded OBJ for HyperVR assets.

By default remaps Z-up STL coordinates to OpenXR Y-up: (x, y, z) → (x, z, -y).

Usage:
  python scripts/stl_to_obj.py assets/bluebvvf.stl assets/player.obj
  python scripts/stl_to_obj.py assets/bluebvvf.stl assets/player.obj --keep-z-up
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path


def stl_to_obj(stl_path: Path, obj_path: Path, *, y_up: bool = True) -> None:
	"""Read a binary STL and write an indexed Wavefront OBJ."""
	data = stl_path.read_bytes()
	if len(data) < 84:
		raise ValueError(f"STL too small: {stl_path}")

	tri_count = struct.unpack_from("<I", data, 80)[0]
	verts: list[tuple[float, float, float]] = []
	indices: list[int] = []
	lookup: dict[tuple[float, float, float], int] = {}

	def add_vert(x: float, y: float, z: float) -> int:
		# Optional Z-up → Y-up bake for OpenXR / HyperVR.
		if y_up:
			x, y, z = x, z, -y
		key = (round(x, 5), round(y, 5), round(z, 5))
		found = lookup.get(key)
		if found is not None:
			return found
		idx = len(verts)
		lookup[key] = idx
		verts.append((x, y, z))
		return idx

	for i in range(tri_count):
		base = 84 + i * 50 + 12  # skip normal
		face: list[int] = []
		for v in range(3):
			x, y, z = struct.unpack_from("<fff", data, base + v * 12)
			face.append(add_vert(x, y, z))
		indices.extend(face)

	obj_path.parent.mkdir(parents=True, exist_ok=True)
	with obj_path.open("w", encoding="utf-8") as out:
		out.write(f"# Converted from {stl_path.name}\n")
		if y_up:
			out.write("# Axes: Z-up STL remapped to Y-up (x, y, z) -> (x, z, -y)\n")
		out.write(f"# {len(verts)} verts, {len(indices) // 3} tris\n")
		out.write("o player\n")
		for x, y, z in verts:
			out.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
		for i in range(0, len(indices), 3):
			out.write(f"f {indices[i] + 1} {indices[i + 1] + 1} {indices[i + 2] + 1}\n")

	print(f"Wrote {obj_path} ({len(verts)} verts, {len(indices) // 3} faces)")


def main() -> None:
	args = [a for a in sys.argv[1:] if a != "--keep-z-up"]
	y_up = "--keep-z-up" not in sys.argv[1:]
	if len(args) != 2:
		print(__doc__.strip())
		sys.exit(1)
	stl_to_obj(Path(args[0]), Path(args[1]), y_up=y_up)


if __name__ == "__main__":
	main()
