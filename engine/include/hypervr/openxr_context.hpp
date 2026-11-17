#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "hypervr/types.hpp"
#include "hypervr/vulkan_renderer.hpp"

namespace hypervr {

/**
 * OpenXR session bound to Vulkan, including stereo swapchains and actions.
 */
class OpenXrContext {
public:
	OpenXrContext() = default;
	~OpenXrContext();

	OpenXrContext(const OpenXrContext&) = delete;
	OpenXrContext& operator=(const OpenXrContext&) = delete;

	/**
	 * Create instance, Vulkan binding, session, swapchains, and action set.
	 *
	 * Initializes `renderer` after the swapchain color format is known.
	 */
	void initialize(const std::string& application_name, VulkanRenderer& renderer, const std::string& shader_directory);

	/**
	 * Headset-free debug path: standalone Vulkan + fixed player + free camera.
	 *
	 * Drag to look, WASD moves relative to look direction, Space/Ctrl up/down.
	 */
	void initialize_desktop(const std::string& application_name, VulkanRenderer& renderer, const std::string& shader_directory);

	/**
	 * Vulkan handles created for this session (owned by this context).
	 */
	VkInstance vk_instance() const { return vk_instance_; }
	VkPhysicalDevice vk_physical_device() const { return vk_physical_; }
	VkDevice vk_device() const { return vk_device_; }
	VkQueue vk_queue() const { return vk_queue_; }
	std::uint32_t queue_family_index() const { return queue_family_; }
	VkFormat color_format() const { return color_format_; }

	/**
	 * Apply mouse-wheel dolly (positive = move forward along look).
	 */
	void desktop_add_scroll(float scroll_y);

	/**
	 * Lock the desktop free-cam to an explicit pose (model-review automation).
	 */
	void set_desktop_camera(float x, float y, float z, float yaw, float pitch);

	/**
	 * Allow WASD / mouse look again after set_desktop_camera.
	 */
	void unlock_desktop_camera();

	/**
	 * True when running without an OpenXR session (desktop debug).
	 */
	bool desktop_mode() const { return desktop_mode_; }

	/**
	 * Request the main loop to exit (used when the mirror window closes).
	 */
	void request_exit() { running_ = false; }

	/**
	 * Poll XR events; may mark the session as stopping.
	 */
	void poll_events();

	/**
	 * True while the app should keep looping.
	 */
	bool is_running() const { return running_; }

	/**
	 * xrWaitFrame + xrBeginFrame; returns false if the frame should be skipped.
	 */
	bool begin_frame(FrameTimings& timings);

	/**
	 * Render queued draws to both eyes and xrEndFrame.
	 */
	void end_frame(VulkanRenderer& renderer, const std::vector<DrawItem>& draws, FrameTimings& timings);

	/**
	 * Copy grip pose for a hand into out_pose.
	 */
	bool controller_pose(Hand hand, Mat4& out_pose) const;

	/**
	 * Query a boolean controller action.
	 */
	bool button_down(Hand hand, ControllerButton button) const;

	/**
	 * Sync action state for the predicted display time from begin_frame.
	 */
	void sync_actions();

	/**
	 * Mid-eye / HMD pose in app space from the last begin_frame(); false if unavailable.
	 */
	bool head_pose(Mat4& out_pose) const;

	/**
	 * Left-eye view-projection for the current predicted frame (desktop mirror).
	 */
	bool left_eye_view_proj(Mat4& out_view_proj) const;

	/**
	 * Analog trigger value in [0,1] (index finger curl).
	 */
	float trigger_value(Hand hand) const;

	/**
	 * Analog grip/squeeze value in [0,1] (pinky curl).
	 */
	float grip_value(Hand hand) const;

	std::string runtime_name() const { return runtime_name_; }

private:
	struct Swapchain {
		XrSwapchain handle = XR_NULL_HANDLE;
		std::int32_t width = 0;
		std::int32_t height = 0;
		VkFormat format = VK_FORMAT_UNDEFINED;
		std::vector<XrSwapchainImageVulkanKHR> images;
		std::vector<VkImageView> views;
	};

	void create_instance(const std::string& application_name);
	void create_vulkan_via_openxr();
	void create_vulkan_standalone();
	void create_session();
	VkFormat pick_swapchain_format() const;
	void create_swapchains();
	void create_actions();
	void destroy_swapchains();
	void update_desktop_poses(float dt);
	Mat4 pose_to_matrix(const XrPosef& pose) const;
	Mat4 projection_matrix(const XrFovf& fov, float near_z, float far_z) const;
	XrPosef make_pose(float x, float y, float z, float yaw, float pitch) const;
	/** Build view-projection for a camera pose (desktop orbit / eye). */
	Mat4 view_proj_from_pose(const XrPosef& pose, const XrFovf& fov) const;

	XrInstance instance_ = XR_NULL_HANDLE;
	XrSystemId system_id_ = XR_NULL_SYSTEM_ID;
	XrSession session_ = XR_NULL_HANDLE;
	XrSpace app_space_ = XR_NULL_HANDLE;
	XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
	bool running_ = true;
	bool session_running_ = false;
	bool frame_started_ = false;
	bool should_render_ = false;
	bool desktop_mode_ = false;
	XrTime predicted_display_time_ = 0;
	std::string runtime_name_;

	/** Desktop free camera (drag look + WASD fly). */
	float cam_yaw_ = 3.14159265f;  // face the player (player looks -Z)
	float cam_pitch_ = -0.12f;
	float cam_x_ = 0.0f;
	float cam_y_ = 1.45f;
	float cam_z_ = -2.8f;
	float cam_scroll_pending_ = 0.0f;
	XrPosef player_head_{};
	XrPosef orbit_camera_{};  // spectator camera pose used by the mirror
	int cam_mouse_x_ = 0;
	int cam_mouse_y_ = 0;
	bool cam_mouse_init_ = false;
	bool desktop_camera_locked_ = false;
	std::chrono::steady_clock::time_point desktop_last_tick_{};

	VkInstance vk_instance_ = VK_NULL_HANDLE;
	VkPhysicalDevice vk_physical_ = VK_NULL_HANDLE;
	VkDevice vk_device_ = VK_NULL_HANDLE;
	std::uint32_t queue_family_ = 0;
	VkQueue vk_queue_ = VK_NULL_HANDLE;
	VkFormat color_format_ = VK_FORMAT_R8G8B8A8_SRGB;
	bool owns_vulkan_ = false;

	std::array<XrViewConfigurationView, 2> config_views_{};
	std::array<XrView, 2> views_{};
	std::array<Swapchain, 2> swapchains_{};
	std::uint32_t view_count_ = 2;

	XrActionSet action_set_ = XR_NULL_HANDLE;
	XrAction grip_pose_action_ = XR_NULL_HANDLE;
	XrAction trigger_action_ = XR_NULL_HANDLE;
	XrAction grip_action_ = XR_NULL_HANDLE;
	XrAction menu_action_ = XR_NULL_HANDLE;
	XrAction ax_action_ = XR_NULL_HANDLE;
	XrAction by_action_ = XR_NULL_HANDLE;
	std::array<XrPath, 2> hand_paths_{};
	std::array<XrSpace, 2> grip_spaces_{};
	std::array<XrPosef, 2> grip_poses_{};
	std::array<bool, 2> grip_valid_{};
	std::array<float, 2> trigger_value_{};
	std::array<float, 2> grip_value_{};
	std::array<bool, 2> trigger_down_{};
	std::array<bool, 2> grip_down_{};
	std::array<bool, 2> menu_down_{};
	std::array<bool, 2> ax_down_{};
	std::array<bool, 2> by_down_{};

	PFN_xrGetVulkanGraphicsRequirements2KHR pfn_get_vk_reqs2_ = nullptr;
	PFN_xrCreateVulkanInstanceKHR pfn_create_vk_instance_ = nullptr;
	PFN_xrCreateVulkanDeviceKHR pfn_create_vk_device_ = nullptr;
};

}  // namespace hypervr
