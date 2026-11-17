"""Camera stand-in matching NULLSIGHT's acoustics.system import.

``AcousticSystem.snapshot`` type-hints ``camera.Camera`` and calls
``eye_position()``, ``.yaw``, and ``.pitch``. HyperVR supplies the same
surface from HMD pose via ``ListenerPose``.
"""

from __future__ import annotations

from room import ListenerPose

# Alias so ``from camera import Camera`` matches NULLSIGHT system.py 1:1.
Camera = ListenerPose

__all__ = ["Camera", "ListenerPose"]
