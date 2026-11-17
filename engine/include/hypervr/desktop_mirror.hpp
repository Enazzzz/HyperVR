#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "hypervr/types.hpp"

namespace hypervr {

class VulkanRenderer;

/**
 * Simple Win32 desktop mirror of the left-eye view (debug / spectator).
 *
 * Re-renders the scene into a small offscreen image, copies it to the CPU,
 * and StretchDIBits into a titled window. Not a zero-copy compositor path —
 * intentionally simple.
 */
class DesktopMirror {
public:
	DesktopMirror() = default;
	~DesktopMirror();

	DesktopMirror(const DesktopMirror&) = delete;
	DesktopMirror& operator=(const DesktopMirror&) = delete;

	/**
	 * Create the Win32 window + GPU offscreen/staging resources.
	 */
	void initialize(VulkanRenderer& renderer, VkFormat color_format, std::uint32_t width = 960, std::uint32_t height = 540);

	/**
	 * Destroy window and GPU objects (safe if never initialized).
	 */
	void shutdown();

	/**
	 * Pump Win32 messages; returns false if the user closed the mirror window.
	 */
	bool poll();

	/**
	 * Accumulated mouse-wheel zoom since last take (positive = zoom in).
	 */
	float take_scroll_y();

	/**
	 * Called from the window proc when the wheel moves.
	 */
	void add_scroll_y(float notches) { scroll_y_ += notches; }

	/**
	 * Render ``draws`` with ``view_proj`` into the mirror and blit to the window.
	 *
	 * ``fps`` is drawn as a simple GDI overlay (pass <= 0 to hide).
	 */
	void present(VulkanRenderer& renderer, const Mat4& view_proj, const std::vector<DrawItem>& draws, float fps = 0.0f);

	/**
	 * Write the last presented frame as a Windows BMP (32-bit BGRA).
	 */
	bool save_screenshot_bmp(const std::string& path) const;

	bool alive() const { return hwnd_ != nullptr; }

private:
	std::uint32_t find_memory_type(VkPhysicalDevice physical, std::uint32_t type_bits, VkMemoryPropertyFlags flags) const;
	void create_gpu_resources(VulkanRenderer& renderer, VkFormat color_format);
	void destroy_gpu_resources();
	void blit_to_window(float fps);

	HWND hwnd_ = nullptr;
	HDC window_dc_ = nullptr;
	HDC mem_dc_ = nullptr;
	HBITMAP dib_ = nullptr;
	void* dib_bits_ = nullptr;
	BITMAPINFO bmi_{};

	std::uint32_t width_ = 960;
	std::uint32_t height_ = 540;
	VkFormat color_format_ = VK_FORMAT_B8G8R8A8_SRGB;
	bool swizzle_rb_ = false;
	float scroll_y_ = 0.0f;

	VkDevice device_ = VK_NULL_HANDLE;
	VkPhysicalDevice physical_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	std::uint32_t queue_family_ = 0;
	VkCommandPool command_pool_ = VK_NULL_HANDLE;

	VkImage color_image_ = VK_NULL_HANDLE;
	VkDeviceMemory color_memory_ = VK_NULL_HANDLE;
	VkImageView color_view_ = VK_NULL_HANDLE;
	VkBuffer staging_ = VK_NULL_HANDLE;
	VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
	void* staging_mapped_ = nullptr;
	VkDeviceSize staging_size_ = 0;
};

}  // namespace hypervr
