#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "hypervr/types.hpp"

namespace hypervr {

/**
 * High-level immediate VR engine: OpenXR session + Vulkan stereo submit.
 *
 * Apps own game logic; the engine owns frame sync, stereo rendering, uploads,
 * and XR input. Python bindings wrap this same type.
 */
class Engine {
public:
	/**
	 * Create Vulkan device + OpenXR session (blocks until runtime is ready).
	 *
	 * When ``desktop_mode`` is true, skips OpenXR and runs a keyboard-driven
	 * free-cam that only presents to the desktop mirror window.
	 * If OpenXR init fails and desktop_mode is false, falls back to desktop.
	 */
	explicit Engine(std::string application_name = "HyperVR", bool desktop_mode = false);

	/**
	 * Destroy XR session and Vulkan resources.
	 */
	~Engine();

	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;

	/**
	 * True while the XR session should keep running.
	 */
	bool is_running() const;

	/**
	 * Poll OpenXR events and refresh controller action state.
	 */
	void poll_events();

	/**
	 * Wait for the next XR frame and begin it (xrWaitFrame / xrBeginFrame).
	 */
	void begin_frame();

	/**
	 * Clear the native draw list for the current frame.
	 *
	 * Called automatically by begin_frame(); exposed for bindings symmetry.
	 */
	void clear_draws();

	/**
	 * Create a unit cube mesh and return its id.
	 */
	MeshId create_cube();

	/**
	 * Create a lightweight room mesh centered on the origin (floor top at y = 0).
	 */
	RoomVisual create_room(float width = 12.0f, float depth = 12.0f, float height = 7.0f);

	/**
	 * Draw the room at its authored world pose (locomotion shifts it under the player).
	 */
	void draw_room(const RoomVisual& room);

	/**
	 * Enable Gorilla Tag–style palm locomotion against a room's inner bounds.
	 *
	 * Called automatically by create_room(). Palms use high friction (little slide)
	 * and shove the body; torso drag is lighter (~1/3 hand friction).
	 */
	void enable_locomotion(const RoomVisual& room, LocomotionSettings settings = {});

	/**
	 * Enable or disable locomotion updates (poses still apply the last offset while disabled).
	 */
	void set_locomotion_enabled(bool enabled);

	/**
	 * World-space locomotion offset of the tracking origin (for debugging / UI).
	 */
	void player_position(float& x, float& y, float& z) const;

	/**
	 * World-space player velocity.
	 */
	void player_velocity(float& x, float& y, float& z) const;

	/**
	 * Create a procedural IK arm + low-poly gorilla hand for one side.
	 */
	HandVisual create_hand(Hand hand);

	/**
	 * Draw two-bone IK arm (shoulder under HMD → controller) and gorilla hand.
	 *
	 * Trigger curls the index finger, grip curls the other fingers, A/X curls the thumb.
	 * When `close_fist` is true, all fingers curl fully.
	 */
	void draw_hand(const HandVisual& hand, bool close_fist = false);

	/**
	 * Upload a static triangle mesh (positions+colors interleaved).
	 */
	MeshId load_mesh(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices);

	/**
	 * Load a Wavefront OBJ from disk and upload it (flat vertex color).
	 */
	MeshId load_obj(const std::string& path, float r = 0.22f, float g = 0.22f, float b = 0.24f);

	/**
	 * Queue a mesh draw with a column-major model matrix and tint color.
	 */
	void draw_mesh(MeshId mesh, const Mat4& model, float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f);

	/**
	 * Queue many draws in one call (preferred hot path from Python).
	 */
	void submit_draws(std::span<const DrawItem> draws);

	/**
	 * Record stereo Vulkan work and submit via xrEndFrame.
	 */
	void end_frame();

	/**
	 * Enable or disable the desktop left-eye mirror window (default on).
	 */
	void set_desktop_mirror(bool enabled);

	/**
	 * True while the desktop mirror window is open.
	 */
	bool desktop_mirror_active() const;

	/**
	 * Lock desktop free-cam to a world pose (yaw/pitch radians). No-op outside desktop mode.
	 */
	void set_desktop_camera(float x, float y, float z, float yaw, float pitch);

	/**
	 * Re-enable interactive desktop camera control.
	 */
	void unlock_desktop_camera();

	/**
	 * Save the latest mirror frame as a 32-bit BMP. Returns false on failure.
	 */
	bool save_desktop_screenshot(const std::string& path) const;

	/**
	 * When false, draw_hand skips torso/head/face (hands + arms only). Default true.
	 */
	void set_avatar_visible(bool visible);

	/**
	 * When false, draw_hand skips arm bones/shoulders and draws palm (+ wrist) only.
	 * Default true. Use with set_avatar_visible(false) for hand-mesh debugging.
	 */
	void set_arms_visible(bool visible);

	/**
	 * Last completed frame timings.
	 */
	FrameTimings timings() const;

	/**
	 * Write a column-major grip pose in world space (includes locomotion offset).
	 */
	bool controller_pose(Hand hand, Mat4& out_pose) const;

	/**
	 * Mid-eye HMD pose in world space; false if views are unavailable.
	 */
	bool head_pose(Mat4& out_pose) const;

	/**
	 * True if the named controller button is currently pressed.
	 */
	bool button_down(Hand hand, ControllerButton button) const;

	/**
	 * Seconds since the previous begin_frame().
	 */
	float delta_time() const;

	/**
	 * Backend / runtime diagnostic name.
	 */
	std::string backend_name() const;

private:
	struct Impl;
	Impl* impl_ = nullptr;
};

}  // namespace hypervr
