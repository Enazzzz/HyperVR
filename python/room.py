"""HyperVR room geometry for the NULLSIGHT-style acoustics pipeline."""

from __future__ import annotations

from dataclasses import dataclass, field

from math3d import Vec3
from occlusion import Aabb, BoundaryPlane


@dataclass(frozen=True)
class RoomData:
	"""Acoustic room payload (subset of NULLSIGHT RoomData used by Acoustics)."""

	# Axis-aligned floor footprint: (x_min, x_max, z_min, z_max).
	bounds: tuple[float, float, float, float]
	occluders: list[Aabb]
	collidables: list[Aabb]
	boundaries: list[BoundaryPlane]
	spawn: Vec3
	name: str
	# Unused by HyperVR rendering; kept for AcousticScene API compatibility.
	environment_edges: list = field(default_factory=list)
	occluder_hulls: list = field(default_factory=list)
	occluder_materials: list[str] = field(default_factory=list)
	shell_material: str = "concrete"
	floor_y: float = 0.0
	ceiling_y: float = 4.0
	walkables: list[Aabb] = field(default_factory=list)
	authored_sounds: list[dict] = field(default_factory=list)
	audio_profile: dict = field(default_factory=dict)
	entities: list[dict] = field(default_factory=list)
	connections: list[str] = field(default_factory=list)


def box_room(
	width: float,
	depth: float,
	height: float,
	*,
	name: str = "hypervr",
	shell_material: str = "concrete",
	extra_occluders: list[Aabb] | None = None,
	extra_materials: list[str] | None = None,
) -> RoomData:
	"""Build an empty axis-aligned room matching HyperVR ``create_room`` bounds.

	Origin is floor-center; walls at ±width/2 and ±depth/2; ceiling at ``height``.
	Coordinates are in **Spherion / NULLSIGHT** space (+Z forward).
	"""
	half_w = width * 0.5
	half_d = depth * 0.5
	boundaries = [
		BoundaryPlane(0, -half_w),
		BoundaryPlane(0, half_w),
		BoundaryPlane(1, 0.0),
		BoundaryPlane(1, height),
		BoundaryPlane(2, -half_d),
		BoundaryPlane(2, half_d),
	]
	occluders = list(extra_occluders or [])
	materials = list(extra_materials or [])
	while len(materials) < len(occluders):
		materials.append("wood")
	return RoomData(
		bounds=(-half_w, half_w, -half_d, half_d),
		occluders=occluders,
		collidables=list(occluders),
		boundaries=boundaries,
		spawn=(0.0, 0.0, 0.0),
		name=name,
		occluder_materials=materials,
		shell_material=shell_material,
		floor_y=0.0,
		ceiling_y=height,
	)


@dataclass
class ListenerPose:
	"""Duck-typed camera stand-in for ``AcousticSystem.snapshot`` / Spherion sync.

	Angles are **radians** in Spherion / NULLSIGHT space (yaw=0 looks +Z).
	``roll`` is optional; Acoustics ignores it, Spherion uses it when synced.
	"""

	eye: Vec3
	yaw: float = 0.0
	pitch: float = 0.0
	roll: float = 0.0

	def eye_position(self) -> Vec3:
		"""Return listener eye position in acoustics world space."""
		return self.eye
