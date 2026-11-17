/**
 * Native HyperVR demo: room + procedural IK arms + physics cube (grab / throw).
 */
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>

#include "hypervr/engine.hpp"

namespace {

/** Simple 3D vector for cube physics. */
struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

/** Return vector length. */
float length(Vec3 v) {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/** Scale a vector. */
Vec3 operator*(Vec3 v, float s) {
	return {v.x * s, v.y * s, v.z * s};
}

/** Add two vectors. */
Vec3 operator+(Vec3 a, Vec3 b) {
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

/** Subtract two vectors. */
Vec3 operator-(Vec3 a, Vec3 b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

/** Origin of a transform. */
Vec3 mat_origin(const hypervr::Mat4& m) {
	return {m.m[12], m.m[13], m.m[14]};
}

/** Copy the 3x3 rotation block from a matrix. */
hypervr::Mat4 mat_rotation(const hypervr::Mat4& m) {
	hypervr::Mat4 out = hypervr::Mat4::identity();
	out.m[0] = m.m[0];
	out.m[1] = m.m[1];
	out.m[2] = m.m[2];
	out.m[4] = m.m[4];
	out.m[5] = m.m[5];
	out.m[6] = m.m[6];
	out.m[8] = m.m[8];
	out.m[9] = m.m[9];
	out.m[10] = m.m[10];
	return out;
}

/**
 * Rigid cube body: gravity, room AABB contacts, grab, and throw.
 */
struct CubeBody {
	float half = 0.075f;
	Vec3 pos{0.0f, 0.095f, -1.2f};
	Vec3 vel{};
	Vec3 ang_vel{};
	hypervr::Mat4 orient = hypervr::Mat4::identity();

	std::optional<hypervr::Hand> held_by;
	hypervr::Mat4 grab_offset = hypervr::Mat4::identity();
	Vec3 last_grip{};
	bool have_last_grip = false;

	/** World model matrix (rotation * uniform scale at center). */
	hypervr::Mat4 model_matrix() const {
		return hypervr::Mat4::multiply(
			hypervr::Mat4::translation(pos.x, pos.y, pos.z),
			hypervr::Mat4::multiply(orient, hypervr::Mat4::scale(half * 2.0f)));
	}
};

/**
 * Apply a small world-space angular step to an orientation matrix.
 */
void integrate_orientation(hypervr::Mat4& orient, Vec3 ang_vel, float dt) {
	const float w = length(ang_vel);
	if (w < 1.0e-5f) {
		return;
	}
	const float ax = ang_vel.x / w;
	const float ay = ang_vel.y / w;
	const float az = ang_vel.z / w;
	const float angle = w * dt;
	const float c = std::cos(angle);
	const float s = std::sin(angle);
	const float t = 1.0f - c;

	// Rodrigues rotation matrix about (ax,ay,az).
	hypervr::Mat4 R = hypervr::Mat4::identity();
	R.m[0] = t * ax * ax + c;
	R.m[1] = t * ax * ay + s * az;
	R.m[2] = t * ax * az - s * ay;
	R.m[4] = t * ax * ay - s * az;
	R.m[5] = t * ay * ay + c;
	R.m[6] = t * ay * az + s * ax;
	R.m[8] = t * ax * az + s * ay;
	R.m[9] = t * ay * az - s * ax;
	R.m[10] = t * az * az + c;
	orient = hypervr::Mat4::multiply(R, orient);
}

/** Dot product. */
float dot(Vec3 a, Vec3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

/** Cross product. */
Vec3 cross(Vec3 a, Vec3 b) {
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x};
}

/** Sign of a float (−1, 0, or +1). */
float signf(float v) {
	if (v > 0.0f) {
		return 1.0f;
	}
	if (v < 0.0f) {
		return -1.0f;
	}
	return 0.0f;
}

/** Local axes of the cube orientation (column-major Mat4). */
void cube_axes(const hypervr::Mat4& orient, Vec3& ax, Vec3& ay, Vec3& az) {
	ax = {orient.m[0], orient.m[1], orient.m[2]};
	ay = {orient.m[4], orient.m[5], orient.m[6]};
	az = {orient.m[8], orient.m[9], orient.m[10]};
}

/**
 * Support point of an oriented cube (OBB) in world direction `dir`.
 * This is the farthest corner along that direction — real box shape, not a sphere.
 */
Vec3 obb_support(Vec3 center, const hypervr::Mat4& orient, float half, Vec3 dir) {
	Vec3 ax{};
	Vec3 ay{};
	Vec3 az{};
	cube_axes(orient, ax, ay, az);
	return center + ax * (half * signf(dot(ax, dir))) + ay * (half * signf(dot(ay, dir))) +
		az * (half * signf(dot(az, dir)));
}

/**
 * Resolve cube OBB against one inward-facing room plane.
 * `normal` points into the free volume; constraint is n·x >= plane.
 */
bool resolve_obb_plane(
	CubeBody& cube,
	Vec3 normal,
	float plane,
	float restitution,
	float linear_friction,
	float angular_friction,
	bool* hit_floor) {
	const Vec3 support = obb_support(cube.pos, cube.orient, cube.half, normal * -1.0f);
	const float penetration = plane - dot(support, normal);
	if (penetration <= 0.0f) {
		return false;
	}

	cube.pos = cube.pos + normal * penetration;

	const float vn = dot(cube.vel, normal);
	if (vn < 0.0f) {
		// Soft contacts when nearly resting — bounce only on real impacts.
		const float e = (std::fabs(vn) < 0.8f) ? 0.0f : restitution;
		cube.vel = cube.vel - normal * (vn * (1.0f + e));
	}

	const float vn_after = dot(cube.vel, normal);
	const Vec3 tangent = (cube.vel - normal * vn_after) * linear_friction;
	cube.vel = normal * vn_after + tangent;
	cube.ang_vel = cube.ang_vel * angular_friction;

	if (hit_floor && normal.y > 0.5f) {
		*hit_floor = true;
	}
	return true;
}

/**
 * Pick the cube axis most aligned with world up (pointing upward).
 */
Vec3 nearest_up_axis(const hypervr::Mat4& orient, float& out_alignment) {
	Vec3 ax{};
	Vec3 ay{};
	Vec3 az{};
	cube_axes(orient, ax, ay, az);
	const Vec3 axes[3]{ax, ay, az};
	const Vec3 up{0.0f, 1.0f, 0.0f};
	out_alignment = 0.0f;
	Vec3 best{0.0f, 1.0f, 0.0f};
	for (const Vec3& a : axes) {
		const float d = dot(a, up);
		const float ad = std::fabs(d);
		if (ad > out_alignment) {
			out_alignment = ad;
			best = (d >= 0.0f) ? a : (a * -1.0f);
		}
	}
	return best;
}

/**
 * Rotate orientation so `from` maps onto `to` (one-shot Rodrigues).
 */
void rotate_orient_towards(hypervr::Mat4& orient, Vec3 from, Vec3 to) {
	const float from_len = length(from);
	const float to_len = length(to);
	if (from_len < 1.0e-6f || to_len < 1.0e-6f) {
		return;
	}
	from = from * (1.0f / from_len);
	to = to * (1.0f / to_len);
	const float cos_a = std::clamp(dot(from, to), -1.0f, 1.0f);
	Vec3 axis = cross(from, to);
	const float axis_len = length(axis);
	if (axis_len < 1.0e-6f) {
		return;
	}
	axis = axis * (1.0f / axis_len);
	const float angle = std::acos(cos_a);
	integrate_orientation(orient, axis * angle, 1.0f);
}

/**
 * Stable floor settle: gentle gravity tip while tilted, hard flatten when close.
 */
void settle_cube_on_floor(CubeBody& cube, float dt) {
	const Vec3 up{0.0f, 1.0f, 0.0f};
	float alignment = 0.0f;
	const Vec3 face = nearest_up_axis(cube.orient, alignment);

	if (alignment > 0.97f) {
		rotate_orient_towards(cube.orient, face, up);
		cube.ang_vel = {};
		return;
	}

	if (alignment > 0.88f) {
		rotate_orient_towards(cube.orient, face, up);
		cube.ang_vel = cube.ang_vel * 0.4f;
		return;
	}

	const Vec3 contact = obb_support(cube.pos, cube.orient, cube.half, {0.0f, -1.0f, 0.0f});
	const Vec3 r = contact - cube.pos;
	constexpr float kTipGain = 18.0f;
	cube.ang_vel = cube.ang_vel + cross(r, up * 9.81f) * (kTipGain * dt);

	const float max_spin = 10.0f;
	if (length(cube.ang_vel) > max_spin) {
		cube.ang_vel = cube.ang_vel * (max_spin / length(cube.ang_vel));
	}
}

/**
 * Integrate free-flying cube against the room interior using OBB contacts.
 */
void integrate_cube(
	CubeBody& cube,
	float dt,
	float room_half_w,
	float room_half_d,
	float room_height) {
	if (dt <= 1.0e-5f || dt > 0.1f) {
		return;
	}

	constexpr float kGravity = -9.81f;
	constexpr float kRestitution = 0.25f;
	constexpr float kAirDrag = 0.15f;
	constexpr float kAngDrag = 1.2f;
	constexpr float kSleepSpeed = 0.06f;

	cube.vel.y += kGravity * dt;
	cube.vel = cube.vel * std::exp(-kAirDrag * dt);
	cube.ang_vel = cube.ang_vel * std::exp(-kAngDrag * dt);

	cube.pos = cube.pos + cube.vel * dt;
	integrate_orientation(cube.orient, cube.ang_vel, dt);

	const float floor_lin = std::exp(-6.0f * dt);
	const float floor_ang = std::exp(-2.5f * dt);
	const float wall_lin = std::exp(-1.5f * dt);
	const float wall_ang = std::exp(-1.5f * dt);

	bool on_floor = false;
	resolve_obb_plane(cube, {0.0f, 1.0f, 0.0f}, 0.0f, kRestitution, floor_lin, floor_ang, &on_floor);
	resolve_obb_plane(cube, {0.0f, -1.0f, 0.0f}, -room_height, kRestitution, wall_lin, wall_ang, nullptr);
	resolve_obb_plane(cube, {1.0f, 0.0f, 0.0f}, -room_half_w, kRestitution, wall_lin, wall_ang, nullptr);
	resolve_obb_plane(cube, {-1.0f, 0.0f, 0.0f}, -room_half_w, kRestitution, wall_lin, wall_ang, nullptr);
	resolve_obb_plane(cube, {0.0f, 0.0f, 1.0f}, -room_half_d, kRestitution, wall_lin, wall_ang, nullptr);
	resolve_obb_plane(cube, {0.0f, 0.0f, -1.0f}, -room_half_d, kRestitution, wall_lin, wall_ang, nullptr);

	if (on_floor) {
		settle_cube_on_floor(cube, dt);
		resolve_obb_plane(cube, {0.0f, 1.0f, 0.0f}, 0.0f, 0.0f, floor_lin, floor_ang, &on_floor);
	}

	float alignment = 0.0f;
	nearest_up_axis(cube.orient, alignment);
	const Vec3 lowest = obb_support(cube.pos, cube.orient, cube.half, {0.0f, -1.0f, 0.0f});
	if (on_floor && alignment > 0.97f && lowest.y <= 2.0e-3f && length(cube.vel) < kSleepSpeed) {
		cube.vel = {};
		cube.ang_vel = {};
		cube.pos.y += -lowest.y;
	}
}

/**
 * Sync cube transform from a full world pose (used while grabbed).
 */
void cube_from_pose(CubeBody& cube, const hypervr::Mat4& pose) {
	cube.pos = mat_origin(pose);
	cube.orient = mat_rotation(pose);
	// Strip scale from the rotation columns if the pose includes uniform scale.
	const float sx = std::sqrt(
		pose.m[0] * pose.m[0] + pose.m[1] * pose.m[1] + pose.m[2] * pose.m[2]);
	if (sx > 1.0e-4f) {
		cube.orient.m[0] /= sx;
		cube.orient.m[1] /= sx;
		cube.orient.m[2] /= sx;
		cube.orient.m[4] /= sx;
		cube.orient.m[5] /= sx;
		cube.orient.m[6] /= sx;
		cube.orient.m[8] /= sx;
		cube.orient.m[9] /= sx;
		cube.orient.m[10] /= sx;
	}
}

/**
 * Try to start or continue a grip-grab; on release keep throw velocity.
 */
void update_grab(hypervr::Engine& engine, hypervr::Hand hand, CubeBody& cube, float grab_radius) {
	hypervr::Mat4 grip = hypervr::Mat4::identity();
	if (!engine.controller_pose(hand, grip)) {
		if (cube.held_by && *cube.held_by == hand) {
			cube.held_by.reset();
			cube.have_last_grip = false;
		}
		return;
	}

	const bool squeezing = engine.button_down(hand, hypervr::ControllerButton::Grip);
	const Vec3 grip_pos = mat_origin(grip);
	const float dt = std::max(engine.delta_time(), 1.0e-4f);

	if (cube.held_by && *cube.held_by == hand) {
		if (!squeezing) {
			// Throw: hand linear velocity + a bit of spin from the flick.
			if (cube.have_last_grip) {
				cube.vel = (grip_pos - cube.last_grip) * (1.0f / dt);
				cube.ang_vel = {
					(cube.vel.z) * 2.0f,
					(-cube.vel.x) * 2.0f,
					0.0f};
			}
			cube.held_by.reset();
			cube.have_last_grip = false;
			return;
		}

		hypervr::Mat4 held = hypervr::Mat4::multiply(grip, cube.grab_offset);
		cube_from_pose(cube, held);
		cube.vel = {};
		cube.ang_vel = {};
		if (cube.have_last_grip) {
			const Vec3 instant = (grip_pos - cube.last_grip) * (1.0f / dt);
			// Keep a short trail so release feels snappy even on a short squeeze.
			cube.vel = instant;
		}
		cube.last_grip = grip_pos;
		cube.have_last_grip = true;
		return;
	}

	// Free grab, or steal from the other hand when this grip squeezes in range.
	if (squeezing && hypervr::Mat4::distance(grip, cube.model_matrix()) <= grab_radius) {
		cube.held_by = hand;
		cube.grab_offset = hypervr::Mat4::multiply(
			hypervr::Mat4::invert_rigid(grip),
			cube.model_matrix());
		cube.vel = {};
		cube.ang_vel = {};
		cube.last_grip = grip_pos;
		cube.have_last_grip = true;
		cube_from_pose(cube, hypervr::Mat4::multiply(grip, cube.grab_offset));
	}
}

}  // namespace

int main() {
	try {
		hypervr::Engine engine("HyperVR xr_demo");
		std::cout << "HyperVR backend: " << engine.backend_name() << "\n";
		std::cout << "You spawn inside a 12x12m room (7m ceiling). Push off the floor/walls with your hands to move (Gorilla Tag style).\n";
		std::cout << "Squeeze grip near the cube to grab it; release to throw.\n";

		constexpr float kRoomW = 12.0f;
		constexpr float kRoomD = 12.0f;
		constexpr float kRoomH = 7.0f;
		const hypervr::RoomVisual room = engine.create_room(kRoomW, kRoomD, kRoomH);
		const hypervr::MeshId cube_mesh = engine.create_cube();
		const hypervr::HandVisual left_arm = engine.create_hand(hypervr::Hand::Left);
		const hypervr::HandVisual right_arm = engine.create_hand(hypervr::Hand::Right);

		CubeBody cube{};
		cube.half = 0.075f;
		cube.pos = {0.0f, cube.half + 0.02f, -1.2f};
		constexpr float kGrabRadius = 0.15f + 0.04f;

		while (engine.is_running()) {
			engine.poll_events();
			engine.begin_frame();

			engine.draw_room(room);

			update_grab(engine, hypervr::Hand::Left, cube, kGrabRadius);
			update_grab(engine, hypervr::Hand::Right, cube, kGrabRadius);

			if (!cube.held_by.has_value()) {
				// Use the same bounds as the rendered room / locomotion.
				integrate_cube(
					cube,
					engine.delta_time(),
					room.width * 0.5f,
					room.depth * 0.5f,
					room.height);
			}

			const bool held = cube.held_by.has_value();
			engine.draw_mesh(
				cube_mesh,
				cube.model_matrix(),
				held ? 0.35f : 1.0f,
				held ? 0.85f : 0.55f,
				held ? 0.40f : 0.20f,
				1.0f);

			engine.draw_hand(left_arm, cube.held_by && *cube.held_by == hypervr::Hand::Left);
			engine.draw_hand(right_arm, cube.held_by && *cube.held_by == hypervr::Hand::Right);

			engine.end_frame();

			const hypervr::FrameTimings t = engine.timings();
			static int frame = 0;
			if ((frame++ % 90) == 0) {
				std::cout << "frame timings ms — wait=" << t.wait_ms << " record=" << t.cpu_record_ms
						  << " submit=" << t.submit_ms << " total=" << t.total_ms << "\n";
			}
		}
	} catch (const std::exception& ex) {
		std::cerr << "HyperVR xr_demo failed: " << ex.what() << "\n";
		return 1;
	}
	return 0;
}
