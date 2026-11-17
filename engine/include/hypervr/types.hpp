#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace hypervr {

/**
 * Opaque mesh handle returned by Engine::create_cube / load_mesh.
 */
using MeshId = int;

/**
 * Hand index for XR controller queries (0 = left, 1 = right).
 */
enum class Hand : int {
	Left = 0,
	Right = 1,
};

/**
 * Simple controller button ids exposed to apps.
 */
enum class ControllerButton : int {
	Trigger = 0,
	Grip = 1,
	Menu = 2,
	A_X = 3,
	B_Y = 4,
};

/**
 * Per-frame timing breakdown in milliseconds.
 */
struct FrameTimings {
	float wait_ms = 0.0f;
	float cpu_record_ms = 0.0f;
	float submit_ms = 0.0f;
	float total_ms = 0.0f;
};

/**
 * Column-major 4x4 matrix (OpenXR / Vulkan convention).
 */
struct Mat4 {
	std::array<float, 16> m{
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1};

	/** Return an identity matrix. */
	static Mat4 identity() {
		return Mat4{};
	}

	/** Translation matrix. */
	static Mat4 translation(float x, float y, float z) {
		Mat4 out = identity();
		out.m[12] = x;
		out.m[13] = y;
		out.m[14] = z;
		return out;
	}

	/** Uniform scale matrix. */
	static Mat4 scale(float s) {
		Mat4 out{};
		out.m[0] = s;
		out.m[5] = s;
		out.m[10] = s;
		return out;
	}

	/** Rotation about X axis (radians). */
	static Mat4 rotation_x(float radians) {
		const float c = std::cos(radians);
		const float s = std::sin(radians);
		Mat4 out = identity();
		out.m[5] = c;
		out.m[6] = s;
		out.m[9] = -s;
		out.m[10] = c;
		return out;
	}

	/** Rotation about Y axis (radians). */
	static Mat4 rotation_y(float radians) {
		const float c = std::cos(radians);
		const float s = std::sin(radians);
		Mat4 out = identity();
		out.m[0] = c;
		out.m[2] = -s;
		out.m[8] = s;
		out.m[10] = c;
		return out;
	}

	/** Rotation about Z axis (radians). */
	static Mat4 rotation_z(float radians) {
		const float c = std::cos(radians);
		const float s = std::sin(radians);
		Mat4 out = identity();
		out.m[0] = c;
		out.m[1] = s;
		out.m[4] = -s;
		out.m[5] = c;
		return out;
	}

	/** Multiply two matrices: result = a * b. */
	static Mat4 multiply(const Mat4& a, const Mat4& b) {
		Mat4 out{};
		for (int col = 0; col < 4; ++col) {
			for (int row = 0; row < 4; ++row) {
				float sum = 0.0f;
				for (int k = 0; k < 4; ++k) {
					sum += a.m[k * 4 + row] * b.m[col * 4 + k];
				}
				out.m[col * 4 + row] = sum;
			}
		}
		return out;
	}

	/** Inverse of a rigid (rotation + translation) transform. */
	static Mat4 invert_rigid(const Mat4& src) {
		Mat4 out = identity();
		out.m[0] = src.m[0];
		out.m[1] = src.m[4];
		out.m[2] = src.m[8];
		out.m[4] = src.m[1];
		out.m[5] = src.m[5];
		out.m[6] = src.m[9];
		out.m[8] = src.m[2];
		out.m[9] = src.m[6];
		out.m[10] = src.m[10];
		out.m[12] = -(out.m[0] * src.m[12] + out.m[4] * src.m[13] + out.m[8] * src.m[14]);
		out.m[13] = -(out.m[1] * src.m[12] + out.m[5] * src.m[13] + out.m[9] * src.m[14]);
		out.m[14] = -(out.m[2] * src.m[12] + out.m[6] * src.m[13] + out.m[10] * src.m[14]);
		return out;
	}

	/** Euclidean distance between two transform origins. */
	static float distance(const Mat4& a, const Mat4& b) {
		const float dx = a.m[12] - b.m[12];
		const float dy = a.m[13] - b.m[13];
		const float dz = a.m[14] - b.m[14];
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}
};

/**
 * Interleaved position + color vertex.
 */
struct Vertex {
	float px, py, pz;
	float r, g, b;
};

/**
 * One queued mesh draw for the current frame.
 */
struct DrawItem {
	MeshId mesh = -1;
	Mat4 model = Mat4::identity();
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float a = 1.0f;
};

/**
 * Procedural gorilla arm / hand mesh set for one controller.
 *
 * ``joint_ball`` seals elbow / wrist / knuckle gaps. Torso / head live on the
 * engine (shared) and are drawn from ``draw_hand`` once per frame.
 */
struct HandVisual {
	MeshId upper = -1;
	MeshId lower = -1;
	MeshId wrist = -1;
	MeshId palm = -1;
	MeshId finger_prox = -1;
	MeshId finger_dist = -1;
	MeshId thumb_prox = -1;
	MeshId thumb_dist = -1;
	MeshId joint_ball = -1;
	Hand side = Hand::Right;
	float upper_len = 0.34f;
	float lower_len = 0.30f;
};

/**
 * Lightweight axis-aligned play room (floor, walls, ceiling) centered on the origin.
 *
 * Floor top is at y = 0 so a standing OpenXR origin sits inside the room.
 */
struct RoomVisual {
	MeshId mesh = -1;
	float width = 12.0f;
	float depth = 12.0f;
	float height = 7.0f;
};

/**
 * Tuning mirroring Another Axiom GorillaLocomotion Player.cs.
 *
 * No floor/wall special cases — same stick + velocity-history path everywhere.
 */
struct LocomotionSettings {
	/** World gravity (m/s²). Negative = down. */
	float gravity = -9.8f;
	/** Hand sphere radius (GT minimumRaycastDistance). */
	float minimum_raycast_distance = 0.05f;
	/** Ignore body-velocity averages below this (GT velocityLimit). */
	float velocity_limit = 0.35f;
	/** Scales averaged stick velocity into launch (GT jumpMultiplier). */
	float jump_multiplier = 0.95f;
	/** Cap on launch speed (GT maxJumpSpeed). */
	float max_jump_speed = 5.8f;
	/** Samples in the body-velocity average (GT velocityHistorySize). */
	int velocity_history_size = 8;
	/** Max controller reach from head (GT maxArmLength). */
	float max_arm_length = 1.5f;
	/** Stretch past this releases a stuck hand (GT unStickDistance). */
	float unstick_distance = 1.0f;
	/** Two-hand surface slip (GT defaultSlideFactor). One-hand ≈ 0.001. */
	float default_slide_factor = 0.03f;
	/** Sphere-cast precision (GT defaultPrecision). */
	float default_precision = 0.995f;
	/**
	 * Horizontal damping (1/s) while grounded with no hands planted.
	 * PhysX capsule friction stand-in — keep small so slides still feel glidey.
	 */
	float body_floor_friction = 3.0f;
};

}  // namespace hypervr
