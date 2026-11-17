"""
Install HyperVR by building with CMake (same tree as xr_demo) and copying hypervr.pyd.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext


class CMakeExtension(Extension):
	"""Placeholder extension produced by CMake."""

	def __init__(self, name: str) -> None:
		super().__init__(name, sources=[])


class CMakeBuild(build_ext):
	"""Build hypervr_python into the repo `build/` folder, then copy the module."""

	def build_extension(self, ext: Extension) -> None:
		extdir = pathlib.Path(self.get_ext_fullpath(ext.name)).parent.resolve()
		extdir.mkdir(parents=True, exist_ok=True)

		source_dir = pathlib.Path(__file__).parent.resolve()
		# Reuse the primary CMake build tree so OpenXR/Vulkan deps are not re-fetched blindly.
		cmake_build = source_dir / "build"
		cfg = "Debug" if self.debug else "Release"

		cmake_args = [
			f"-DPython3_EXECUTABLE={sys.executable}",
			"-DHYPERVR_BUILD_DEMO=ON",
			"-DHYPERVR_BUILD_PYTHON_BINDINGS=ON",
		]
		if "VULKAN_SDK" in os.environ:
			cmake_args.append(f"-DCMAKE_PREFIX_PATH={os.environ['VULKAN_SDK']}")

		subprocess.check_call(["cmake", "-S", str(source_dir), "-B", str(cmake_build), *cmake_args])
		subprocess.check_call(
			["cmake", "--build", str(cmake_build), "--config", cfg, "--target", "hypervr_python"]
		)

		candidates = list(cmake_build.rglob("hypervr.pyd")) + list(cmake_build.rglob("hypervr*.so"))
		if not candidates:
			raise RuntimeError("CMake did not produce hypervr.pyd / hypervr.so")
		# Prefer Release output when multiple configs exist.
		candidates.sort(key=lambda p: ("Release" not in str(p), len(str(p))))
		shutil.copy2(candidates[0], extdir / candidates[0].name)


setup(
	name="hypervr",
	version="0.1.0",
	description="Immediate-mode OpenXR/Vulkan VR renderer for Python",
	ext_modules=[CMakeExtension("hypervr")],
	cmdclass={"build_ext": CMakeBuild},
	zip_safe=False,
)
