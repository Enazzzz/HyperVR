#include "hypervr/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "hypervr/mesh_loader.hpp"
#include "hypervr/desktop_mirror.hpp"
#include "hypervr/openxr_context.hpp"
#include "hypervr/vulkan_renderer.hpp"

namespace hypervr {
namespace {

/**
 * Resolve the directory that contains compiled SPIR-V shaders.
 */
std::string find_shader_directory() {
	namespace fs = std::filesystem;
	const std::vector<fs::path> candidates = {
		fs::current_path() / "shaders",
		fs::current_path() / "engine" / "shaders",
		fs::path(HYPERVR_SHADER_DIR),
	};
	for (const fs::path& path : candidates) {
		if (fs::exists(path / "mesh.vert.spv") && fs::exists(path / "mesh.frag.spv")) {
			return path.string();
		}
	}
	return std::string(HYPERVR_SHADER_DIR);
}

/**
 * Build a unit cube with per-vertex colors.
 */
void make_cube_geometry(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices) {
	const float s = 0.5f;
	const Vertex corners[8] = {
		{-s, -s, -s, 1, 0, 0},
		{s, -s, -s, 0, 1, 0},
		{s, s, -s, 0, 0, 1},
		{-s, s, -s, 1, 1, 0},
		{-s, -s, s, 1, 0, 1},
		{s, -s, s, 0, 1, 1},
		{s, s, s, 1, 1, 1},
		{-s, s, s, 0.5f, 0.5f, 1},
	};
	const int faces[6][4] = {
		{0, 1, 2, 3},
		{4, 7, 6, 5},
		{0, 4, 5, 1},
		{2, 6, 7, 3},
		{0, 3, 7, 4},
		{1, 5, 6, 2},
	};
	vertices.clear();
	indices.clear();
	for (const auto& face : faces) {
		const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
		for (int i = 0; i < 4; ++i) {
			vertices.push_back(corners[face[i]]);
		}
		indices.push_back(base + 0);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base + 0);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
	}
}

/**
 * Append an axis-aligned box (center + half-extents) with flat color.
 *
 * Uses clockwise winding so it matches the Vulkan FOV Y-flip front face.
 */
void append_box(
	std::vector<Vertex>& vertices,
	std::vector<std::uint32_t>& indices,
	float cx,
	float cy,
	float cz,
	float hx,
	float hy,
	float hz,
	float r,
	float g,
	float b) {
	const Vertex corners[8] = {
		{cx - hx, cy - hy, cz - hz, r, g, b},
		{cx + hx, cy - hy, cz - hz, r, g, b},
		{cx + hx, cy + hy, cz - hz, r, g, b},
		{cx - hx, cy + hy, cz - hz, r, g, b},
		{cx - hx, cy - hy, cz + hz, r, g, b},
		{cx + hx, cy - hy, cz + hz, r, g, b},
		{cx + hx, cy + hy, cz + hz, r, g, b},
		{cx - hx, cy + hy, cz + hz, r, g, b},
	};
	const int faces[6][4] = {
		{0, 3, 2, 1},
		{4, 5, 6, 7},
		{0, 1, 5, 4},
		{3, 7, 6, 2},
		{0, 4, 7, 3},
		{1, 2, 6, 5},
	};
	for (const auto& face : faces) {
		const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
		for (int i = 0; i < 4; ++i) {
			vertices.push_back(corners[face[i]]);
		}
		indices.push_back(base + 0);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base + 0);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
	}
}

/** Simple float3 for IK / limb transforms. */
struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

Vec3 operator+(Vec3 a, Vec3 b) {
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(Vec3 a, Vec3 b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(Vec3 a, float s) {
	return {a.x * s, a.y * s, a.z * s};
}

float vec_length(Vec3 v) {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3 vec_normalize(Vec3 v) {
	const float len = vec_length(v);
	if (len < 1.0e-6f) {
		return {0.0f, 1.0f, 0.0f};
	}
	return v * (1.0f / len);
}

Vec3 vec_cross(Vec3 a, Vec3 b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float vec_dot(Vec3 a, Vec3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 transform_point(const Mat4& m, Vec3 p) {
	return {
		m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
		m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
		m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14],
	};
}

Vec3 transform_dir(const Mat4& m, Vec3 d) {
	return {
		m.m[0] * d.x + m.m[4] * d.y + m.m[8] * d.z,
		m.m[1] * d.x + m.m[5] * d.y + m.m[9] * d.z,
		m.m[2] * d.x + m.m[6] * d.y + m.m[10] * d.z,
	};
}

Vec3 mat_origin(const Mat4& m) {
	return {m.m[12], m.m[13], m.m[14]};
}

/**
 * Tapered low-poly limb along −Z (knuckle at origin, tip at −length).
 *
 * Elliptical cross-section (wider than tall) reads more like an arm than a pipe.
 * Extra mid-ring keeps bends from looking like hollow tubes. Winding is clockwise
 * for the Vulkan FOV Y-flip front-face mode.
 */
void make_tapered_limb(
	std::vector<Vertex>& vertices,
	std::vector<std::uint32_t>& indices,
	float length,
	float rx0,
	float ry0,
	float rx1,
	float ry1,
	int sides,
	float r,
	float g,
	float b) {
	vertices.clear();
	indices.clear();
	if (sides < 4) {
		sides = 4;
	}

	// Three rings: base, mid (slight belly), tip — seals silhouette holes at bends.
	const float rxm = (rx0 + rx1) * 0.55f;
	const float rym = (ry0 + ry1) * 0.55f;
	auto push_ring = [&](float rx, float ry, float z) {
		for (int i = 0; i < sides; ++i) {
			const float a = (6.2831853f * static_cast<float>(i)) / static_cast<float>(sides);
			vertices.push_back({rx * std::cos(a), ry * std::sin(a), z, r, g, b});
		}
	};
	push_ring(rx0, ry0, 0.0f);
	push_ring(rxm, rym, -length * 0.5f);
	push_ring(rx1, ry1, -length);

	const std::uint32_t ring0 = 0;
	const std::uint32_t ring1 = static_cast<std::uint32_t>(sides);
	const std::uint32_t ring2 = static_cast<std::uint32_t>(sides * 2);
	const std::uint32_t cap0 = static_cast<std::uint32_t>(vertices.size());
	vertices.push_back({0.0f, 0.0f, 0.0f, r, g, b});
	const std::uint32_t cap1 = static_cast<std::uint32_t>(vertices.size());
	vertices.push_back({0.0f, 0.0f, -length, r, g, b});

	auto stitch = [&](std::uint32_t a0, std::uint32_t b0) {
		for (int i = 0; i < sides; ++i) {
			const std::uint32_t i0 = a0 + static_cast<std::uint32_t>(i);
			const std::uint32_t i1 = a0 + static_cast<std::uint32_t>((i + 1) % sides);
			const std::uint32_t j0 = b0 + static_cast<std::uint32_t>(i);
			const std::uint32_t j1 = b0 + static_cast<std::uint32_t>((i + 1) % sides);
			indices.push_back(i0);
			indices.push_back(j1);
			indices.push_back(j0);
			indices.push_back(i0);
			indices.push_back(i1);
			indices.push_back(j1);
		}
	};
	stitch(ring0, ring1);
	stitch(ring1, ring2);

	for (int i = 0; i < sides; ++i) {
		const std::uint32_t i0 = ring0 + static_cast<std::uint32_t>(i);
		const std::uint32_t i1 = ring0 + static_cast<std::uint32_t>((i + 1) % sides);
		const std::uint32_t j0 = ring2 + static_cast<std::uint32_t>(i);
		const std::uint32_t j1 = ring2 + static_cast<std::uint32_t>((i + 1) % sides);
		// Start cap (outward +Z).
		indices.push_back(cap0);
		indices.push_back(i0);
		indices.push_back(i1);
		// Tip cap (outward −Z).
		indices.push_back(cap1);
		indices.push_back(j1);
		indices.push_back(j0);
	}
}

/**
 * UV sphere used as joint plugs so angled bones do not leave V-gaps / holes.
 */
void make_joint_sphere(
	std::vector<Vertex>& vertices,
	std::vector<std::uint32_t>& indices,
	float radius,
	int slices,
	int stacks,
	float r,
	float g,
	float b) {
	vertices.clear();
	indices.clear();
	if (slices < 6) {
		slices = 6;
	}
	if (stacks < 4) {
		stacks = 4;
	}

	for (int v = 0; v <= stacks; ++v) {
		const float phi = 3.14159265f * static_cast<float>(v) / static_cast<float>(stacks);
		const float y = radius * std::cos(phi);
		const float ring_r = radius * std::sin(phi);
		for (int u = 0; u <= slices; ++u) {
			const float theta = 6.2831853f * static_cast<float>(u) / static_cast<float>(slices);
			vertices.push_back({ring_r * std::cos(theta), y, ring_r * std::sin(theta), r, g, b});
		}
	}

	const int stride = slices + 1;
	for (int v = 0; v < stacks; ++v) {
		for (int u = 0; u < slices; ++u) {
			const std::uint32_t i0 = static_cast<std::uint32_t>(v * stride + u);
			const std::uint32_t i1 = static_cast<std::uint32_t>(v * stride + u + 1);
			const std::uint32_t i2 = static_cast<std::uint32_t>((v + 1) * stride + u);
			const std::uint32_t i3 = static_cast<std::uint32_t>((v + 1) * stride + u + 1);
			// Clockwise for Vulkan Y-flip cull.
			indices.push_back(i0);
			indices.push_back(i2);
			indices.push_back(i1);
			indices.push_back(i1);
			indices.push_back(i2);
			indices.push_back(i3);
		}
	}
}

/**
 * Gapless GT pale mitten — few solid boxes, deep penetration, staggered Y to avoid z-fight seams.
 *
 * Local: wrist +Z, fingers -Z, thumb -X. Intentional opening: V between finger stacks only.
 */
void make_gorilla_palm(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, float r, float g, float b) {
	vertices.clear();
	indices.clear();

	// One thick palm+wrist slab.
	append_box(vertices, indices, 0.0f, 0.0f, 0.005f, 0.068f, 0.044f, 0.095f, r, g, b);
	// Two finger prongs — centers far enough for a V, but roots buried deep in the palm slab.
	append_box(vertices, indices, -0.040f, 0.001f, -0.120f, 0.040f, 0.040f, 0.095f, r, g, b);
	append_box(vertices, indices, 0.040f, -0.001f, -0.120f, 0.040f, 0.040f, 0.095f, r, g, b);
	// Soft tips (slightly thinner Y so tops aren't coplanar with finger bodies).
	append_box(vertices, indices, -0.072f, 0.0f, -0.195f, 0.028f, 0.032f, 0.035f, r, g, b);
	append_box(vertices, indices, 0.072f, 0.0f, -0.195f, 0.028f, 0.032f, 0.035f, r, g, b);
	// Thumb rooted inside palm, extending out.
	append_box(vertices, indices, -0.070f, 0.002f, -0.030f, 0.040f, 0.036f, 0.055f, r, g, b);
	append_box(vertices, indices, -0.100f, 0.0f, -0.075f, 0.028f, 0.030f, 0.040f, r, g, b);
}

/**
 * Gapless GT closed fist — compact overlapping boxes, staggered Y.
 */
void make_gorilla_fist(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, float r, float g, float b) {
	vertices.clear();
	indices.clear();

	append_box(vertices, indices, 0.0f, 0.0f, -0.010f, 0.065f, 0.048f, 0.090f, r, g, b);
	append_box(vertices, indices, -0.028f, -0.004f, -0.090f, 0.042f, 0.040f, 0.055f, r, g, b);
	append_box(vertices, indices, 0.028f, 0.004f, -0.090f, 0.042f, 0.040f, 0.055f, r, g, b);
	append_box(vertices, indices, -0.065f, 0.0f, -0.035f, 0.038f, 0.036f, 0.050f, r, g, b);
}

/**
 * Unused standalone digit (kept so call sites compile); prefer the solid mitten.
 */
void make_paw_digit(
	std::vector<Vertex>& vertices,
	std::vector<std::uint32_t>& indices,
	float length,
	float half_w0,
	float half_h0,
	float half_w1,
	float half_h1,
	float r,
	float g,
	float b) {
	make_tapered_limb(vertices, indices, length, half_w0, half_h0, half_w1, half_h1, 6, r, g, b);
}

/**
 * Unused standalone thumb (mitten bakes the thumb in).
 */
void make_paw_thumb(
	std::vector<Vertex>& vertices,
	std::vector<std::uint32_t>& indices,
	float length,
	float r,
	float g,
	float b) {
	make_paw_digit(vertices, indices, length, 0.016f, 0.014f, 0.012f, 0.011f, r, g, b);
}

/**
 * Append a filled ellipse in the XY plane (faces −Z) with flat color.
 */
void append_disk_xy(
	std::vector<Vertex>& vertices,
	std::vector<std::uint32_t>& indices,
	float cx,
	float cy,
	float cz,
	float rx,
	float ry,
	int sides,
	float r,
	float g,
	float b) {
	if (sides < 6) {
		sides = 6;
	}
	const std::uint32_t center = static_cast<std::uint32_t>(vertices.size());
	vertices.push_back({cx, cy, cz, r, g, b});
	for (int i = 0; i < sides; ++i) {
		const float a = (6.2831853f * static_cast<float>(i)) / static_cast<float>(sides);
		vertices.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a), cz, r, g, b});
	}
	for (int i = 0; i < sides; ++i) {
		indices.push_back(center);
		indices.push_back(center + static_cast<std::uint32_t>(1 + ((i + 1) % sides)));
		indices.push_back(center + static_cast<std::uint32_t>(1 + i));
	}
}

/**
 * Thick legless GT torso: short barrel that hangs under the head (no legs).
 */
void make_avatar_torso(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, float r, float g, float b) {
	// Wider, shorter sausage — GT silhouette is a stump, not a tall capsule.
	make_tapered_limb(vertices, indices, 0.38f, 0.175f, 0.145f, 0.130f, 0.105f, 8, r, g, b);
}

/**
 * GT face mask: pale rounded plate on the front (−Z). Eyes drawn as spheres in draw_hand.
 */
void make_avatar_face(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, float r, float g, float b) {
	// Pale oval mask as a flattened sphere — boxes were getting backface-culled into a wire outline.
	vertices.clear();
	indices.clear();
	make_joint_sphere(vertices, indices, 1.0f, 14, 12, r, g, b);
	for (Vertex& v : vertices) {
		v.px *= 0.072f;
		v.py *= 0.085f;
		v.pz *= 0.022f;
	}
	// Bake nostrils + mouth as dark dents on the front (−Z).
	auto bump = [&](float x, float y, float z, float rx, float ry, float rz, float cr, float cg, float cb) {
		const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
		std::vector<Vertex> tmp_v;
		std::vector<std::uint32_t> tmp_i;
		make_joint_sphere(tmp_v, tmp_i, 1.0f, 8, 6, cr, cg, cb);
		for (Vertex& v : tmp_v) {
			v.px = v.px * rx + x;
			v.py = v.py * ry + y;
			v.pz = v.pz * rz + z;
			vertices.push_back(v);
		}
		for (std::uint32_t idx : tmp_i) {
			indices.push_back(base + idx);
		}
	};
	bump(-0.014f, -0.018f, -0.020f, 0.008f, 0.006f, 0.004f, 0.08f, 0.07f, 0.08f);
	bump(0.014f, -0.018f, -0.020f, 0.008f, 0.006f, 0.004f, 0.08f, 0.07f, 0.08f);
	bump(0.0f, -0.042f, -0.018f, 0.030f, 0.006f, 0.004f, 0.06f, 0.05f, 0.06f);
}

/**
 * GT ear — small rounded blob (drawn as a sphere; this mesh kept for tinting).
 */
void make_avatar_ear(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, float r, float g, float b) {
	make_joint_sphere(vertices, indices, 1.0f, 8, 6, r, g, b);
}

/**
 * Build a matrix that places a −Z limb mesh from point A to point B.
 *
 * `mesh_length` is the authored bone length; Z is scaled to the actual span.
 */
Mat4 bone_matrix(Vec3 a, Vec3 b, Vec3 up_hint, float mesh_length) {
	const Vec3 delta = b - a;
	const float span = vec_length(delta);
	if (span < 1.0e-5f || mesh_length < 1.0e-5f) {
		return Mat4::translation(a.x, a.y, a.z);
	}

	// Local +Z points from tip toward knuckle so authored −Z runs A→B.
	const Vec3 z_axis = vec_normalize(a - b);
	Vec3 x_axis = vec_cross(up_hint, z_axis);
	if (vec_length(x_axis) < 1.0e-4f) {
		x_axis = vec_cross(Vec3{0.0f, 0.0f, 1.0f}, z_axis);
	}
	if (vec_length(x_axis) < 1.0e-4f) {
		x_axis = Vec3{1.0f, 0.0f, 0.0f};
	}
	x_axis = vec_normalize(x_axis);
	const Vec3 y_axis = vec_cross(z_axis, x_axis);
	const float sz = span / mesh_length;

	Mat4 out = Mat4::identity();
	out.m[0] = x_axis.x;
	out.m[1] = x_axis.y;
	out.m[2] = x_axis.z;
	out.m[4] = y_axis.x;
	out.m[5] = y_axis.y;
	out.m[6] = y_axis.z;
	out.m[8] = z_axis.x * sz;
	out.m[9] = z_axis.y * sz;
	out.m[10] = z_axis.z * sz;
	out.m[12] = a.x;
	out.m[13] = a.y;
	out.m[14] = a.z;
	return out;
}

/**
 * Analytic two-bone IK: shoulder → elbow → wrist with a pole hint.
 */
Vec3 solve_elbow(Vec3 shoulder, Vec3 wrist, float upper_len, float lower_len, Vec3 pole) {
	Vec3 to_wrist = wrist - shoulder;
	float dist = vec_length(to_wrist);
	const float max_reach = upper_len + lower_len - 0.002f;
	const float min_reach = std::fabs(upper_len - lower_len) + 0.002f;
	if (dist > max_reach) {
		to_wrist = to_wrist * (max_reach / dist);
		dist = max_reach;
		wrist = shoulder + to_wrist;
	} else if (dist < min_reach) {
		to_wrist = vec_normalize(to_wrist) * min_reach;
		dist = min_reach;
		wrist = shoulder + to_wrist;
	}

	const Vec3 forward = vec_normalize(to_wrist);
	// Distance from shoulder to elbow projection along the shoulder→wrist axis.
	float along = (dist * dist + upper_len * upper_len - lower_len * lower_len) / (2.0f * dist);
	along = std::max(-upper_len, std::min(upper_len, along));
	float height_sq = upper_len * upper_len - along * along;
	if (height_sq < 0.0f) {
		height_sq = 0.0f;
	}
	const float height = std::sqrt(height_sq);

	Vec3 pole_dir = pole - shoulder;
	Vec3 bend = vec_cross(forward, vec_cross(pole_dir, forward));
	if (vec_length(bend) < 1.0e-4f) {
		bend = vec_cross(forward, Vec3{0.0f, 0.0f, 1.0f});
	}
	if (vec_length(bend) < 1.0e-4f) {
		bend = Vec3{1.0f, 0.0f, 0.0f};
	}
	bend = vec_normalize(bend);
	return shoulder + forward * along + bend * height;
}

/** Avatar limb / hand tint — dark fur body, pale paw (GT-style). */
constexpr float kAvatarR = 0.10f;
constexpr float kAvatarG = 0.09f;
constexpr float kAvatarB = 0.10f;
constexpr float kHandR = 0.62f;
constexpr float kHandG = 0.62f;
constexpr float kHandB = 0.64f;
constexpr float kFaceR = 0.90f;
constexpr float kFaceG = 0.88f;
constexpr float kFaceB = 0.86f;
constexpr float kBellyR = 0.48f;
constexpr float kBellyG = 0.46f;
constexpr float kBellyB = 0.44f;

/**
 * Gorilla Tag-style player locomotion against an axis-aligned room interior.
 *
 * Faithful port of Another Axiom GorillaLocomotion Player.cs stick model.
 */
struct LocomotionState {
	bool enabled = false;
	LocomotionSettings settings{};
	Vec3 position{0.0f, 0.0f, 0.0f};
	Vec3 velocity{0.0f, 0.0f, 0.0f};

	/** World-space stuck hand positions (GT lastLeft/RightHandPosition). */
	Vec3 last_hand_position[2]{};
	bool hand_init[2]{false, false};
	bool was_hand_touching[2]{false, false};

	/** GT lastHeadPosition. */
	Vec3 last_head_position{0.0f, 0.0f, 0.0f};
	bool head_init = false;

	/** Rolling body-velocity average (GT StoreVelocities). */
	static constexpr int kMaxVelocityHistory = 16;
	Vec3 velocity_history[kMaxVelocityHistory]{};
	int velocity_index = 0;
	Vec3 denormalized_velocity_average{0.0f, 0.0f, 0.0f};
	Vec3 last_position{0.0f, 0.0f, 0.0f};
	bool last_position_init = false;

	float room_half_w = 6.0f;
	float room_half_d = 6.0f;
	float room_height = 7.0f;
};

/**
 * Deepest sphere penetration against the room's inner AABB.
 */
bool room_palm_contact(
	Vec3 world_palm,
	float radius,
	float half_w,
	float half_d,
	float height,
	Vec3& out_normal,
	float& out_pen) {
	out_pen = 0.0f;
	out_normal = {0.0f, 1.0f, 0.0f};
	bool hit = false;

	auto consider = [&](float pen, Vec3 n) {
		if (pen > out_pen) {
			out_pen = pen;
			out_normal = n;
			hit = true;
		}
	};

	consider(radius - world_palm.y, Vec3{0.0f, 1.0f, 0.0f});
	consider(world_palm.y + radius - height, Vec3{0.0f, -1.0f, 0.0f});
	consider(radius - (world_palm.x + half_w), Vec3{1.0f, 0.0f, 0.0f});
	consider(world_palm.x + radius - half_w, Vec3{-1.0f, 0.0f, 0.0f});
	consider(radius - (world_palm.z + half_d), Vec3{0.0f, 0.0f, 1.0f});
	consider(world_palm.z + radius - half_d, Vec3{0.0f, 0.0f, -1.0f});
	return hit && out_pen > 0.0f;
}

/**
 * Push a sphere out of room interior surfaces.
 */
bool resolve_room_sphere(
	Vec3 pos,
	float radius,
	float half_w,
	float half_d,
	float height,
	Vec3& out_pos,
	Vec3& out_normal) {
	float pen = 0.0f;
	if (!room_palm_contact(pos, radius, half_w, half_d, height, out_normal, pen)) {
		out_pos = pos;
		return false;
	}
	out_pos = pos + out_normal * pen;
	return true;
}

/**
 * Project vector onto plane with the given normal.
 */
Vec3 project_on_plane(Vec3 v, Vec3 normal) {
	return v - normal * vec_dot(v, normal);
}

/**
 * Sphere-cast against the room interior AABB (GT CollisionsSphereCast stand-in).
 *
 * Unity casts against outside colliders; our room is an inside-out box, so this
 * sweeps the sphere center within the shrunk free AABB and reports the first hit.
 * Cast range is extended by radius*(1-precision) like GT's SphereCast max distance.
 *
 * Interior AABBs keep hand followers slightly embedded on contact, so an
 * overlapping start must depenetrate and count as a hit — otherwise hands sink
 * through the floor (Unity's "overlap => miss" does not map cleanly here).
 */
bool room_sphere_cast(
	Vec3 start,
	float radius,
	Vec3 movement,
	float precision,
	float half_w,
	float half_d,
	float height,
	Vec3& out_end,
	Vec3& out_normal) {
	// Already overlapping: snap out and report contact (keeps hands on surfaces).
	Vec3 dep{};
	Vec3 dep_n{};
	if (resolve_room_sphere(start, radius, half_w, half_d, height, dep, dep_n)) {
		out_end = dep;
		out_normal = dep_n;
		return true;
	}

	// GT: cast distance = movement.magnitude + sphereRadius * (1 - precision).
	Vec3 cast_move = movement;
	const float move_len = vec_length(movement);
	if (move_len > 1.0e-8f) {
		cast_move = movement + vec_normalize(movement) * (radius * (1.0f - precision));
	}

	const float x_min = -half_w + radius;
	const float x_max = half_w - radius;
	const float y_min = radius;
	const float y_max = height - radius;
	const float z_min = -half_d + radius;
	const float z_max = half_d - radius;

	float t_hit = 2.0f;
	Vec3 hit_n{0.0f, 1.0f, 0.0f};

	auto consider_plane = [&](float start_c, float mov_c, float bound, Vec3 n) {
		if (std::fabs(mov_c) < 1.0e-8f) {
			return;
		}
		const float t = (bound - start_c) / mov_c;
		if (t >= 0.0f && t <= 1.0f && t < t_hit) {
			t_hit = t;
			hit_n = n;
		}
	};

	consider_plane(start.x, cast_move.x, x_min, Vec3{1.0f, 0.0f, 0.0f});
	consider_plane(start.x, cast_move.x, x_max, Vec3{-1.0f, 0.0f, 0.0f});
	consider_plane(start.y, cast_move.y, y_min, Vec3{0.0f, 1.0f, 0.0f});
	consider_plane(start.y, cast_move.y, y_max, Vec3{0.0f, -1.0f, 0.0f});
	consider_plane(start.z, cast_move.z, z_min, Vec3{0.0f, 0.0f, 1.0f});
	consider_plane(start.z, cast_move.z, z_max, Vec3{0.0f, 0.0f, -1.0f});

	if (t_hit <= 1.0f) {
		out_end = start + cast_move * t_hit;
		out_normal = hit_n;
		out_end = out_end + hit_n * 1.0e-4f;
		return true;
	}

	if (resolve_room_sphere(start + cast_move, radius, half_w, half_d, height, out_end, out_normal)) {
		return true;
	}
	return false;
}

/**
 * GT IterativeCollisionSphereCast against the room (+ surface slip).
 */
bool iterative_room_sphere_cast(
	Vec3 start,
	float sphere_radius,
	Vec3 movement,
	float precision,
	bool single_hand,
	float default_slide_factor,
	float half_w,
	float half_d,
	float height,
	Vec3& out_end,
	Vec3& out_normal) {
	const float r = sphere_radius * precision;

	if (room_sphere_cast(start, r, movement, precision, half_w, half_d, height, out_end, out_normal)) {
		const Vec3 first_position = out_end;
		const float slip = single_hand ? 0.001f : default_slide_factor;
		const Vec3 along = project_on_plane(start + movement - first_position, out_normal) * slip;
		if (vec_length(along) > 1.0e-8f) {
			Vec3 slid_end{};
			Vec3 slid_n{};
			// GT second cast uses full sphereRadius with precision^2.
			if (room_sphere_cast(
					first_position,
					sphere_radius,
					along,
					precision * precision,
					half_w,
					half_d,
					height,
					slid_end,
					slid_n)) {
				out_end = slid_end;
				out_normal = slid_n;
			} else {
				out_end = first_position + along;
				if (resolve_room_sphere(
						out_end, sphere_radius, half_w, half_d, height, slid_end, slid_n)) {
					out_end = slid_end;
					out_normal = slid_n;
				}
			}
		}
		return true;
	}

	// GT sanity: smaller sphere along an extended probe.
	const float small_r = r * 0.66f;
	const float move_len = vec_length(movement);
	Vec3 probe_move = movement;
	if (move_len > 1.0e-8f) {
		probe_move = vec_normalize(movement) * (move_len + sphere_radius * precision * 0.34f);
	}
	Vec3 probe_end{};
	Vec3 n{};
	if (room_sphere_cast(start, small_r, probe_move, precision * 0.66f, half_w, half_d, height, probe_end, n) ||
		resolve_room_sphere(start, small_r, half_w, half_d, height, probe_end, n)) {
		out_end = start;
		out_normal = n;
		return true;
	}
	return false;
}

/**
 * GT CurrentHandPosition: controller pose + local offset, clamped to maxArmLength.
 */
Vec3 tracking_hand_locomotion_point(const Mat4& grip) {
	// GT PositionWithOffset(handTransform, handOffset). Offsets are inspector-tuned;
	// zero matches the common default when offsets are left empty.
	return mat_origin(grip);
}

/**
 * Palm pose matching gorilla hand orientation in draw_hand.
 */
Mat4 tracking_palm_pose(Hand hand, const Mat4& grip) {
	constexpr float kDeg = 3.14159265f / 180.0f;
	constexpr float kHandPitchDown = -35.0f * kDeg;
	const bool left = hand == Hand::Left;
	const float hand_roll = left ? (90.0f * kDeg) : (-90.0f * kDeg);
	Mat4 palm_pose = Mat4::multiply(grip, Mat4::rotation_x(kHandPitchDown));
	return Mat4::multiply(palm_pose, Mat4::rotation_z(hand_roll));
}

/**
 * Palm center in tracking space.
 */
Vec3 tracking_palm_center(Hand hand, const Mat4& grip) {
	return transform_point(tracking_palm_pose(hand, grip), Vec3{0.0f, 0.0f, -0.036f});
}

/**
 * Clamp hand world position to max arm length from the head (GT maxArmLength).
 */
Vec3 clamp_hand_to_arm(Vec3 hand_world, Vec3 head_world, float max_arm) {
	const Vec3 delta = hand_world - head_world;
	const float len = vec_length(delta);
	if (len <= max_arm || len < 1.0e-6f) {
		return hand_world;
	}
	return head_world + delta * (max_arm / len);
}

/**
 * GT StoreVelocities — rolling denormalized body-velocity average.
 */
void store_locomotion_velocities(LocomotionState& loco, float dt) {
	const LocomotionSettings& s = loco.settings;
	const int hist = std::clamp(s.velocity_history_size, 1, LocomotionState::kMaxVelocityHistory);

	if (!loco.last_position_init) {
		loco.last_position = loco.position;
		loco.last_position_init = true;
		loco.velocity_index = 0;
		loco.denormalized_velocity_average = {0.0f, 0.0f, 0.0f};
		for (int i = 0; i < hist; ++i) {
			loco.velocity_history[i] = {0.0f, 0.0f, 0.0f};
		}
		return;
	}

	loco.velocity_index = (loco.velocity_index + 1) % hist;
	const Vec3 oldest = loco.velocity_history[loco.velocity_index];
	const Vec3 current = (loco.position - loco.last_position) * (1.0f / dt);
	loco.denormalized_velocity_average =
		loco.denormalized_velocity_average + (current - oldest) * (1.0f / static_cast<float>(hist));
	loco.velocity_history[loco.velocity_index] = current;
	loco.last_position = loco.position;
}

/**
 * Queue a draw in tracking/render space (no locomotion shift).
 */
void queue_draw_tracking(
	std::vector<DrawItem>& draws,
	MeshId mesh,
	const Mat4& model,
	float r,
	float g,
	float b,
	float a) {
	DrawItem item{};
	item.mesh = mesh;
	item.model = model;
	item.r = r;
	item.g = g;
	item.b = b;
	item.a = a;
	draws.push_back(item);
}

/**
 * Apply locomotion offset so world-space models stay fixed while the player moves.
 */
Mat4 world_to_tracking(const LocomotionState& loco, const Mat4& world_model) {
	if (!loco.enabled) {
		return world_model;
	}
	return Mat4::multiply(
		Mat4::translation(-loco.position.x, -loco.position.y, -loco.position.z),
		world_model);
}

/**
 * Translate a tracking-space pose into world space.
 */
void apply_player_offset(const LocomotionState& loco, Mat4& pose) {
	if (!loco.enabled) {
		return;
	}
	pose.m[12] += loco.position.x;
	pose.m[13] += loco.position.y;
	pose.m[14] += loco.position.z;
}

/**
 * Integrate GorillaLocomotion Player.cs against the room AABB.
 *
 * Same path for floor and walls — no custom still-hand / floor-friction layers.
 */
void tick_locomotion(LocomotionState& loco, OpenXrContext& xr, float dt) {
	if (!loco.enabled || dt <= 1.0e-5f || dt > 0.1f) {
		return;
	}

	const LocomotionSettings& s = loco.settings;
	const float radius = s.minimum_raycast_distance;
	const float precision = s.default_precision;

	Mat4 head_pose = Mat4::identity();
	const bool have_head = xr.head_pose(head_pose);
	const Vec3 head_tracking = have_head ? mat_origin(head_pose) : Vec3{0.0f, 0.0f, 0.0f};
	const Vec3 head_world = loco.position + head_tracking;
	if (!loco.head_init) {
		loco.last_head_position = head_world;
		loco.head_init = true;
	}

	bool have_hand[2]{false, false};
	bool colliding[2]{false, false};
	Vec3 current_hand[2]{};
	Vec3 first_iteration[2]{};
	Mat4 grips[2]{Mat4::identity(), Mat4::identity()};

	for (int i = 0; i < 2; ++i) {
		if (!xr.controller_pose(static_cast<Hand>(i), grips[i])) {
			loco.hand_init[i] = false;
			loco.was_hand_touching[i] = false;
			continue;
		}
		have_hand[i] = true;
		// GT: handTransform.position + rotation * handOffset (not the rendered palm mesh).
		current_hand[i] = clamp_hand_to_arm(
			loco.position + tracking_hand_locomotion_point(grips[i]),
			head_world,
			s.max_arm_length);
		if (!loco.hand_init[i]) {
			loco.last_hand_position[i] = current_hand[i];
			loco.hand_init[i] = true;
		}
	}

	// --- First pass (GT left/right hand with gravity bias) ---
	bool first_pass_hit[2]{false, false};
	Vec3 contact_normal[2]{{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
	const Vec3 gravity_bias{0.0f, -2.0f * 9.8f * dt * dt, 0.0f};
	for (int i = 0; i < 2; ++i) {
		if (!have_hand[i]) {
			continue;
		}

		const Vec3 distance_traveled = current_hand[i] - loco.last_hand_position[i] + gravity_bias;
		Vec3 final_position{};
		Vec3 hit_normal{};
		if (iterative_room_sphere_cast(
				loco.last_hand_position[i],
				radius,
				distance_traveled,
				precision,
				true,
				s.default_slide_factor,
				loco.room_half_w,
				loco.room_half_d,
				loco.room_height,
				final_position,
				hit_normal)) {
			if (loco.was_hand_touching[i]) {
				first_iteration[i] = loco.last_hand_position[i] - current_hand[i];
			} else {
				first_iteration[i] = final_position - current_hand[i];
			}
			/**
			 * PhysX stand-in for push-off aim: never stick the body into the
			 * surface. A straight shove (hands into the wall) keeps the away-
			 * from-wall component; a retract (hands leave along the normal)
			 * drops the into-wall junk and keeps any along-surface / forward
			 * motion — so retract launches more "forward" instead of fighting
			 * the shove direction in the velocity average.
			 */
			const float into_surface = vec_dot(first_iteration[i], hit_normal);
			if (into_surface < 0.0f) {
				first_iteration[i] = first_iteration[i] - hit_normal * into_surface;
			}
			loco.velocity = {0.0f, 0.0f, 0.0f};
			colliding[i] = true;
			first_pass_hit[i] = true;
			contact_normal[i] = hit_normal;
		}
	}

	Vec3 rigid_body_movement{0.0f, 0.0f, 0.0f};
	const bool left_active = colliding[0] || loco.was_hand_touching[0];
	const bool right_active = colliding[1] || loco.was_hand_touching[1];
	// Average only when both hands contributed stick this frame. GT also gates on
	// wasTouching, which halves a real shove when the other hand flickers off —
	// that was a common "launch fails half the time" path.
	if (first_pass_hit[0] && first_pass_hit[1]) {
		rigid_body_movement = (first_iteration[0] + first_iteration[1]) * 0.5f;
	} else {
		rigid_body_movement = first_iteration[0] + first_iteration[1];
	}

	// --- Head collision check (GT IterativeCollisionSphereCast, singleHand=false) ---
	const float head_radius = 0.12f;
	if (vec_length(rigid_body_movement) > 1.0e-8f) {
		Vec3 head_final{};
		Vec3 head_n{};
		const Vec3 head_travel = head_world + rigid_body_movement - loco.last_head_position;
		if (iterative_room_sphere_cast(
				loco.last_head_position,
				head_radius,
				head_travel,
				precision,
				false,
				s.default_slide_factor,
				loco.room_half_w,
				loco.room_half_d,
				loco.room_height,
				head_final,
				head_n)) {
			rigid_body_movement = head_final - loco.last_head_position;
			// GT anti-clip: Raycast(lastHead, head - lastHead + movement, ...).
			// Point-sweep stand-in — do not use a full head-sphere overlap test
			// (that falsely cancelled valid stick shoves).
			const Vec3 anti_dir =
				head_world - loco.last_head_position + rigid_body_movement;
			const float anti_len = vec_length(anti_dir);
			if (anti_len > 1.0e-8f) {
				Vec3 anti_end{};
				Vec3 anti_n{};
				const float anti_dist =
					anti_len + head_radius * precision * 0.999f;
				if (room_sphere_cast(
						loco.last_head_position,
						0.001f,
						vec_normalize(anti_dir) * anti_dist,
						1.0f,
						loco.room_half_w,
						loco.room_half_d,
						loco.room_height,
						anti_end,
						anti_n)) {
					rigid_body_movement = loco.last_head_position - head_world;
				}
			}
		}
	}

	if (vec_length(rigid_body_movement) > 1.0e-8f) {
		loco.position = loco.position + rigid_body_movement;
	}

	// GT: lastHeadPosition = headCollider after the body moved with the rig.
	loco.last_head_position = loco.position + head_tracking;

	// Refresh current hands after body move (GT controllers move with the rig).
	for (int i = 0; i < 2; ++i) {
		if (!have_hand[i]) {
			continue;
		}
		xr.controller_pose(static_cast<Hand>(i), grips[i]);
		current_hand[i] = clamp_hand_to_arm(
			loco.position + tracking_hand_locomotion_point(grips[i]),
			loco.last_head_position,
			s.max_arm_length);
	}

	// --- Second pass: final hand positions (GT lastHandPosition update) ---
	const bool both_hands = left_active && right_active;
	for (int i = 0; i < 2; ++i) {
		if (!have_hand[i]) {
			colliding[i] = false;
			continue;
		}

		const Vec3 distance_traveled = current_hand[i] - loco.last_hand_position[i];
		Vec3 final_position{};
		Vec3 hit_normal{};
		if (iterative_room_sphere_cast(
				loco.last_hand_position[i],
				radius,
				distance_traveled,
				precision,
				!both_hands,
				s.default_slide_factor,
				loco.room_half_w,
				loco.room_half_d,
				loco.room_height,
				final_position,
				hit_normal)) {
			loco.last_hand_position[i] = final_position;
			colliding[i] = true;
			contact_normal[i] = hit_normal;
		} else {
			// Never let the follower track through geometry (visual + next-cast start).
			Vec3 free_hand = current_hand[i];
			Vec3 pen_n{};
			if (resolve_room_sphere(
					current_hand[i],
					radius * precision,
					loco.room_half_w,
					loco.room_half_d,
					loco.room_height,
					free_hand,
					pen_n)) {
				loco.last_hand_position[i] = free_hand;
				colliding[i] = true;
				contact_normal[i] = pen_n;
			} else {
				loco.last_hand_position[i] = current_hand[i];
				colliding[i] = false;
			}
		}
	}

	// GT StoreVelocities — after stick move, before jump assignment.
	store_locomotion_velocities(loco, dt);

	/**
	 * Jump / launch (GT Player.cs). Sets Rigidbody.velocity while a hand is
	 * colliding; PhysX then integrates every step — including frames that are
	 * still "touching". Include first-pass hits so a leave frame that cleared
	 * second-pass contact still gets the shove that was just zeroed+stored.
	 *
	 * Blend this-frame stick velocity into the history average so push-off
	 * aims with the hand path you just made (GT history alone is a bit laggy
	 * without PhysX contact response shaping the integrate).
	 */
	if (first_pass_hit[0] || first_pass_hit[1] || colliding[0] || colliding[1]) {
		const Vec3 instant = rigid_body_movement * (1.0f / dt);
		// Favor the smoothed history so single-frame spikes don't hop the body.
		const Vec3 blended =
			loco.denormalized_velocity_average * 0.78f + instant * 0.22f;
		const float launch_speed = vec_length(blended);
		if (launch_speed > s.velocity_limit) {
			Vec3 launch = blended;
			if (launch_speed * s.jump_multiplier > s.max_jump_speed) {
				launch = vec_normalize(blended) * s.max_jump_speed;
			} else {
				launch = blended * s.jump_multiplier;
			}
			// Don't fire back into the surface we just pushed from.
			for (int i = 0; i < 2; ++i) {
				if (!first_pass_hit[i] && !colliding[i]) {
					continue;
				}
				const float into = vec_dot(launch, contact_normal[i]);
				if (into < 0.0f) {
					launch = launch - contact_normal[i] * into;
				}
			}
			loco.velocity = launch;
		}
	}

	// GT unStickDistance — after jump set, before wasTouching latch.
	for (int i = 0; i < 2; ++i) {
		if (!have_hand[i] || !colliding[i]) {
			continue;
		}
		if (vec_length(current_hand[i] - loco.last_hand_position[i]) > s.unstick_distance) {
			loco.last_hand_position[i] = current_hand[i];
			colliding[i] = false;
		}
	}

	for (int i = 0; i < 2; ++i) {
		loco.was_hand_touching[i] = have_hand[i] ? colliding[i] : false;
	}

	// Unity Rigidbody stand-in: gravity + integrate every frame (FixedUpdate).
	loco.velocity.y += s.gravity * dt;
	loco.position = loco.position + loco.velocity * dt;

	// Capsule-vs-floor stand-in (GT uses PhysX). Only block sinking — never
	// kill upward launch velocity while still near the floor plane.
	if (loco.position.y < 0.0f) {
		loco.position.y = 0.0f;
		if (loco.velocity.y < 0.0f) {
			loco.velocity.y = 0.0f;
		}
	}

	// Light body friction when sliding on the floor without hand contact.
	const bool hands_planted = loco.was_hand_touching[0] || loco.was_hand_touching[1];
	if (!hands_planted && loco.position.y <= 1.0e-4f && s.body_floor_friction > 0.0f) {
		const float damp = std::exp(-s.body_floor_friction * dt);
		loco.velocity.x *= damp;
		loco.velocity.z *= damp;
	}

	const float kEdge = 0.02f;
	loco.position.x =
		std::clamp(loco.position.x, -loco.room_half_w + kEdge, loco.room_half_w - kEdge);
	loco.position.z =
		std::clamp(loco.position.z, -loco.room_half_d + kEdge, loco.room_half_d - kEdge);
	// Match the visual ceiling (was height-0.5, which made a false upper boundary).
	if (loco.position.y > loco.room_height - kEdge) {
		loco.position.y = loco.room_height - kEdge;
		if (loco.velocity.y > 0.0f) {
			loco.velocity.y = 0.0f;
		}
	}
}

}  // namespace

struct Engine::Impl {
	VulkanRenderer renderer;
	OpenXrContext xr;
	DesktopMirror mirror;
	bool mirror_enabled = true;
	bool desktop_mode = false;
	std::vector<DrawItem> draws;
	FrameTimings last_timings{};
	std::chrono::steady_clock::time_point last_begin = std::chrono::steady_clock::now();
	float dt = 0.0f;
	float fps_smooth = 0.0f;
	bool frame_open = false;
	LocomotionState loco{};
	/** Shared GT-style body (drawn once from the right-hand pass). */
	MeshId avatar_torso = -1;
	MeshId avatar_belly = -1;
	MeshId avatar_head = -1;
	MeshId avatar_face = -1;
	MeshId avatar_ear = -1;
	bool avatar_built = false;
	bool avatar_visible = true;
	bool arms_visible = true;
};

Engine::Engine(std::string application_name, bool desktop_mode) {
	impl_ = new Impl();
	impl_->desktop_mode = desktop_mode;
	const std::string shader_dir = find_shader_directory();
	try {
		if (desktop_mode) {
			impl_->xr.initialize_desktop(application_name, impl_->renderer, shader_dir);
		} else {
			try {
				impl_->xr.initialize(application_name, impl_->renderer, shader_dir);
			} catch (const std::exception& xr_ex) {
				// Rebuild a clean Impl so a half-init OpenXR context cannot leak into desktop mode.
				std::cerr << "OpenXR unavailable (" << xr_ex.what() << "); falling back to desktop mode.\n";
				delete impl_;
				impl_ = new Impl();
				impl_->desktop_mode = true;
				impl_->xr.initialize_desktop(application_name, impl_->renderer, shader_dir);
			}
		}
		try {
			impl_->mirror.initialize(impl_->renderer, impl_->xr.color_format(), 960, 540);
		} catch (const std::exception& ex) {
			std::cerr << "desktop mirror unavailable: " << ex.what() << "\n";
			impl_->mirror_enabled = false;
			if (impl_->desktop_mode) {
				throw;  // Desktop mode has no headset path — mirror is required.
			}
		}
	} catch (...) {
		delete impl_;
		impl_ = nullptr;
		throw;
	}
}

Engine::~Engine() {
	if (impl_) {
		impl_->mirror.shutdown();
		impl_->renderer.shutdown();
	}
	delete impl_;
	impl_ = nullptr;
}

bool Engine::is_running() const {
	return impl_ && impl_->xr.is_running();
}

void Engine::poll_events() {
	impl_->xr.poll_events();
	if (impl_->mirror_enabled && impl_->mirror.alive()) {
		if (!impl_->mirror.poll()) {
			impl_->mirror_enabled = false;
			if (impl_->desktop_mode || impl_->xr.desktop_mode()) {
				impl_->xr.request_exit();
			}
		} else if (impl_->xr.desktop_mode()) {
			impl_->xr.desktop_add_scroll(impl_->mirror.take_scroll_y());
		}
	}
}

void Engine::begin_frame() {
	const auto now = std::chrono::steady_clock::now();
	impl_->dt = std::chrono::duration<float>(now - impl_->last_begin).count();
	impl_->last_begin = now;
	if (impl_->dt > 1.0e-4f && impl_->dt < 1.0f) {
		const float instant = 1.0f / impl_->dt;
		impl_->fps_smooth = (impl_->fps_smooth <= 1.0f) ? instant : (impl_->fps_smooth * 0.9f + instant * 0.1f);
	}
	clear_draws();
	impl_->frame_open = impl_->xr.begin_frame(impl_->last_timings);
	if (impl_->frame_open) {
		tick_locomotion(impl_->loco, impl_->xr, impl_->dt);
	}
}

void Engine::clear_draws() {
	impl_->draws.clear();
}

MeshId Engine::create_cube() {
	std::vector<Vertex> vertices;
	std::vector<std::uint32_t> indices;
	make_cube_geometry(vertices, indices);
	return load_mesh(vertices, indices);
}

RoomVisual Engine::create_room(float width, float depth, float height) {
	RoomVisual room{};
	room.width = width;
	room.depth = depth;
	room.height = height;

	const float half_w = width * 0.5f;
	const float half_d = depth * 0.5f;
	const float wall_t = 0.06f;
	const float slab_t = 0.04f;

	// Muted room colors (vertex-tinted; draw with white tint).
	constexpr float kFloorR = 0.18f;
	constexpr float kFloorG = 0.19f;
	constexpr float kFloorB = 0.22f;
	constexpr float kWallR = 0.32f;
	constexpr float kWallG = 0.34f;
	constexpr float kWallB = 0.38f;
	constexpr float kCeilR = 0.26f;
	constexpr float kCeilG = 0.27f;
	constexpr float kCeilB = 0.30f;

	std::vector<Vertex> vertices;
	std::vector<std::uint32_t> indices;

	// Floor: top face at y = 0.
	append_box(vertices, indices, 0.0f, -slab_t * 0.5f, 0.0f, half_w, slab_t * 0.5f, half_d, kFloorR, kFloorG, kFloorB);
	// Ceiling.
	append_box(
		vertices,
		indices,
		0.0f,
		height + slab_t * 0.5f,
		0.0f,
		half_w,
		slab_t * 0.5f,
		half_d,
		kCeilR,
		kCeilG,
		kCeilB);
	// Four walls (centers at mid-height).
	append_box(
		vertices,
		indices,
		0.0f,
		height * 0.5f,
		-(half_d + wall_t * 0.5f),
		half_w + wall_t,
		height * 0.5f,
		wall_t * 0.5f,
		kWallR,
		kWallG,
		kWallB);
	append_box(
		vertices,
		indices,
		0.0f,
		height * 0.5f,
		+(half_d + wall_t * 0.5f),
		half_w + wall_t,
		height * 0.5f,
		wall_t * 0.5f,
		kWallR * 0.92f,
		kWallG * 0.92f,
		kWallB * 0.92f);
	append_box(
		vertices,
		indices,
		-(half_w + wall_t * 0.5f),
		height * 0.5f,
		0.0f,
		wall_t * 0.5f,
		height * 0.5f,
		half_d,
		kWallR * 0.96f,
		kWallG * 0.96f,
		kWallB * 0.96f);
	append_box(
		vertices,
		indices,
		+(half_w + wall_t * 0.5f),
		height * 0.5f,
		0.0f,
		wall_t * 0.5f,
		height * 0.5f,
		half_d,
		kWallR * 0.88f,
		kWallG * 0.88f,
		kWallB * 0.88f);

	room.mesh = load_mesh(vertices, indices);
	enable_locomotion(room);
	return room;
}

void Engine::draw_room(const RoomVisual& room) {
	if (room.mesh < 0) {
		return;
	}
	draw_mesh(room.mesh, Mat4::identity(), 1.0f, 1.0f, 1.0f, 1.0f);
}

void Engine::enable_locomotion(const RoomVisual& room, LocomotionSettings settings) {
	impl_->loco.settings = settings;
	impl_->loco.room_half_w = room.width * 0.5f;
	impl_->loco.room_half_d = room.depth * 0.5f;
	impl_->loco.room_height = room.height;
	impl_->loco.position = {0.0f, 0.0f, 0.0f};
	impl_->loco.velocity = {0.0f, 0.0f, 0.0f};
	impl_->loco.hand_init[0] = false;
	impl_->loco.hand_init[1] = false;
	impl_->loco.was_hand_touching[0] = false;
	impl_->loco.was_hand_touching[1] = false;
	impl_->loco.head_init = false;
	impl_->loco.last_position_init = false;
	impl_->loco.denormalized_velocity_average = {0.0f, 0.0f, 0.0f};
	impl_->loco.velocity_index = 0;
	// Free-cam owns movement in desktop debug; GT palm locomotion would fight WASD.
	impl_->loco.enabled = !(impl_->desktop_mode || impl_->xr.desktop_mode());
}

void Engine::set_locomotion_enabled(bool enabled) {
	impl_->loco.enabled = enabled;
	if (!enabled) {
		impl_->loco.hand_init[0] = false;
		impl_->loco.hand_init[1] = false;
		impl_->loco.was_hand_touching[0] = false;
		impl_->loco.was_hand_touching[1] = false;
		impl_->loco.head_init = false;
		impl_->loco.last_position_init = false;
		impl_->loco.velocity = {0.0f, 0.0f, 0.0f};
	}
}

void Engine::player_position(float& x, float& y, float& z) const {
	x = impl_->loco.position.x;
	y = impl_->loco.position.y;
	z = impl_->loco.position.z;
}

void Engine::player_velocity(float& x, float& y, float& z) const {
	x = impl_->loco.velocity.x;
	y = impl_->loco.velocity.y;
	z = impl_->loco.velocity.z;
}

HandVisual Engine::create_hand(Hand hand) {
	HandVisual visual{};
	visual.side = hand;
	// GT arms are long AND thick — keep reach, fatten the sausages a lot.
	visual.upper_len = 0.38f;
	visual.lower_len = 0.34f;

	std::vector<Vertex> vertices;
	std::vector<std::uint32_t> indices;

	make_tapered_limb(vertices, indices, visual.upper_len, 0.095f, 0.080f, 0.085f, 0.070f, 8, kAvatarR, kAvatarG, kAvatarB);
	visual.upper = load_mesh(vertices, indices);

	make_tapered_limb(vertices, indices, visual.lower_len, 0.082f, 0.068f, 0.072f, 0.058f, 8, kAvatarR, kAvatarG, kAvatarB);
	visual.lower = load_mesh(vertices, indices);

	make_tapered_limb(vertices, indices, 0.040f, 0.048f, 0.042f, 0.044f, 0.038f, 8, kHandR, kHandG, kHandB);
	visual.wrist = load_mesh(vertices, indices);

	make_gorilla_palm(vertices, indices, kHandR, kHandG, kHandB);
	visual.palm = load_mesh(vertices, indices);

	// Closed-fist mitt stored in finger_prox (open mitt uses palm).
	make_gorilla_fist(vertices, indices, kHandR, kHandG, kHandB);
	visual.finger_prox = load_mesh(vertices, indices);
	visual.finger_dist = visual.finger_prox;
	visual.thumb_prox = visual.finger_prox;
	visual.thumb_dist = visual.finger_prox;

	make_joint_sphere(vertices, indices, 1.0f, 10, 8, 1.0f, 1.0f, 1.0f);
	visual.joint_ball = load_mesh(vertices, indices);

	if (!impl_->avatar_built) {
		make_avatar_torso(vertices, indices, kAvatarR, kAvatarG, kAvatarB);
		impl_->avatar_torso = load_mesh(vertices, indices);
		make_joint_sphere(vertices, indices, 1.0f, 10, 8, 1.0f, 1.0f, 1.0f);
		impl_->avatar_belly = load_mesh(vertices, indices);
		// Round head (slightly flattened vertically).
		make_joint_sphere(vertices, indices, 1.0f, 12, 10, 1.0f, 1.0f, 1.0f);
		for (Vertex& v : vertices) {
			v.py *= 1.05f;
			v.px *= 1.02f;
			v.pz *= 1.00f;
		}
		impl_->avatar_head = load_mesh(vertices, indices);
		make_avatar_face(vertices, indices, kFaceR, kFaceG, kFaceB);
		impl_->avatar_face = load_mesh(vertices, indices);
		make_avatar_ear(vertices, indices, kAvatarR, kAvatarG, kAvatarB);
		impl_->avatar_ear = load_mesh(vertices, indices);
		impl_->avatar_built = true;
	}

	return visual;
}

void Engine::draw_hand(const HandVisual& hand, bool close_fist) {
	Mat4 head = Mat4::identity();
	Mat4 grip = Mat4::identity();
	if (!impl_->xr.head_pose(head) || !impl_->xr.controller_pose(hand.side, grip)) {
		return;
	}

	const bool left = hand.side == Hand::Left;
	const float side = left ? -1.0f : 1.0f;

	constexpr float kDeg = 3.14159265f / 180.0f;
	// Hands-only debug: palm faces the camera (−Z), fingers hang down (−Y) so the V cleft reads.
	// Normal play: hang from the grip and face the palm toward −Z.
	constexpr float kHandPitchPlay = -80.0f * kDeg;
	constexpr float kHandPitchDebug = -90.0f * kDeg;
	const float hand_pitch = impl_->arms_visible ? kHandPitchPlay : kHandPitchDebug;
	const float hand_roll = impl_->arms_visible ? (left ? (8.0f * kDeg) : (-8.0f * kDeg)) : 0.0f;
	Mat4 palm_pose = Mat4::multiply(grip, Mat4::rotation_x(hand_pitch));
	palm_pose = Mat4::multiply(palm_pose, Mat4::rotation_z(hand_roll));
	if (left) {
		Mat4 mirror = Mat4::identity();
		mirror.m[0] = -1.0f;
		palm_pose = Mat4::multiply(palm_pose, mirror);
	}

	constexpr float kWristLen = 0.035f;
	constexpr float kBoneOverlap = 0.055f;
	const Vec3 wrist_joint = transform_point(palm_pose, Vec3{0.0f, 0.0f, 0.030f});
	const Vec3 palm_up = transform_dir(palm_pose, Vec3{0.0f, 1.0f, 0.0f});

	// Shoulders sit on the wide GT trunk.
	const Vec3 shoulder = transform_point(head, Vec3{side * 0.24f, -0.18f, 0.02f});
	const Vec3 pole = shoulder + transform_dir(head, Vec3{side * 0.12f, -0.50f, 0.08f});
	const Vec3 elbow = solve_elbow(shoulder, wrist_joint, hand.upper_len, hand.lower_len, pole);
	const Vec3 up_hint = transform_dir(head, Vec3{0.0f, 1.0f, 0.0f});

	auto draw_tracking = [&](MeshId mesh, const Mat4& model, float r = 1.0f, float g = 1.0f, float b = 1.0f) {
		queue_draw_tracking(impl_->draws, mesh, model, r, g, b, 1.0f);
	};

	auto draw_ball = [&](Vec3 center, float radius, MeshId mesh, float r, float g, float b) {
		if (mesh < 0) {
			return;
		}
		Mat4 m = Mat4::translation(center.x, center.y, center.z);
		m.m[0] *= radius;
		m.m[5] *= radius;
		m.m[10] *= radius;
		draw_tracking(mesh, m, r, g, b);
	};

	auto draw_bone = [&](MeshId mesh, Vec3 a, Vec3 b, float mesh_len, Vec3 bone_up) {
		const Vec3 delta = b - a;
		const float span = vec_length(delta);
		if (span < 1.0e-5f) {
			return;
		}
		const Vec3 dir = delta * (1.0f / span);
		draw_tracking(mesh, bone_matrix(a - dir * kBoneOverlap, b + dir * kBoneOverlap, bone_up, mesh_len));
	};

	if (impl_->avatar_visible && !left && impl_->avatar_built) {
		// Short legless trunk under HMD.
		const Vec3 torso_top = transform_point(head, Vec3{0.0f, -0.10f, 0.02f});
		const Vec3 torso_bot = transform_point(head, Vec3{0.0f, -0.48f, 0.04f});
		draw_tracking(impl_->avatar_torso, bone_matrix(torso_top, torso_bot, up_hint, 0.38f));
		// Belly on the FRONT (−Z), not the back (that was the pale back patch).
		draw_ball(transform_point(head, Vec3{0.0f, -0.30f, -0.06f}), 0.105f, impl_->avatar_belly, kBellyR, kBellyG, kBellyB);
		draw_ball(transform_point(head, Vec3{0.0f, -0.02f, 0.0f}), 0.130f, impl_->avatar_head, kAvatarR, kAvatarG, kAvatarB);
		// Face mask on the front of the head.
		draw_tracking(impl_->avatar_face, Mat4::multiply(head, Mat4::translation(0.0f, -0.01f, -0.112f)));
		// Large dark eyes on the mask.
		constexpr float kEyeR = 0.04f;
		constexpr float kEyeG = 0.04f;
		constexpr float kEyeB = 0.05f;
		draw_ball(transform_point(head, Vec3{-0.034f, 0.010f, -0.128f}), 0.032f, hand.joint_ball, kEyeR, kEyeG, kEyeB);
		draw_ball(transform_point(head, Vec3{0.034f, 0.010f, -0.128f}), 0.032f, hand.joint_ball, kEyeR, kEyeG, kEyeB);
		draw_ball(transform_point(head, Vec3{-0.040f, 0.016f, -0.150f}), 0.008f, hand.joint_ball, 0.92f, 0.92f, 0.95f);
		draw_ball(transform_point(head, Vec3{0.028f, 0.016f, -0.150f}), 0.008f, hand.joint_ball, 0.92f, 0.92f, 0.95f);
		// Small ears on the sides only.
		draw_ball(transform_point(head, Vec3{-0.122f, 0.01f, 0.0f}), 0.028f, impl_->avatar_ear, kAvatarR, kAvatarG, kAvatarB);
		draw_ball(transform_point(head, Vec3{0.122f, 0.01f, 0.0f}), 0.028f, impl_->avatar_ear, kAvatarR, kAvatarG, kAvatarB);
		if (impl_->arms_visible) {
			draw_ball(shoulder, 0.040f, hand.joint_ball, kAvatarR, kAvatarG, kAvatarB);
		}
	} else if (impl_->avatar_visible && left && impl_->arms_visible) {
		draw_ball(shoulder, 0.040f, hand.joint_ball, kAvatarR, kAvatarG, kAvatarB);
	}

	if (impl_->arms_visible) {
		draw_bone(hand.upper, shoulder, elbow, hand.upper_len, up_hint);
		const Vec3 forearm_up = vec_normalize(up_hint + palm_up);
		draw_bone(hand.lower, elbow, wrist_joint, hand.lower_len, forearm_up);
		draw_ball(elbow, 0.070f, hand.joint_ball, kAvatarR, kAvatarG, kAvatarB);
	}

	if (impl_->arms_visible) {
		const Vec3 wrist_tip = transform_point(palm_pose, Vec3{0.0f, 0.0f, -0.002f});
		draw_tracking(hand.wrist, bone_matrix(wrist_joint, wrist_tip, palm_up, kWristLen));
		draw_ball(wrist_joint, 0.042f, hand.joint_ball, kHandR, kHandG, kHandB);
	}
	// Open mitt vs closed fist (fist mesh lives in finger_prox).
	draw_tracking(close_fist ? hand.finger_prox : hand.palm, palm_pose);
	(void)side;
}

void Engine::set_avatar_visible(bool visible) {
	impl_->avatar_visible = visible;
}

void Engine::set_arms_visible(bool visible) {
	impl_->arms_visible = visible;
}

MeshId Engine::load_mesh(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices) {
	return impl_->renderer.create_mesh(vertices.data(), vertices.size(), indices.data(), indices.size());
}

MeshId Engine::load_obj(const std::string& path, float r, float g, float b) {
	std::vector<Vertex> vertices;
	std::vector<std::uint32_t> indices;
	load_obj_file(path, vertices, indices, r, g, b);
	return load_mesh(vertices, indices);
}

void Engine::draw_mesh(MeshId mesh, const Mat4& model, float r, float g, float b, float a) {
	queue_draw_tracking(impl_->draws, mesh, world_to_tracking(impl_->loco, model), r, g, b, a);
}

void Engine::submit_draws(std::span<const DrawItem> draws) {
	for (const DrawItem& item : draws) {
		DrawItem shifted = item;
		shifted.model = world_to_tracking(impl_->loco, item.model);
		impl_->draws.push_back(shifted);
	}
}

void Engine::end_frame() {
	if (!impl_->frame_open) {
		return;
	}
	impl_->xr.end_frame(impl_->renderer, impl_->draws, impl_->last_timings);
	if (impl_->mirror_enabled && impl_->mirror.alive()) {
		Mat4 view_proj = Mat4::identity();
		if (impl_->xr.left_eye_view_proj(view_proj)) {
			try {
				impl_->mirror.present(impl_->renderer, view_proj, impl_->draws, impl_->fps_smooth);
			} catch (const std::exception& ex) {
				std::cerr << "desktop mirror present failed: " << ex.what() << "\n";
				impl_->mirror_enabled = false;
			}
		}
	}
	impl_->frame_open = false;
	impl_->draws.clear();
}

void Engine::set_desktop_mirror(bool enabled) {
	impl_->mirror_enabled = enabled;
	if (enabled && !impl_->mirror.alive()) {
		try {
			impl_->mirror.initialize(impl_->renderer, impl_->xr.color_format(), 960, 540);
		} catch (const std::exception& ex) {
			std::cerr << "desktop mirror unavailable: " << ex.what() << "\n";
			impl_->mirror_enabled = false;
		}
	}
	if (!enabled) {
		impl_->mirror.shutdown();
	}
}

bool Engine::desktop_mirror_active() const {
	return impl_->mirror_enabled && impl_->mirror.alive();
}

void Engine::set_desktop_camera(float x, float y, float z, float yaw, float pitch) {
	if (impl_->xr.desktop_mode()) {
		impl_->xr.set_desktop_camera(x, y, z, yaw, pitch);
	}
}

void Engine::unlock_desktop_camera() {
	impl_->xr.unlock_desktop_camera();
}

bool Engine::save_desktop_screenshot(const std::string& path) const {
	if (!impl_->mirror_enabled || !impl_->mirror.alive()) {
		return false;
	}
	return impl_->mirror.save_screenshot_bmp(path);
}

FrameTimings Engine::timings() const {
	return impl_->last_timings;
}

bool Engine::controller_pose(Hand hand, Mat4& out_pose) const {
	if (!impl_->xr.controller_pose(hand, out_pose)) {
		return false;
	}
	apply_player_offset(impl_->loco, out_pose);
	return true;
}

bool Engine::head_pose(Mat4& out_pose) const {
	if (!impl_->xr.head_pose(out_pose)) {
		return false;
	}
	apply_player_offset(impl_->loco, out_pose);
	return true;
}

bool Engine::button_down(Hand hand, ControllerButton button) const {
	return impl_->xr.button_down(hand, button);
}

float Engine::delta_time() const {
	return impl_->dt;
}

std::string Engine::backend_name() const {
	if (impl_->xr.desktop_mode()) {
		return "Vulkan/Desktop (" + impl_->xr.runtime_name() + ")";
	}
	return "Vulkan/OpenXR (" + impl_->xr.runtime_name() + ")";
}

}  // namespace hypervr
