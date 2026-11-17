#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "hypervr/types.hpp"

namespace hypervr {

/**
 * GPU mesh buffers owned by the Vulkan renderer.
 */
struct GpuMesh {
	VkBuffer vertex_buffer = VK_NULL_HANDLE;
	VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
	VkBuffer index_buffer = VK_NULL_HANDLE;
	VkDeviceMemory index_memory = VK_NULL_HANDLE;
	std::uint32_t index_count = 0;
};

/**
 * Owns Vulkan device objects used for stereo mesh rendering.
 */
class VulkanRenderer {
public:
	VulkanRenderer() = default;
	~VulkanRenderer();

	VulkanRenderer(const VulkanRenderer&) = delete;
	VulkanRenderer& operator=(const VulkanRenderer&) = delete;

	/**
	 * Create logical device objects from an already-created Vulkan device.
	 */
	void initialize(
		VkInstance instance,
		VkPhysicalDevice physical_device,
		VkDevice device,
		std::uint32_t queue_family_index,
		VkQueue queue,
		VkFormat color_format,
		const std::string& shader_directory);

	/**
	 * Destroy GPU objects while the Vulkan device is still alive.
	 */
	void shutdown();

	/**
	 * Upload a mesh; returns local mesh index (0-based).
	 */
	MeshId create_mesh(const Vertex* vertices, std::size_t vertex_count, const std::uint32_t* indices, std::size_t index_count);

	/**
	 * Ensure a depth image exists for the given extent (recreated on resize).
	 */
	void ensure_depth(std::uint32_t width, std::uint32_t height);

	/**
	 * Record draws into an eye color image (with depth).
	 */
	void record_eye(
		VkCommandBuffer cmd,
		VkImage color_image,
		VkImageView color_view,
		std::uint32_t width,
		std::uint32_t height,
		const Mat4& view_proj,
		const std::vector<DrawItem>& draws);

	VkDevice device() const { return device_; }
	VkQueue queue() const { return queue_; }
	std::uint32_t queue_family_index() const { return queue_family_index_; }
	VkCommandPool command_pool() const { return command_pool_; }
	VkPhysicalDevice physical_device() const { return physical_device_; }
	VkInstance instance() const { return instance_; }

	/**
	 * Create a transient command buffer, begin it, and return it.
	 */
	VkCommandBuffer begin_one_time_commands();

	/**
	 * End, submit, and free a one-time command buffer.
	 */
	void end_one_time_commands(VkCommandBuffer cmd);

private:
	void create_pipeline(VkFormat color_format);
	void destroy_depth();
	std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const;
	void create_buffer(
		VkDeviceSize size,
		VkBufferUsageFlags usage,
		VkMemoryPropertyFlags memory_flags,
		VkBuffer& buffer,
		VkDeviceMemory& memory);
	void upload_buffer(VkBuffer dst, const void* data, VkDeviceSize size);
	std::vector<std::uint32_t> load_spirv(const std::string& path) const;

	VkInstance instance_ = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	std::uint32_t queue_family_index_ = 0;
	VkCommandPool command_pool_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	VkShaderModule vert_module_ = VK_NULL_HANDLE;
	VkShaderModule frag_module_ = VK_NULL_HANDLE;

	VkImage depth_image_ = VK_NULL_HANDLE;
	VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
	VkImageView depth_view_ = VK_NULL_HANDLE;
	std::uint32_t depth_width_ = 0;
	std::uint32_t depth_height_ = 0;

	std::vector<GpuMesh> meshes_;
	std::string shader_directory_;
};

}  // namespace hypervr
