#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hypervr/types.hpp"

namespace hypervr {

/**
 * Load a triangle mesh from a Wavefront OBJ (positions + faces only).
 *
 * Supports `v` and triangle `f` (with optional /vt/vn). Applies a flat vertex color.
 * Throws std::runtime_error on I/O or parse failure.
 */
void load_obj_file(
	const std::string& path,
	std::vector<Vertex>& out_vertices,
	std::vector<std::uint32_t>& out_indices,
	float r = 0.25f,
	float g = 0.25f,
	float b = 0.28f);

}  // namespace hypervr
