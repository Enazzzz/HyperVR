#include "hypervr/vulkan_renderer.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace hypervr {
namespace {

/**
 * Push-constant block matching mesh.vert.
 */
struct PushConstants {
	float mvp[16];
	float tint[4];
};

/**
 * Throw if a Vulkan result is not VK_SUCCESS.
 */
void check_vk(VkResult result, const char* what) {
	if (result != VK_SUCCESS) {
		throw std::runtime_error(std::string("Vulkan error in ") + what + " (" + std::to_string(static_cast<int>(result)) + ")");
	}
}

}  // namespace

VulkanRenderer::~VulkanRenderer() {
	shutdown();
}

void VulkanRenderer::shutdown() {
	if (device_ == VK_NULL_HANDLE) {
		return;
	}

	vkDeviceWaitIdle(device_);
	destroy_depth();

	for (GpuMesh& mesh : meshes_) {
		if (mesh.vertex_buffer) {
			vkDestroyBuffer(device_, mesh.vertex_buffer, nullptr);
		}
		if (mesh.vertex_memory) {
			vkFreeMemory(device_, mesh.vertex_memory, nullptr);
		}
		if (mesh.index_buffer) {
			vkDestroyBuffer(device_, mesh.index_buffer, nullptr);
		}
		if (mesh.index_memory) {
			vkFreeMemory(device_, mesh.index_memory, nullptr);
		}
	}
	meshes_.clear();

	if (pipeline_) {
		vkDestroyPipeline(device_, pipeline_, nullptr);
		pipeline_ = VK_NULL_HANDLE;
	}
	if (pipeline_layout_) {
		vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
		pipeline_layout_ = VK_NULL_HANDLE;
	}
	if (vert_module_) {
		vkDestroyShaderModule(device_, vert_module_, nullptr);
		vert_module_ = VK_NULL_HANDLE;
	}
	if (frag_module_) {
		vkDestroyShaderModule(device_, frag_module_, nullptr);
		frag_module_ = VK_NULL_HANDLE;
	}
	if (command_pool_) {
		vkDestroyCommandPool(device_, command_pool_, nullptr);
		command_pool_ = VK_NULL_HANDLE;
	}

	device_ = VK_NULL_HANDLE;
	physical_device_ = VK_NULL_HANDLE;
	instance_ = VK_NULL_HANDLE;
	queue_ = VK_NULL_HANDLE;
}

void VulkanRenderer::initialize(
	VkInstance instance,
	VkPhysicalDevice physical_device,
	VkDevice device,
	std::uint32_t queue_family_index,
	VkQueue queue,
	VkFormat color_format,
	const std::string& shader_directory) {
	instance_ = instance;
	physical_device_ = physical_device;
	device_ = device;
	queue_family_index_ = queue_family_index;
	queue_ = queue;
	shader_directory_ = shader_directory;

	VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family_index_;
	check_vk(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_), "vkCreateCommandPool");

	create_pipeline(color_format);
}

std::vector<std::uint32_t> VulkanRenderer::load_spirv(const std::string& path) const {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		throw std::runtime_error("Failed to open shader: " + path);
	}
	const std::streamsize size = file.tellg();
	if (size <= 0 || (size % 4) != 0) {
		throw std::runtime_error("Invalid SPIR-V size: " + path);
	}
	file.seekg(0);
	std::vector<std::uint32_t> words(static_cast<std::size_t>(size) / 4);
	file.read(reinterpret_cast<char*>(words.data()), size);
	return words;
}

void VulkanRenderer::create_pipeline(VkFormat color_format) {
	const auto vert_spirv = load_spirv(shader_directory_ + "/mesh.vert.spv");
	const auto frag_spirv = load_spirv(shader_directory_ + "/mesh.frag.spv");

	VkShaderModuleCreateInfo vert_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	vert_info.codeSize = vert_spirv.size() * sizeof(std::uint32_t);
	vert_info.pCode = vert_spirv.data();
	check_vk(vkCreateShaderModule(device_, &vert_info, nullptr, &vert_module_), "vert module");

	VkShaderModuleCreateInfo frag_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	frag_info.codeSize = frag_spirv.size() * sizeof(std::uint32_t);
	frag_info.pCode = frag_spirv.data();
	check_vk(vkCreateShaderModule(device_, &frag_info, nullptr, &frag_module_), "frag module");

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert_module_;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag_module_;
	stages[1].pName = "main";

	VkVertexInputBindingDescription binding{};
	binding.binding = 0;
	binding.stride = sizeof(Vertex);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attrs[2]{};
	attrs[0].location = 0;
	attrs[0].binding = 0;
	attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
	attrs[0].offset = offsetof(Vertex, px);
	attrs[1].location = 1;
	attrs[1].binding = 0;
	attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
	attrs[1].offset = offsetof(Vertex, r);

	VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &binding;
	vertex_input.vertexAttributeDescriptionCount = 2;
	vertex_input.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	viewport_state.viewportCount = 1;
	viewport_state.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	// Vulkan FOV projection flips clip-space Y (Khronos xr_linear), which reverses winding.
	raster.cullMode = VK_CULL_MODE_BACK_BIT;
	raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
	raster.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	depth.depthTestEnable = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS;

	VkPipelineColorBlendAttachmentState blend_attachment{};
	blend_attachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;

	VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;

	VkPushConstantRange push_range{};
	push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push_range.offset = 0;
	push_range.size = sizeof(PushConstants);

	VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	check_vk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_), "pipeline layout");

	VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &color_format;
	rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

	VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pipeline_info.pNext = &rendering;
	pipeline_info.stageCount = 2;
	pipeline_info.pStages = stages;
	pipeline_info.pVertexInputState = &vertex_input;
	pipeline_info.pInputAssemblyState = &input_assembly;
	pipeline_info.pViewportState = &viewport_state;
	pipeline_info.pRasterizationState = &raster;
	pipeline_info.pMultisampleState = &multisample;
	pipeline_info.pDepthStencilState = &depth;
	pipeline_info.pColorBlendState = &blend;
	pipeline_info.pDynamicState = &dynamic;
	pipeline_info.layout = pipeline_layout_;
	check_vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_), "pipeline");
}

std::uint32_t VulkanRenderer::find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const {
	VkPhysicalDeviceMemoryProperties props{};
	vkGetPhysicalDeviceMemoryProperties(physical_device_, &props);
	for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
		if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
			return i;
		}
	}
	throw std::runtime_error("No suitable Vulkan memory type");
}

void VulkanRenderer::create_buffer(
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags memory_flags,
	VkBuffer& buffer,
	VkDeviceMemory& memory) {
	VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	check_vk(vkCreateBuffer(device_, &info, nullptr, &buffer), "vkCreateBuffer");

	VkMemoryRequirements reqs{};
	vkGetBufferMemoryRequirements(device_, buffer, &reqs);

	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = reqs.size;
	alloc.memoryTypeIndex = find_memory_type(reqs.memoryTypeBits, memory_flags);
	check_vk(vkAllocateMemory(device_, &alloc, nullptr, &memory), "vkAllocateMemory");
	check_vk(vkBindBufferMemory(device_, buffer, memory, 0), "vkBindBufferMemory");
}

void VulkanRenderer::upload_buffer(VkBuffer dst, const void* data, VkDeviceSize size) {
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_memory = VK_NULL_HANDLE;
	create_buffer(
		size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		staging,
		staging_memory);

	void* mapped = nullptr;
	check_vk(vkMapMemory(device_, staging_memory, 0, size, 0, &mapped), "vkMapMemory");
	std::memcpy(mapped, data, static_cast<std::size_t>(size));
	vkUnmapMemory(device_, staging_memory);

	VkCommandBuffer cmd = begin_one_time_commands();
	VkBufferCopy copy{};
	copy.size = size;
	vkCmdCopyBuffer(cmd, staging, dst, 1, &copy);
	end_one_time_commands(cmd);

	vkDestroyBuffer(device_, staging, nullptr);
	vkFreeMemory(device_, staging_memory, nullptr);
}

VkCommandBuffer VulkanRenderer::begin_one_time_commands() {
	VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	alloc.commandPool = command_pool_;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	check_vk(vkAllocateCommandBuffers(device_, &alloc, &cmd), "allocate cmd");

	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	check_vk(vkBeginCommandBuffer(cmd, &begin), "begin cmd");
	return cmd;
}

void VulkanRenderer::end_one_time_commands(VkCommandBuffer cmd) {
	check_vk(vkEndCommandBuffer(cmd), "end cmd");
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	check_vk(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "submit");
	check_vk(vkQueueWaitIdle(queue_), "wait idle");
	vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
}

MeshId VulkanRenderer::create_mesh(
	const Vertex* vertices,
	std::size_t vertex_count,
	const std::uint32_t* indices,
	std::size_t index_count) {
	GpuMesh mesh{};
	const VkDeviceSize vbytes = sizeof(Vertex) * vertex_count;
	const VkDeviceSize ibytes = sizeof(std::uint32_t) * index_count;

	create_buffer(
		vbytes,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		mesh.vertex_buffer,
		mesh.vertex_memory);
	upload_buffer(mesh.vertex_buffer, vertices, vbytes);

	create_buffer(
		ibytes,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		mesh.index_buffer,
		mesh.index_memory);
	upload_buffer(mesh.index_buffer, indices, ibytes);
	mesh.index_count = static_cast<std::uint32_t>(index_count);

	meshes_.push_back(mesh);
	return static_cast<MeshId>(meshes_.size() - 1);
}

void VulkanRenderer::destroy_depth() {
	if (depth_view_) {
		vkDestroyImageView(device_, depth_view_, nullptr);
		depth_view_ = VK_NULL_HANDLE;
	}
	if (depth_image_) {
		vkDestroyImage(device_, depth_image_, nullptr);
		depth_image_ = VK_NULL_HANDLE;
	}
	if (depth_memory_) {
		vkFreeMemory(device_, depth_memory_, nullptr);
		depth_memory_ = VK_NULL_HANDLE;
	}
	depth_width_ = 0;
	depth_height_ = 0;
}

void VulkanRenderer::ensure_depth(std::uint32_t width, std::uint32_t height) {
	if (depth_image_ && depth_width_ == width && depth_height_ == height) {
		return;
	}
	destroy_depth();

	VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_D32_SFLOAT;
	image_info.extent = {width, height, 1};
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	check_vk(vkCreateImage(device_, &image_info, nullptr, &depth_image_), "depth image");

	VkMemoryRequirements reqs{};
	vkGetImageMemoryRequirements(device_, depth_image_, &reqs);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = reqs.size;
	alloc.memoryTypeIndex = find_memory_type(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check_vk(vkAllocateMemory(device_, &alloc, nullptr, &depth_memory_), "depth memory");
	check_vk(vkBindImageMemory(device_, depth_image_, depth_memory_, 0), "bind depth");

	VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	view_info.image = depth_image_;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = VK_FORMAT_D32_SFLOAT;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	check_vk(vkCreateImageView(device_, &view_info, nullptr, &depth_view_), "depth view");

	depth_width_ = width;
	depth_height_ = height;
}

void VulkanRenderer::record_eye(
	VkCommandBuffer cmd,
	VkImage color_image,
	VkImageView color_view,
	std::uint32_t width,
	std::uint32_t height,
	const Mat4& view_proj,
	const std::vector<DrawItem>& draws) {
	ensure_depth(width, height);

	// Color: UNDEFINED -> COLOR_ATTACHMENT
	VkImageMemoryBarrier color_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	color_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	color_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	color_barrier.image = color_image;
	color_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	color_barrier.subresourceRange.levelCount = 1;
	color_barrier.subresourceRange.layerCount = 1;
	color_barrier.srcAccessMask = 0;
	color_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	// First use: transition from UNDEFINED
	VkImageMemoryBarrier color_init = color_barrier;
	color_init.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	color_init.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkImageMemoryBarrier depth_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	depth_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depth_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
	depth_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depth_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depth_barrier.image = depth_image_;
	depth_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depth_barrier.subresourceRange.levelCount = 1;
	depth_barrier.subresourceRange.layerCount = 1;
	depth_barrier.srcAccessMask = 0;
	depth_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	VkImageMemoryBarrier barriers[2] = {color_init, depth_barrier};
	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		2,
		barriers);

	VkRenderingAttachmentInfo color_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	color_att.imageView = color_view;
	color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	color_att.clearValue.color = {{0.02f, 0.02f, 0.05f, 1.0f}};

	VkRenderingAttachmentInfo depth_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	depth_att.imageView = depth_view_;
	depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
	depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depth_att.clearValue.depthStencil = {1.0f, 0};

	VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
	rendering.renderArea.extent = {width, height};
	rendering.layerCount = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments = &color_att;
	rendering.pDepthAttachment = &depth_att;

	vkCmdBeginRendering(cmd, &rendering);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

	VkViewport viewport{};
	viewport.width = static_cast<float>(width);
	viewport.height = static_cast<float>(height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.extent = {width, height};
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	for (const DrawItem& item : draws) {
		if (item.mesh < 0 || static_cast<std::size_t>(item.mesh) >= meshes_.size()) {
			continue;
		}
		const GpuMesh& mesh = meshes_[static_cast<std::size_t>(item.mesh)];
		const Mat4 mvp = Mat4::multiply(view_proj, item.model);

		PushConstants pc{};
		std::memcpy(pc.mvp, mvp.m.data(), sizeof(pc.mvp));
		pc.tint[0] = item.r;
		pc.tint[1] = item.g;
		pc.tint[2] = item.b;
		pc.tint[3] = item.a;

		vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertex_buffer, &offset);
		vkCmdBindIndexBuffer(cmd, mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(cmd, mesh.index_count, 1, 0, 0, 0);
	}

	vkCmdEndRendering(cmd);

	// Transition color to COLOR_ATTACHMENT_OPTIMAL expected by OpenXR composition (SHADER_READ often used).
	VkImageMemoryBarrier to_sample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	to_sample.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	to_sample.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	to_sample.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_sample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_sample.image = color_image;
	to_sample.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	to_sample.subresourceRange.levelCount = 1;
	to_sample.subresourceRange.layerCount = 1;
	to_sample.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	to_sample.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		1,
		&to_sample);
}

}  // namespace hypervr
