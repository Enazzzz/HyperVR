#include "hypervr/openxr_context.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hypervr {
namespace {

/**
 * Throw on failed OpenXR result.
 */
void check_xr(XrResult result, const char* what) {
	if (XR_FAILED(result)) {
		throw std::runtime_error(std::string("OpenXR error in ") + what + " (" + std::to_string(static_cast<int>(result)) + ")");
	}
}

/**
 * True if an extension name is in the runtime list.
 */
bool has_extension(const std::vector<XrExtensionProperties>& props, const char* name) {
	for (const XrExtensionProperties& p : props) {
		if (std::strcmp(p.extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

}  // namespace

OpenXrContext::~OpenXrContext() {
	if (session_ != XR_NULL_HANDLE && session_running_) {
		xrEndSession(session_);
		session_running_ = false;
	}

	destroy_swapchains();

	for (XrSpace& space : grip_spaces_) {
		if (space != XR_NULL_HANDLE) {
			xrDestroySpace(space);
			space = XR_NULL_HANDLE;
		}
	}
	if (action_set_ != XR_NULL_HANDLE) {
		xrDestroyActionSet(action_set_);
		action_set_ = XR_NULL_HANDLE;
	}
	if (app_space_ != XR_NULL_HANDLE) {
		xrDestroySpace(app_space_);
		app_space_ = XR_NULL_HANDLE;
	}
	if (session_ != XR_NULL_HANDLE) {
		xrDestroySession(session_);
		session_ = XR_NULL_HANDLE;
	}
	if (instance_ != XR_NULL_HANDLE) {
		xrDestroyInstance(instance_);
		instance_ = XR_NULL_HANDLE;
	}

	// Vulkan device/instance were created via OpenXR helpers; destroy if we own them.
	if (owns_vulkan_) {
		if (vk_device_ != VK_NULL_HANDLE) {
			vkDestroyDevice(vk_device_, nullptr);
			vk_device_ = VK_NULL_HANDLE;
		}
		if (vk_instance_ != VK_NULL_HANDLE) {
			vkDestroyInstance(vk_instance_, nullptr);
			vk_instance_ = VK_NULL_HANDLE;
		}
	}
}

void OpenXrContext::initialize(
	const std::string& application_name,
	VulkanRenderer& renderer,
	const std::string& shader_directory) {
	create_instance(application_name);
	create_vulkan_via_openxr();
	create_session();
	color_format_ = pick_swapchain_format();
	renderer.initialize(
		vk_instance_,
		vk_physical_,
		vk_device_,
		queue_family_,
		vk_queue_,
		color_format_,
		shader_directory);
	create_swapchains();
	create_actions();
}

void OpenXrContext::initialize_desktop(
	const std::string& application_name,
	VulkanRenderer& renderer,
	const std::string& shader_directory) {
	(void)application_name;
	desktop_mode_ = true;
	runtime_name_ = "Desktop (no headset)";
	color_format_ = VK_FORMAT_B8G8R8A8_SRGB;
	create_vulkan_standalone();
	renderer.initialize(
		vk_instance_,
		vk_physical_,
		vk_device_,
		queue_family_,
		vk_queue_,
		color_format_,
		shader_directory);

	// Seed a stereo-ish FOV and standing head / hand poses.
	view_count_ = 2;
	for (XrView& view : views_) {
		view = XrView{XR_TYPE_VIEW};
		view.fov.angleLeft = -0.85f;
		view.fov.angleRight = 0.85f;
		view.fov.angleUp = 0.75f;
		view.fov.angleDown = -0.75f;
	}
	desktop_last_tick_ = std::chrono::steady_clock::now();
	update_desktop_poses(0.0f);
	session_running_ = true;
	predicted_display_time_ = 1;
	running_ = true;
	std::cout << "HyperVR desktop: drag look, WASD move, Space/Ctrl up/down, Esc quit.\n";
}

void OpenXrContext::create_vulkan_standalone() {
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "HyperVR";
	app.applicationVersion = 1;
	app.pEngineName = "HyperVR";
	app.engineVersion = 1;
	app.apiVersion = VK_API_VERSION_1_3;

	std::vector<const char*> layers;
#ifndef NDEBUG
	layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

	VkInstanceCreateInfo inst_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	inst_info.pApplicationInfo = &app;
	inst_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
	inst_info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

	VkResult vk_result = vkCreateInstance(&inst_info, nullptr, &vk_instance_);
	if (vk_result != VK_SUCCESS) {
		throw std::runtime_error("Desktop mode: vkCreateInstance failed");
	}

	std::uint32_t phys_count = 0;
	vkEnumeratePhysicalDevices(vk_instance_, &phys_count, nullptr);
	if (phys_count == 0) {
		throw std::runtime_error("Desktop mode: no Vulkan physical devices");
	}
	std::vector<VkPhysicalDevice> physicals(phys_count);
	vkEnumeratePhysicalDevices(vk_instance_, &phys_count, physicals.data());

	// Prefer a discrete GPU with a graphics queue.
	vk_physical_ = VK_NULL_HANDLE;
	queue_family_ = 0;
	for (VkPhysicalDevice candidate : physicals) {
		std::uint32_t family_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
		std::vector<VkQueueFamilyProperties> families(family_count);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
		for (std::uint32_t i = 0; i < family_count; ++i) {
			if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
				continue;
			}
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(candidate, &props);
			vk_physical_ = candidate;
			queue_family_ = i;
			if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
				break;
			}
		}
		if (vk_physical_ != VK_NULL_HANDLE) {
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(vk_physical_, &props);
			if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
				break;
			}
		}
	}
	if (vk_physical_ == VK_NULL_HANDLE) {
		throw std::runtime_error("Desktop mode: no graphics queue family");
	}

	float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queue_info.queueFamilyIndex = queue_family_;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &queue_priority;

	VkPhysicalDeviceDynamicRenderingFeatures dyn_rendering{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
	dyn_rendering.dynamicRendering = VK_TRUE;

	const char* device_exts[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};

	VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	device_info.pNext = &dyn_rendering;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = 1;
	device_info.ppEnabledExtensionNames = device_exts;

	vk_result = vkCreateDevice(vk_physical_, &device_info, nullptr, &vk_device_);
	if (vk_result != VK_SUCCESS) {
		throw std::runtime_error("Desktop mode: vkCreateDevice failed");
	}
	owns_vulkan_ = true;
	vkGetDeviceQueue(vk_device_, queue_family_, 0, &vk_queue_);
}

XrPosef OpenXrContext::make_pose(float x, float y, float z, float yaw, float pitch) const {
	// Yaw (Y) then pitch (X); OpenXR looks down -Z when identity.
	const float cy = std::cos(yaw * 0.5f);
	const float sy = std::sin(yaw * 0.5f);
	const float cp = std::cos(pitch * 0.5f);
	const float sp = std::sin(pitch * 0.5f);

	XrPosef pose{};
	pose.orientation.x = sp * cy;
	pose.orientation.y = cp * sy;
	pose.orientation.z = -sp * sy;
	pose.orientation.w = cp * cy;
	pose.position.x = x;
	pose.position.y = y;
	pose.position.z = z;
	return pose;
}

Mat4 OpenXrContext::view_proj_from_pose(const XrPosef& pose, const XrFovf& fov) const {
	const Mat4 to_view = pose_to_matrix(pose);
	Mat4 view = Mat4::identity();
	view.m[0] = to_view.m[0];
	view.m[1] = to_view.m[4];
	view.m[2] = to_view.m[8];
	view.m[4] = to_view.m[1];
	view.m[5] = to_view.m[5];
	view.m[6] = to_view.m[9];
	view.m[8] = to_view.m[2];
	view.m[9] = to_view.m[6];
	view.m[10] = to_view.m[10];
	view.m[12] = -(view.m[0] * to_view.m[12] + view.m[4] * to_view.m[13] + view.m[8] * to_view.m[14]);
	view.m[13] = -(view.m[1] * to_view.m[12] + view.m[5] * to_view.m[13] + view.m[9] * to_view.m[14]);
	view.m[14] = -(view.m[2] * to_view.m[12] + view.m[6] * to_view.m[13] + view.m[10] * to_view.m[14]);
	return Mat4::multiply(projection_matrix(fov, 0.05f, 100.0f), view);
}

void OpenXrContext::desktop_add_scroll(float scroll_y) {
	if (!desktop_camera_locked_) {
		cam_scroll_pending_ += scroll_y;
	}
}

void OpenXrContext::set_desktop_camera(float x, float y, float z, float yaw, float pitch) {
	cam_x_ = x;
	cam_y_ = y;
	cam_z_ = z;
	cam_yaw_ = yaw;
	cam_pitch_ = pitch;
	desktop_camera_locked_ = true;
	cam_scroll_pending_ = 0.0f;
	orbit_camera_ = make_pose(cam_x_, cam_y_, cam_z_, cam_yaw_, cam_pitch_);
	views_[0].pose = orbit_camera_;
	views_[1].pose = orbit_camera_;
}

void OpenXrContext::unlock_desktop_camera() {
	desktop_camera_locked_ = false;
}

void OpenXrContext::update_desktop_poses(float dt) {
	const auto key = [](int vk) -> bool {
		return (GetAsyncKeyState(vk) & 0x8000) != 0;
	};

	if (key(VK_ESCAPE)) {
		running_ = false;
	}

	if (!desktop_camera_locked_) {
		POINT cursor{};
		GetCursorPos(&cursor);
		const bool dragging = key(VK_LBUTTON) || key(VK_RBUTTON);
		if (dragging) {
			if (cam_mouse_init_) {
				cam_yaw_ += static_cast<float>(cursor.x - cam_mouse_x_) * 0.009f;
				cam_pitch_ -= static_cast<float>(cursor.y - cam_mouse_y_) * 0.009f;
			}
			cam_mouse_x_ = cursor.x;
			cam_mouse_y_ = cursor.y;
			cam_mouse_init_ = true;
		} else {
			cam_mouse_init_ = false;
		}

		if (key(VK_LEFT)) {
			cam_yaw_ -= 1.6f * dt;
		}
		if (key(VK_RIGHT)) {
			cam_yaw_ += 1.6f * dt;
		}
		if (key(VK_UP)) {
			cam_pitch_ += 1.6f * dt;
		}
		if (key(VK_DOWN)) {
			cam_pitch_ -= 1.6f * dt;
		}

		constexpr float kPitchMax = 1.45f;
		if (cam_pitch_ > kPitchMax) {
			cam_pitch_ = kPitchMax;
		}
		if (cam_pitch_ < -kPitchMax) {
			cam_pitch_ = -kPitchMax;
		}

		const float cp = std::cos(cam_pitch_);
		const float sp = std::sin(cam_pitch_);
		const float cy = std::cos(cam_yaw_);
		const float sy = std::sin(cam_yaw_);
		const float fwd_x = -sy * cp;
		const float fwd_y = sp;
		const float fwd_z = -cy * cp;
		const float right_x = cy;
		const float right_z = -sy;

		constexpr float kMove = 4.5f;
		float mx = 0.0f;
		float my = 0.0f;
		float mz = 0.0f;
		if (key('W')) {
			mx += fwd_x;
			my += fwd_y;
			mz += fwd_z;
		}
		if (key('S')) {
			mx -= fwd_x;
			my -= fwd_y;
			mz -= fwd_z;
		}
		if (key('A')) {
			mx -= right_x;
			mz -= right_z;
		}
		if (key('D')) {
			mx += right_x;
			mz += right_z;
		}
		if (key(VK_SPACE)) {
			my += 1.0f;
		}
		if (key(VK_CONTROL)) {
			my -= 1.0f;
		}
		if (cam_scroll_pending_ != 0.0f) {
			mx += fwd_x * cam_scroll_pending_ * 0.7f;
			my += fwd_y * cam_scroll_pending_ * 0.7f;
			mz += fwd_z * cam_scroll_pending_ * 0.7f;
			cam_scroll_pending_ = 0.0f;
		}

		const float len = std::sqrt(mx * mx + my * my + mz * mz);
		if (len > 1.0e-4f) {
			const float scale = kMove * dt / len;
			cam_x_ += mx * scale;
			cam_y_ += my * scale;
			cam_z_ += mz * scale;
		}
	}

	orbit_camera_ = make_pose(cam_x_, cam_y_, cam_z_, cam_yaw_, cam_pitch_);

	for (XrView& view : views_) {
		view.fov.angleLeft = -0.95f;
		view.fov.angleRight = 0.95f;
		view.fov.angleUp = 0.70f;
		view.fov.angleDown = -0.70f;
	}
	views_[0].pose = orbit_camera_;
	views_[1].pose = orbit_camera_;

	// Fixed standing player for model inspection — arms hang forward like GT idle.
	constexpr float kHeadY = 1.55f;
	player_head_ = make_pose(0.0f, kHeadY, 0.0f, 0.0f, 0.0f);
	// Inspect grips: chest-high, out in front. Identity look (−Z) keeps fingers forward
	// before the palm hang rotation faces the mitten at the camera.
	// Hand-review friendly grips: centered, identity look so the mitt mesh is easy to orbit.
	// (Normal play still uses controller poses; desktop stubs are inspect poses.)
	// Zero yaw so both mitts face the camera the same way in hand review.
	grip_poses_[0] = make_pose(-0.22f, 1.20f, 0.0f, 0.0f, 0.0f);
	grip_poses_[1] = make_pose(0.22f, 1.20f, 0.0f, 0.0f, 0.0f);
	grip_valid_[0] = true;
	grip_valid_[1] = true;
	grip_value_[0] = grip_value_[1] = 0.0f;
	grip_down_[0] = grip_down_[1] = false;
	trigger_value_[0] = trigger_value_[1] = 0.0f;
	trigger_down_[0] = trigger_down_[1] = false;
	menu_down_[0] = menu_down_[1] = false;
	ax_down_[0] = ax_down_[1] = false;
	by_down_[0] = by_down_[1] = false;
}

void OpenXrContext::create_instance(const std::string& application_name) {
	std::uint32_t ext_count = 0;
	check_xr(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr), "enumerate ext count");
	std::vector<XrExtensionProperties> extensions(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
	check_xr(xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, extensions.data()), "enumerate ext");

	std::vector<const char*> enabled;
	if (has_extension(extensions, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
		enabled.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	} else {
		throw std::runtime_error("Runtime missing XR_KHR_vulkan_enable2");
	}

	XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strncpy(create.applicationInfo.applicationName, application_name.c_str(), XR_MAX_APPLICATION_NAME_SIZE - 1);
	create.applicationInfo.applicationVersion = 1;
	std::strncpy(create.applicationInfo.engineName, "HyperVR", XR_MAX_ENGINE_NAME_SIZE - 1);
	create.applicationInfo.engineVersion = 1;
	create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	create.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
	create.enabledExtensionNames = enabled.data();

	XrResult create_result = xrCreateInstance(&create, &instance_);
	if (XR_FAILED(create_result)) {
		throw std::runtime_error(
			std::string("OpenXR xrCreateInstance failed (") + std::to_string(static_cast<int>(create_result)) +
			"). Start SteamVR (or your OpenXR runtime) and ensure a headset is available.");
	}

	XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
	check_xr(xrGetInstanceProperties(instance_, &props), "xrGetInstanceProperties");
	runtime_name_ = props.runtimeName;

	check_xr(
		xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsRequirements2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfn_get_vk_reqs2_)),
		"get xrGetVulkanGraphicsRequirements2KHR");
	check_xr(
		xrGetInstanceProcAddr(instance_, "xrCreateVulkanInstanceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfn_create_vk_instance_)),
		"get xrCreateVulkanInstanceKHR");
	check_xr(
		xrGetInstanceProcAddr(instance_, "xrCreateVulkanDeviceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfn_create_vk_device_)),
		"get xrCreateVulkanDeviceKHR");

	XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	check_xr(xrGetSystem(instance_, &system_info, &system_id_), "xrGetSystem");
}

void OpenXrContext::create_vulkan_via_openxr() {
	XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	check_xr(pfn_get_vk_reqs2_(instance_, system_id_, &requirements), "xrGetVulkanGraphicsRequirements2KHR");
	(void)requirements;

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "HyperVR";
	app.applicationVersion = 1;
	app.pEngineName = "HyperVR";
	app.engineVersion = 1;
	app.apiVersion = VK_API_VERSION_1_3;

	std::vector<const char*> layers;
#ifndef NDEBUG
	layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

	VkInstanceCreateInfo inst_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	inst_info.pApplicationInfo = &app;
	inst_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
	inst_info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

	XrVulkanInstanceCreateInfoKHR xr_inst{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	xr_inst.systemId = system_id_;
	xr_inst.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_inst.vulkanCreateInfo = &inst_info;
	xr_inst.vulkanAllocator = nullptr;

	VkResult vk_result = VK_SUCCESS;
	check_xr(pfn_create_vk_instance_(instance_, &xr_inst, &vk_instance_, &vk_result), "xrCreateVulkanInstanceKHR");
	if (vk_result != VK_SUCCESS) {
		throw std::runtime_error("vkCreateInstance failed via OpenXR");
	}

	XrVulkanGraphicsDeviceGetInfoKHR device_get{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	device_get.systemId = system_id_;
	device_get.vulkanInstance = vk_instance_;

	PFN_xrGetVulkanGraphicsDevice2KHR get_device2 = nullptr;
	check_xr(
		xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&get_device2)),
		"xrGetVulkanGraphicsDevice2KHR");
	check_xr(get_device2(instance_, &device_get, &vk_physical_), "get graphics device");

	float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queue_info.queueFamilyIndex = 0;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &queue_priority;

	std::uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_, &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_, &family_count, families.data());
	queue_family_ = 0;
	for (std::uint32_t i = 0; i < family_count; ++i) {
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			queue_family_ = i;
			break;
		}
	}
	queue_info.queueFamilyIndex = queue_family_;

	VkPhysicalDeviceDynamicRenderingFeatures dyn_rendering{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
	dyn_rendering.dynamicRendering = VK_TRUE;

	const char* device_exts[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};

	VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	device_info.pNext = &dyn_rendering;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = 1;
	device_info.ppEnabledExtensionNames = device_exts;

	XrVulkanDeviceCreateInfoKHR xr_dev{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	xr_dev.systemId = system_id_;
	xr_dev.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_dev.vulkanPhysicalDevice = vk_physical_;
	xr_dev.vulkanCreateInfo = &device_info;
	xr_dev.vulkanAllocator = nullptr;

	vk_result = VK_SUCCESS;
	check_xr(pfn_create_vk_device_(instance_, &xr_dev, &vk_device_, &vk_result), "xrCreateVulkanDeviceKHR");
	if (vk_result != VK_SUCCESS) {
		throw std::runtime_error("vkCreateDevice failed via OpenXR");
	}
	owns_vulkan_ = true;
	vkGetDeviceQueue(vk_device_, queue_family_, 0, &vk_queue_);
}

VkFormat OpenXrContext::pick_swapchain_format() const {
	std::uint32_t format_count = 0;
	check_xr(xrEnumerateSwapchainFormats(session_, 0, &format_count, nullptr), "swapchain formats count");
	std::vector<std::int64_t> formats(format_count);
	check_xr(xrEnumerateSwapchainFormats(session_, format_count, &format_count, formats.data()), "swapchain formats");

	const std::int64_t preferred[] = {
		VK_FORMAT_R8G8B8A8_SRGB,
		VK_FORMAT_B8G8R8A8_SRGB,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8A8_UNORM,
	};
	std::int64_t chosen = formats.empty() ? VK_FORMAT_R8G8B8A8_SRGB : formats[0];
	for (std::int64_t pref : preferred) {
		if (std::find(formats.begin(), formats.end(), pref) != formats.end()) {
			chosen = pref;
			break;
		}
	}
	return static_cast<VkFormat>(chosen);
}

void OpenXrContext::create_session() {
	XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
	binding.instance = vk_instance_;
	binding.physicalDevice = vk_physical_;
	binding.device = vk_device_;
	binding.queueFamilyIndex = queue_family_;
	binding.queueIndex = 0;

	XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
	session_info.next = &binding;
	session_info.systemId = system_id_;
	check_xr(xrCreateSession(instance_, &session_info, &session_), "xrCreateSession");

	XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_info.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
	check_xr(xrCreateReferenceSpace(session_, &space_info, &app_space_), "xrCreateReferenceSpace");
}

void OpenXrContext::destroy_swapchains() {
	for (Swapchain& sc : swapchains_) {
		for (VkImageView view : sc.views) {
			if (view != VK_NULL_HANDLE && vk_device_ != VK_NULL_HANDLE) {
				vkDestroyImageView(vk_device_, view, nullptr);
			}
		}
		sc.views.clear();
		sc.images.clear();
		if (sc.handle != XR_NULL_HANDLE) {
			xrDestroySwapchain(sc.handle);
			sc.handle = XR_NULL_HANDLE;
		}
	}
}

void OpenXrContext::create_swapchains() {
	check_xr(
		xrEnumerateViewConfigurationViews(instance_, system_id_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count_, nullptr),
		"enumerate views count");
	if (view_count_ > config_views_.size()) {
		view_count_ = static_cast<std::uint32_t>(config_views_.size());
	}
	for (XrViewConfigurationView& v : config_views_) {
		v = XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW};
	}
	check_xr(
		xrEnumerateViewConfigurationViews(
			instance_, system_id_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count_, &view_count_, config_views_.data()),
		"enumerate views");

	const std::int64_t chosen = static_cast<std::int64_t>(color_format_);

	for (std::uint32_t i = 0; i < view_count_; ++i) {
		const XrViewConfigurationView& cfg = config_views_[i];
		XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		info.format = chosen;
		info.sampleCount = cfg.recommendedSwapchainSampleCount;
		info.width = cfg.recommendedImageRectWidth;
		info.height = cfg.recommendedImageRectHeight;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;

		Swapchain& sc = swapchains_[i];
		check_xr(xrCreateSwapchain(session_, &info, &sc.handle), "xrCreateSwapchain");
		sc.width = static_cast<std::int32_t>(info.width);
		sc.height = static_cast<std::int32_t>(info.height);
		sc.format = static_cast<VkFormat>(chosen);

		std::uint32_t image_count = 0;
		check_xr(xrEnumerateSwapchainImages(sc.handle, 0, &image_count, nullptr), "swapchain image count");
		sc.images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
		check_xr(
			xrEnumerateSwapchainImages(
				sc.handle, image_count, &image_count, reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())),
			"swapchain images");

		sc.views.resize(image_count);
		for (std::uint32_t ii = 0; ii < image_count; ++ii) {
			VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view_info.image = sc.images[ii].image;
			view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view_info.format = sc.format;
			view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			view_info.subresourceRange.levelCount = 1;
			view_info.subresourceRange.layerCount = 1;
			if (vkCreateImageView(vk_device_, &view_info, nullptr, &sc.views[ii]) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create swapchain image view");
			}
		}
	}

	for (XrView& view : views_) {
		view = XrView{XR_TYPE_VIEW};
	}
}

void OpenXrContext::create_actions() {
	XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strncpy(set_info.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	std::strncpy(set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
	check_xr(xrCreateActionSet(instance_, &set_info, &action_set_), "xrCreateActionSet");

	check_xr(xrStringToPath(instance_, "/user/hand/left", &hand_paths_[0]), "left path");
	check_xr(xrStringToPath(instance_, "/user/hand/right", &hand_paths_[1]), "right path");

	auto make_bool = [&](const char* name, const char* localized, XrAction* out) {
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
		std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		info.countSubactionPaths = 2;
		info.subactionPaths = hand_paths_.data();
		check_xr(xrCreateAction(action_set_, &info, out), name);
	};

	auto make_float = [&](const char* name, const char* localized, XrAction* out) {
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
		std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		info.countSubactionPaths = 2;
		info.subactionPaths = hand_paths_.data();
		check_xr(xrCreateAction(action_set_, &info, out), name);
	};

	XrActionCreateInfo pose_info{XR_TYPE_ACTION_CREATE_INFO};
	pose_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
	std::strncpy(pose_info.actionName, "grip_pose", XR_MAX_ACTION_NAME_SIZE - 1);
	std::strncpy(pose_info.localizedActionName, "Grip Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
	pose_info.countSubactionPaths = 2;
	pose_info.subactionPaths = hand_paths_.data();
	check_xr(xrCreateAction(action_set_, &pose_info, &grip_pose_action_), "grip_pose");

	make_float("trigger", "Trigger", &trigger_action_);
	make_float("grip", "Grip", &grip_action_);
	make_bool("menu", "Menu", &menu_action_);
	make_bool("ax_button", "A/X", &ax_action_);
	make_bool("by_button", "B/Y", &by_action_);

	for (int i = 0; i < 2; ++i) {
		XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		space_info.action = grip_pose_action_;
		space_info.subactionPath = hand_paths_[static_cast<std::size_t>(i)];
		space_info.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
		check_xr(xrCreateActionSpace(session_, &space_info, &grip_spaces_[static_cast<std::size_t>(i)]), "action space");
	}

	auto path = [&](const char* s) {
		XrPath out = XR_NULL_PATH;
		check_xr(xrStringToPath(instance_, s, &out), s);
		return out;
	};

	auto suggest = [&](const char* profile_name, const std::vector<XrActionSuggestedBinding>& bindings) {
		XrPath profile = path(profile_name);
		XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		suggested.interactionProfile = profile;
		suggested.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
		suggested.suggestedBindings = bindings.data();
		const XrResult result = xrSuggestInteractionProfileBindings(instance_, &suggested);
		if (XR_FAILED(result)) {
			std::cerr << "HyperVR: skipped bindings for " << profile_name << " (" << static_cast<int>(result) << ")\n";
		}
	};

	// Khronos simple controller: select → trigger (treated as 0/1 via float path where available).
	suggest("/interaction_profiles/khr/simple_controller", {
		{grip_pose_action_, path("/user/hand/left/input/grip/pose")},
		{grip_pose_action_, path("/user/hand/right/input/grip/pose")},
		{menu_action_, path("/user/hand/left/input/menu/click")},
		{menu_action_, path("/user/hand/right/input/menu/click")},
	});

	// Oculus Touch / Quest: top trigger, grip squeeze, A/X.
	suggest("/interaction_profiles/oculus/touch_controller", {
		{grip_pose_action_, path("/user/hand/left/input/grip/pose")},
		{grip_pose_action_, path("/user/hand/right/input/grip/pose")},
		{trigger_action_, path("/user/hand/left/input/trigger/value")},
		{trigger_action_, path("/user/hand/right/input/trigger/value")},
		{grip_action_, path("/user/hand/left/input/squeeze/value")},
		{grip_action_, path("/user/hand/right/input/squeeze/value")},
		{ax_action_, path("/user/hand/left/input/x/click")},
		{ax_action_, path("/user/hand/right/input/a/click")},
		{by_action_, path("/user/hand/left/input/y/click")},
		{by_action_, path("/user/hand/right/input/b/click")},
		{menu_action_, path("/user/hand/left/input/menu/click")},
	});

	// Valve Index.
	suggest("/interaction_profiles/valve/index_controller", {
		{grip_pose_action_, path("/user/hand/left/input/grip/pose")},
		{grip_pose_action_, path("/user/hand/right/input/grip/pose")},
		{trigger_action_, path("/user/hand/left/input/trigger/value")},
		{trigger_action_, path("/user/hand/right/input/trigger/value")},
		{grip_action_, path("/user/hand/left/input/squeeze/value")},
		{grip_action_, path("/user/hand/right/input/squeeze/value")},
		{ax_action_, path("/user/hand/left/input/a/click")},
		{ax_action_, path("/user/hand/right/input/a/click")},
		{by_action_, path("/user/hand/left/input/b/click")},
		{by_action_, path("/user/hand/right/input/b/click")},
	});

	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &action_set_;
	check_xr(xrAttachSessionActionSets(session_, &attach), "attach action sets");
}

void OpenXrContext::poll_events() {
	if (desktop_mode_) {
		return;
	}
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
		switch (event.type) {
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			running_ = false;
			break;
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
			session_state_ = changed->state;
			switch (session_state_) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				check_xr(xrBeginSession(session_, &begin), "xrBeginSession");
				session_running_ = true;
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				if (session_running_) {
					xrEndSession(session_);
					session_running_ = false;
				}
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				running_ = false;
				break;
			default:
				break;
			}
			break;
		}
		default:
			break;
		}
		event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
	}
}

void OpenXrContext::sync_actions() {
	if (!session_running_ || predicted_display_time_ == 0) {
		return;
	}
	XrActiveActionSet active{action_set_, XR_NULL_PATH};
	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (XR_FAILED(xrSyncActions(session_, &sync))) {
		return;
	}

	for (int hand = 0; hand < 2; ++hand) {
		const XrPath sub = hand_paths_[static_cast<std::size_t>(hand)];

		XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};
		XrActionStateGetInfo pose_info{XR_TYPE_ACTION_STATE_GET_INFO};
		pose_info.action = grip_pose_action_;
		pose_info.subactionPath = sub;
		xrGetActionStatePose(session_, &pose_info, &pose_state);
		grip_valid_[static_cast<std::size_t>(hand)] = pose_state.isActive;

		if (pose_state.isActive) {
			XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
			xrLocateSpace(grip_spaces_[static_cast<std::size_t>(hand)], app_space_, predicted_display_time_, &location);
			const bool ok = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
				(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
			grip_valid_[static_cast<std::size_t>(hand)] = ok;
			if (ok) {
				grip_poses_[static_cast<std::size_t>(hand)] = location.pose;
			}
		}

		auto read_bool = [&](XrAction action) -> bool {
			XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
			XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
			info.action = action;
			info.subactionPath = sub;
			xrGetActionStateBoolean(session_, &info, &state);
			return state.isActive && state.currentState;
		};

		auto read_float = [&](XrAction action) -> float {
			XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
			XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
			info.action = action;
			info.subactionPath = sub;
			xrGetActionStateFloat(session_, &info, &state);
			if (!state.isActive) {
				return 0.0f;
			}
			if (state.currentState < 0.0f) {
				return 0.0f;
			}
			if (state.currentState > 1.0f) {
				return 1.0f;
			}
			return state.currentState;
		};

		trigger_value_[static_cast<std::size_t>(hand)] = read_float(trigger_action_);
		grip_value_[static_cast<std::size_t>(hand)] = read_float(grip_action_);
		trigger_down_[static_cast<std::size_t>(hand)] = trigger_value_[static_cast<std::size_t>(hand)] > 0.35f;
		grip_down_[static_cast<std::size_t>(hand)] = grip_value_[static_cast<std::size_t>(hand)] > 0.35f;
		menu_down_[static_cast<std::size_t>(hand)] = read_bool(menu_action_);
		ax_down_[static_cast<std::size_t>(hand)] = read_bool(ax_action_);
		by_down_[static_cast<std::size_t>(hand)] = read_bool(by_action_);
	}
}

bool OpenXrContext::begin_frame(FrameTimings& timings) {
	frame_started_ = false;
	should_render_ = false;

	if (desktop_mode_) {
		const auto now = std::chrono::steady_clock::now();
		float dt = std::chrono::duration<float>(now - desktop_last_tick_).count();
		desktop_last_tick_ = now;
		if (dt < 0.0f || dt > 0.1f) {
			dt = 1.0f / 90.0f;
		}
		// Soft cap ~90 Hz so the mirror doesn't spin the CPU.
		if (dt < 1.0f / 90.0f) {
			const auto sleep_for = std::chrono::duration<float>(1.0f / 90.0f - dt);
			std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(sleep_for));
			desktop_last_tick_ = std::chrono::steady_clock::now();
			dt = 1.0f / 90.0f;
		}
		timings.wait_ms = dt * 1000.0f;
		update_desktop_poses(dt);
		predicted_display_time_ = 1;
		frame_started_ = true;
		should_render_ = false;  // No XR swapchains; mirror presents.
		return true;
	}

	if (!session_running_) {
		return false;
	}

	const auto t0 = std::chrono::steady_clock::now();
	XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frame_state{XR_TYPE_FRAME_STATE};
	check_xr(xrWaitFrame(session_, &wait, &frame_state), "xrWaitFrame");
	const auto t1 = std::chrono::steady_clock::now();
	timings.wait_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

	XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
	check_xr(xrBeginFrame(session_, &begin), "xrBeginFrame");
	frame_started_ = true;
	predicted_display_time_ = frame_state.predictedDisplayTime;
	should_render_ = frame_state.shouldRender;

	sync_actions();

	if (should_render_) {
		XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
		locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate.displayTime = predicted_display_time_;
		locate.space = app_space_;
		XrViewState view_state{XR_TYPE_VIEW_STATE};
		std::uint32_t count = view_count_;
		check_xr(xrLocateViews(session_, &locate, &view_state, count, &count, views_.data()), "xrLocateViews");
		view_count_ = count;
	}
	return true;
}

Mat4 OpenXrContext::pose_to_matrix(const XrPosef& pose) const {
	// Match Khronos `XrMatrix4x4f_CreateFromQuaternion` + translation (column-major, +Y up).
	const float x = pose.orientation.x;
	const float y = pose.orientation.y;
	const float z = pose.orientation.z;
	const float w = pose.orientation.w;
	const float x2 = x + x;
	const float y2 = y + y;
	const float z2 = z + z;
	const float xx2 = x * x2;
	const float yy2 = y * y2;
	const float zz2 = z * z2;
	const float yz2 = y * z2;
	const float wx2 = w * x2;
	const float xy2 = x * y2;
	const float wz2 = w * z2;
	const float xz2 = x * z2;
	const float wy2 = w * y2;

	Mat4 out{};
	out.m[0] = 1.0f - yy2 - zz2;
	out.m[1] = xy2 + wz2;
	out.m[2] = xz2 - wy2;
	out.m[3] = 0.0f;
	out.m[4] = xy2 - wz2;
	out.m[5] = 1.0f - xx2 - zz2;
	out.m[6] = yz2 + wx2;
	out.m[7] = 0.0f;
	out.m[8] = xz2 + wy2;
	out.m[9] = yz2 - wx2;
	out.m[10] = 1.0f - xx2 - yy2;
	out.m[11] = 0.0f;
	out.m[12] = pose.position.x;
	out.m[13] = pose.position.y;
	out.m[14] = pose.position.z;
	out.m[15] = 1.0f;
	return out;
}

Mat4 OpenXrContext::projection_matrix(const XrFovf& fov, float near_z, float far_z) const {
	// Match Khronos OpenXR-SDK `xr_linear.h` XrMatrix4x4f_CreateProjection(..., GRAPHICS_VULKAN, ...).
	// OpenXR world/view space is +Y up; Vulkan clip space is +Y down, so height uses (down - up).
	const float tan_left = std::tan(fov.angleLeft);
	const float tan_right = std::tan(fov.angleRight);
	const float tan_down = std::tan(fov.angleDown);
	const float tan_up = std::tan(fov.angleUp);

	const float tan_width = tan_right - tan_left;
	const float tan_height = tan_down - tan_up;  // Vulkan (+Y down clip space)
	const float offset_z = 0.0f;                 // Vulkan [0,1] Z

	Mat4 out{};
	out.m[0] = 2.0f / tan_width;
	out.m[5] = 2.0f / tan_height;
	out.m[8] = (tan_right + tan_left) / tan_width;
	out.m[9] = (tan_up + tan_down) / tan_height;
	out.m[10] = -(far_z + offset_z) / (far_z - near_z);
	out.m[11] = -1.0f;
	out.m[14] = -(far_z * (near_z + offset_z)) / (far_z - near_z);
	out.m[15] = 0.0f;
	return out;
}

void OpenXrContext::end_frame(VulkanRenderer& renderer, const std::vector<DrawItem>& draws, FrameTimings& timings) {
	if (!frame_started_) {
		return;
	}

	if (desktop_mode_) {
		(void)renderer;
		(void)draws;
		timings.cpu_record_ms = 0.0f;
		timings.submit_ms = 0.0f;
		timings.total_ms = timings.wait_ms;
		frame_started_ = false;
		return;
	}

	std::vector<XrCompositionLayerProjectionView> projection_views;
	XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	const XrCompositionLayerBaseHeader* layers[1] = {};
	std::uint32_t layer_count = 0;

	const auto t0 = std::chrono::steady_clock::now();

	if (should_render_) {
		VkCommandBuffer cmd = renderer.begin_one_time_commands();
		projection_views.resize(view_count_);

		for (std::uint32_t eye = 0; eye < view_count_; ++eye) {
			Swapchain& sc = swapchains_[eye];
			XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			std::uint32_t image_index = 0;
			check_xr(xrAcquireSwapchainImage(sc.handle, &acquire, &image_index), "acquire");

			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = XR_INFINITE_DURATION;
			check_xr(xrWaitSwapchainImage(sc.handle, &wait), "wait swapchain");

			const Mat4 to_view = pose_to_matrix(views_[eye].pose);
			// View matrix = inverse of camera pose (rigid-body inverse).
			Mat4 view = Mat4::identity();
			view.m[0] = to_view.m[0];
			view.m[1] = to_view.m[4];
			view.m[2] = to_view.m[8];
			view.m[4] = to_view.m[1];
			view.m[5] = to_view.m[5];
			view.m[6] = to_view.m[9];
			view.m[8] = to_view.m[2];
			view.m[9] = to_view.m[6];
			view.m[10] = to_view.m[10];
			view.m[12] = -(view.m[0] * to_view.m[12] + view.m[4] * to_view.m[13] + view.m[8] * to_view.m[14]);
			view.m[13] = -(view.m[1] * to_view.m[12] + view.m[5] * to_view.m[13] + view.m[9] * to_view.m[14]);
			view.m[14] = -(view.m[2] * to_view.m[12] + view.m[6] * to_view.m[13] + view.m[10] * to_view.m[14]);

			const Mat4 proj = projection_matrix(views_[eye].fov, 0.05f, 100.0f);
			const Mat4 view_proj = Mat4::multiply(proj, view);

			renderer.record_eye(
				cmd,
				sc.images[image_index].image,
				sc.views[image_index],
				static_cast<std::uint32_t>(sc.width),
				static_cast<std::uint32_t>(sc.height),
				view_proj,
				draws);

			XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			check_xr(xrReleaseSwapchainImage(sc.handle, &release), "release");

			projection_views[eye] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
			projection_views[eye].pose = views_[eye].pose;
			projection_views[eye].fov = views_[eye].fov;
			projection_views[eye].subImage.swapchain = sc.handle;
			projection_views[eye].subImage.imageRect.offset = {0, 0};
			projection_views[eye].subImage.imageRect.extent = {sc.width, sc.height};
		}

		renderer.end_one_time_commands(cmd);

		layer.space = app_space_;
		layer.viewCount = view_count_;
		layer.views = projection_views.data();
		layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
		layer_count = 1;
	}

	const auto t1 = std::chrono::steady_clock::now();
	timings.cpu_record_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

	const auto t2 = std::chrono::steady_clock::now();
	XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
	end.displayTime = predicted_display_time_;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = layer_count;
	end.layers = layer_count ? layers : nullptr;
	check_xr(xrEndFrame(session_, &end), "xrEndFrame");
	const auto t3 = std::chrono::steady_clock::now();
	timings.submit_ms = std::chrono::duration<float, std::milli>(t3 - t2).count();
	timings.total_ms = timings.wait_ms + timings.cpu_record_ms + timings.submit_ms;

	frame_started_ = false;
}

bool OpenXrContext::controller_pose(Hand hand, Mat4& out_pose) const {
	const int idx = static_cast<int>(hand);
	if (idx < 0 || idx > 1 || !grip_valid_[static_cast<std::size_t>(idx)]) {
		return false;
	}
	out_pose = pose_to_matrix(grip_poses_[static_cast<std::size_t>(idx)]);
	return true;
}

bool OpenXrContext::head_pose(Mat4& out_pose) const {
	if (desktop_mode_) {
		out_pose = pose_to_matrix(player_head_);
		return true;
	}
	if (view_count_ < 1 || predicted_display_time_ == 0) {
		return false;
	}
	out_pose = pose_to_matrix(views_[0].pose);
	if (view_count_ >= 2) {
		const Mat4 right = pose_to_matrix(views_[1].pose);
		out_pose.m[12] = 0.5f * (out_pose.m[12] + right.m[12]);
		out_pose.m[13] = 0.5f * (out_pose.m[13] + right.m[13]);
		out_pose.m[14] = 0.5f * (out_pose.m[14] + right.m[14]);
	}
	return true;
}

bool OpenXrContext::left_eye_view_proj(Mat4& out_view_proj) const {
	if (view_count_ < 1 || predicted_display_time_ == 0) {
		return false;
	}
	if (desktop_mode_) {
		out_view_proj = view_proj_from_pose(orbit_camera_, views_[0].fov);
		return true;
	}
	out_view_proj = view_proj_from_pose(views_[0].pose, views_[0].fov);
	return true;
}

float OpenXrContext::trigger_value(Hand hand) const {
	const int idx = static_cast<int>(hand);
	if (idx < 0 || idx > 1) {
		return 0.0f;
	}
	return trigger_value_[static_cast<std::size_t>(idx)];
}

float OpenXrContext::grip_value(Hand hand) const {
	const int idx = static_cast<int>(hand);
	if (idx < 0 || idx > 1) {
		return 0.0f;
	}
	return grip_value_[static_cast<std::size_t>(idx)];
}

bool OpenXrContext::button_down(Hand hand, ControllerButton button) const {
	const int idx = static_cast<int>(hand);
	if (idx < 0 || idx > 1) {
		return false;
	}
	switch (button) {
	case ControllerButton::Trigger:
		return trigger_down_[static_cast<std::size_t>(idx)];
	case ControllerButton::Grip:
		return grip_down_[static_cast<std::size_t>(idx)];
	case ControllerButton::Menu:
		return menu_down_[static_cast<std::size_t>(idx)];
	case ControllerButton::A_X:
		return ax_down_[static_cast<std::size_t>(idx)];
	case ControllerButton::B_Y:
		return by_down_[static_cast<std::size_t>(idx)];
	}
	const int missing = static_cast<int>(button);
	(void)missing;
	return false;
}

}  // namespace hypervr
