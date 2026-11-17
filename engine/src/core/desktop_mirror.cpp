#include "hypervr/desktop_mirror.hpp"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "hypervr/vulkan_renderer.hpp"

namespace hypervr {
namespace {

constexpr const wchar_t* kMirrorClass = L"HyperVRDesktopMirror";

/**
 * Throw if a Vulkan result is not VK_SUCCESS.
 */
void check_vk(VkResult result, const char* what) {
	if (result != VK_SUCCESS) {
		throw std::runtime_error(std::string("DesktopMirror Vulkan error in ") + what);
	}
}

/**
 * Win32 WndProc — close, wheel zoom, and paint.
 */
LRESULT CALLBACK mirror_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	auto* self = reinterpret_cast<DesktopMirror*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
	if (msg == WM_CLOSE) {
		DestroyWindow(hwnd);
		return 0;
	}
	if (msg == WM_DESTROY) {
		return 0;
	}
	if (msg == WM_MOUSEWHEEL && self) {
		const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
		// Positive wheel = zoom in (closer).
		self->add_scroll_y(static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA));
		return 0;
	}
	if (msg == WM_PAINT) {
		PAINTSTRUCT ps{};
		BeginPaint(hwnd, &ps);
		EndPaint(hwnd, &ps);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/**
 * Register the mirror window class once per process.
 */
void ensure_window_class() {
	static bool registered = false;
	if (registered) {
		return;
	}
	WNDCLASSW wc{};
	wc.lpfnWndProc = mirror_wnd_proc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = kMirrorClass;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		throw std::runtime_error("DesktopMirror: RegisterClassW failed");
	}
	registered = true;
}

}  // namespace

DesktopMirror::~DesktopMirror() {
	shutdown();
}

void DesktopMirror::shutdown() {
	destroy_gpu_resources();
	if (dib_) {
		DeleteObject(dib_);
		dib_ = nullptr;
		dib_bits_ = nullptr;
	}
	if (mem_dc_) {
		DeleteDC(mem_dc_);
		mem_dc_ = nullptr;
	}
	if (window_dc_ && hwnd_) {
		ReleaseDC(hwnd_, window_dc_);
		window_dc_ = nullptr;
	}
	if (hwnd_) {
		DestroyWindow(hwnd_);
		hwnd_ = nullptr;
	}
	scroll_y_ = 0.0f;
}

std::uint32_t DesktopMirror::find_memory_type(
	VkPhysicalDevice physical,
	std::uint32_t type_bits,
	VkMemoryPropertyFlags flags) const {
	VkPhysicalDeviceMemoryProperties props{};
	vkGetPhysicalDeviceMemoryProperties(physical, &props);
	for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
		if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
			return i;
		}
	}
	throw std::runtime_error("DesktopMirror: no matching memory type");
}

void DesktopMirror::initialize(VulkanRenderer& renderer, VkFormat color_format, std::uint32_t width, std::uint32_t height) {
	shutdown();
	width_ = width < 160 ? 160 : width;
	height_ = height < 90 ? 90 : height;
	color_format_ = color_format;
	swizzle_rb_ = (color_format == VK_FORMAT_R8G8B8A8_UNORM || color_format == VK_FORMAT_R8G8B8A8_SRGB);

	ensure_window_class();
	hwnd_ = CreateWindowExW(
		0,
		kMirrorClass,
		L"HyperVR Mirror",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		80,
		80,
		static_cast<int>(width_) + 16,
		static_cast<int>(height_) + 39,
		nullptr,
		nullptr,
		GetModuleHandleW(nullptr),
		this);
	if (!hwnd_) {
		throw std::runtime_error("DesktopMirror: CreateWindowExW failed");
	}
	SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

	window_dc_ = GetDC(hwnd_);
	mem_dc_ = CreateCompatibleDC(window_dc_);
	std::memset(&bmi_, 0, sizeof(bmi_));
	bmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi_.bmiHeader.biWidth = static_cast<LONG>(width_);
	bmi_.bmiHeader.biHeight = -static_cast<LONG>(height_);  // top-down
	bmi_.bmiHeader.biPlanes = 1;
	bmi_.bmiHeader.biBitCount = 32;
	bmi_.bmiHeader.biCompression = BI_RGB;
	dib_ = CreateDIBSection(mem_dc_, &bmi_, DIB_RGB_COLORS, &dib_bits_, nullptr, 0);
	if (!dib_ || !dib_bits_) {
		throw std::runtime_error("DesktopMirror: CreateDIBSection failed");
	}
	SelectObject(mem_dc_, dib_);

	create_gpu_resources(renderer, color_format);
}

void DesktopMirror::create_gpu_resources(VulkanRenderer& renderer, VkFormat color_format) {
	device_ = renderer.device();
	physical_ = renderer.physical_device();
	queue_ = renderer.queue();
	queue_family_ = renderer.queue_family_index();
	command_pool_ = renderer.command_pool();

	VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = color_format;
	image_info.extent = {width_, height_, 1};
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	check_vk(vkCreateImage(device_, &image_info, nullptr, &color_image_), "vkCreateImage");

	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(device_, color_image_, &req);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = find_memory_type(physical_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check_vk(vkAllocateMemory(device_, &alloc, nullptr, &color_memory_), "vkAllocateMemory image");
	check_vk(vkBindImageMemory(device_, color_image_, color_memory_, 0), "vkBindImageMemory");

	VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	view_info.image = color_image_;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = color_format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	check_vk(vkCreateImageView(device_, &view_info, nullptr, &color_view_), "vkCreateImageView");

	staging_size_ = static_cast<VkDeviceSize>(width_) * height_ * 4;
	VkBufferCreateInfo buf{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buf.size = staging_size_;
	buf.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	check_vk(vkCreateBuffer(device_, &buf, nullptr, &staging_), "vkCreateBuffer staging");
	vkGetBufferMemoryRequirements(device_, staging_, &req);
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = find_memory_type(
		physical_,
		req.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	check_vk(vkAllocateMemory(device_, &alloc, nullptr, &staging_memory_), "vkAllocateMemory staging");
	check_vk(vkBindBufferMemory(device_, staging_, staging_memory_, 0), "vkBindBufferMemory");
	check_vk(vkMapMemory(device_, staging_memory_, 0, staging_size_, 0, &staging_mapped_), "vkMapMemory");
}

void DesktopMirror::destroy_gpu_resources() {
	if (device_ == VK_NULL_HANDLE) {
		return;
	}
	vkDeviceWaitIdle(device_);
	if (staging_mapped_) {
		vkUnmapMemory(device_, staging_memory_);
		staging_mapped_ = nullptr;
	}
	if (staging_) {
		vkDestroyBuffer(device_, staging_, nullptr);
		staging_ = VK_NULL_HANDLE;
	}
	if (staging_memory_) {
		vkFreeMemory(device_, staging_memory_, nullptr);
		staging_memory_ = VK_NULL_HANDLE;
	}
	if (color_view_) {
		vkDestroyImageView(device_, color_view_, nullptr);
		color_view_ = VK_NULL_HANDLE;
	}
	if (color_image_) {
		vkDestroyImage(device_, color_image_, nullptr);
		color_image_ = VK_NULL_HANDLE;
	}
	if (color_memory_) {
		vkFreeMemory(device_, color_memory_, nullptr);
		color_memory_ = VK_NULL_HANDLE;
	}
	device_ = VK_NULL_HANDLE;
}

bool DesktopMirror::poll() {
	if (!hwnd_) {
		return false;
	}
	MSG msg{};
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
		if (msg.message == WM_QUIT) {
			hwnd_ = nullptr;
			return false;
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	if (!IsWindow(hwnd_)) {
		hwnd_ = nullptr;
		return false;
	}
	return true;
}

float DesktopMirror::take_scroll_y() {
	const float out = scroll_y_;
	scroll_y_ = 0.0f;
	return out;
}

void DesktopMirror::present(VulkanRenderer& renderer, const Mat4& view_proj, const std::vector<DrawItem>& draws, float fps) {
	if (!hwnd_ || !color_image_) {
		return;
	}

	VkCommandBuffer cmd = renderer.begin_one_time_commands();
	renderer.record_eye(cmd, color_image_, color_view_, width_, height_, view_proj, draws);

	// COLOR_ATTACHMENT -> TRANSFER_SRC for readback.
	VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_src.image = color_image_;
	to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	to_src.subresourceRange.levelCount = 1;
	to_src.subresourceRange.layerCount = 1;
	to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		1,
		&to_src);

	VkBufferImageCopy region{};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent = {width_, height_, 1};
	vkCmdCopyImageToBuffer(cmd, color_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_, 1, &region);
	renderer.end_one_time_commands(cmd);

	blit_to_window(fps);
}

void DesktopMirror::blit_to_window(float fps) {
	if (!dib_bits_ || !staging_mapped_ || !hwnd_) {
		return;
	}
	const std::uint32_t pixel_count = width_ * height_;
	const auto* src = static_cast<const std::uint8_t*>(staging_mapped_);
	auto* dst = static_cast<std::uint8_t*>(dib_bits_);
	if (swizzle_rb_) {
		for (std::uint32_t i = 0; i < pixel_count; ++i) {
			dst[i * 4 + 0] = src[i * 4 + 2];
			dst[i * 4 + 1] = src[i * 4 + 1];
			dst[i * 4 + 2] = src[i * 4 + 0];
			dst[i * 4 + 3] = 255;
		}
	} else {
		// Already BGRA-ish for GDI.
		std::memcpy(dst, src, static_cast<std::size_t>(staging_size_));
	}

	RECT client{};
	GetClientRect(hwnd_, &client);
	const int cw = client.right - client.left;
	const int ch = client.bottom - client.top;
	StretchBlt(window_dc_, 0, 0, cw, ch, mem_dc_, 0, 0, static_cast<int>(width_), static_cast<int>(height_), SRCCOPY);

	if (fps > 0.0f) {
		wchar_t label[64];
		std::swprintf(label, 64, L"%.0f FPS", static_cast<double>(fps));
		SetBkMode(window_dc_, TRANSPARENT);
		SetTextColor(window_dc_, RGB(255, 240, 80));
		TextOutW(window_dc_, 12, 10, label, static_cast<int>(wcslen(label)));

		wchar_t title[96];
		std::swprintf(title, 96, L"HyperVR Mirror  —  %.0f FPS", static_cast<double>(fps));
		SetWindowTextW(hwnd_, title);
	}
}

bool DesktopMirror::save_screenshot_bmp(const std::string& path) const {
	if (!dib_bits_ || width_ == 0 || height_ == 0) {
		return false;
	}

#pragma pack(push, 1)
	struct BmpFileHeader {
		std::uint16_t type = 0x4D42;
		std::uint32_t size = 0;
		std::uint16_t reserved1 = 0;
		std::uint16_t reserved2 = 0;
		std::uint32_t off_bits = 54;
	};
	struct BmpInfoHeader {
		std::uint32_t size = 40;
		std::int32_t width = 0;
		std::int32_t height = 0;
		std::uint16_t planes = 1;
		std::uint16_t bit_count = 24;
		std::uint32_t compression = 0;
		std::uint32_t size_image = 0;
		std::int32_t x_pels = 0;
		std::int32_t y_pels = 0;
		std::uint32_t clr_used = 0;
		std::uint32_t clr_important = 0;
	};
#pragma pack(pop)

	const std::uint32_t row_stride = ((width_ * 3 + 3) / 4) * 4;
	const std::uint32_t image_size = row_stride * height_;
	BmpFileHeader file{};
	BmpInfoHeader info{};
	info.width = static_cast<std::int32_t>(width_);
	info.height = static_cast<std::int32_t>(height_);  // bottom-up
	info.size_image = image_size;
	file.size = 54 + image_size;

	std::ofstream out(path, std::ios::binary);
	if (!out) {
		return false;
	}
	out.write(reinterpret_cast<const char*>(&file), sizeof(file));
	out.write(reinterpret_cast<const char*>(&info), sizeof(info));

	std::vector<std::uint8_t> row(row_stride, 0);
	const auto* src = static_cast<const std::uint8_t*>(dib_bits_);
	for (std::int32_t y = static_cast<std::int32_t>(height_) - 1; y >= 0; --y) {
		const std::uint8_t* src_row = src + static_cast<std::size_t>(y) * width_ * 4;
		for (std::uint32_t x = 0; x < width_; ++x) {
			row[x * 3 + 0] = src_row[x * 4 + 0];  // B
			row[x * 3 + 1] = src_row[x * 4 + 1];  // G
			row[x * 3 + 2] = src_row[x * 4 + 2];  // R
		}
		out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row_stride));
	}
	return static_cast<bool>(out);
}

}  // namespace hypervr
