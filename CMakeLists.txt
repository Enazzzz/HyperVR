cmake_minimum_required(VERSION 3.24)

project(hypervr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HYPERVR_BUILD_PYTHON_BINDINGS "Build Python extension module" ON)
option(HYPERVR_BUILD_DEMO "Build native xr_demo application" ON)

if(NOT DEFINED ENV{VULKAN_SDK} AND EXISTS "C:/VulkanSDK")
	file(GLOB _hypervr_vulkan_sdks "C:/VulkanSDK/*")
	list(SORT _hypervr_vulkan_sdks)
	list(REVERSE _hypervr_vulkan_sdks)
	list(GET _hypervr_vulkan_sdks 0 _hypervr_vulkan_sdk)
	set(ENV{VULKAN_SDK} "${_hypervr_vulkan_sdk}")
	list(APPEND CMAKE_PREFIX_PATH "${_hypervr_vulkan_sdk}")
endif()

find_package(Vulkan REQUIRED)

include(FetchContent)
FetchContent_Declare(
	OpenXR-SDK
	GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
	GIT_TAG release-1.1.43
	GIT_SHALLOW TRUE
)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(OpenXR-SDK)

set(HYPERVR_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders")
find_program(GLSLANG_VALIDATOR glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(NOT GLSLANG_VALIDATOR)
	message(FATAL_ERROR "glslangValidator not found; install the Vulkan SDK")
endif()

set(HYPERVR_VERT_SPV "${HYPERVR_SHADER_DIR}/mesh.vert.spv")
set(HYPERVR_FRAG_SPV "${HYPERVR_SHADER_DIR}/mesh.frag.spv")
add_custom_command(
	OUTPUT "${HYPERVR_VERT_SPV}"
	COMMAND "${GLSLANG_VALIDATOR}" -V "${HYPERVR_SHADER_DIR}/mesh.vert" -o "${HYPERVR_VERT_SPV}"
	DEPENDS "${HYPERVR_SHADER_DIR}/mesh.vert"
	COMMENT "Compiling mesh.vert"
)
add_custom_command(
	OUTPUT "${HYPERVR_FRAG_SPV}"
	COMMAND "${GLSLANG_VALIDATOR}" -V "${HYPERVR_SHADER_DIR}/mesh.frag" -o "${HYPERVR_FRAG_SPV}"
	DEPENDS "${HYPERVR_SHADER_DIR}/mesh.frag"
	COMMENT "Compiling mesh.frag"
)
add_custom_target(hypervr_shaders DEPENDS "${HYPERVR_VERT_SPV}" "${HYPERVR_FRAG_SPV}")

add_library(hypervr_engine STATIC
	engine/src/core/engine.cpp
	engine/src/core/desktop_mirror.cpp
	engine/src/core/mesh_loader.cpp
	engine/src/vulkan/vulkan_renderer.cpp
	engine/src/xr/openxr_context.cpp
)
add_dependencies(hypervr_engine hypervr_shaders)
target_include_directories(hypervr_engine PUBLIC
	engine/include
	${Vulkan_INCLUDE_DIRS}
)
target_compile_definitions(hypervr_engine PUBLIC
	HYPERVR_SHADER_DIR="${HYPERVR_SHADER_DIR}"
	HYPERVR_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
	XR_USE_GRAPHICS_API_VULKAN
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	_CRT_SECURE_NO_WARNINGS
)
target_link_libraries(hypervr_engine PUBLIC Vulkan::Vulkan OpenXR::openxr_loader)
if(WIN32)
	target_link_libraries(hypervr_engine PUBLIC user32 gdi32)
endif()
if(MSVC)
	target_compile_options(hypervr_engine PRIVATE /W4 /permissive- /EHsc)
endif()

if(HYPERVR_BUILD_DEMO)
	add_executable(xr_demo apps/xr_demo/main.cpp)
	target_link_libraries(xr_demo PRIVATE hypervr_engine)
	set_target_properties(xr_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

if(HYPERVR_BUILD_PYTHON_BINDINGS)
	find_package(Python3 COMPONENTS Interpreter Development.Module)
	if(Python3_FOUND)
		Python3_add_library(hypervr_python MODULE bindings/python/src/module.cpp)
		target_link_libraries(hypervr_python PRIVATE hypervr_engine)
		target_compile_definitions(hypervr_python PRIVATE PY_SSIZE_T_CLEAN)
		set_target_properties(hypervr_python PROPERTIES OUTPUT_NAME hypervr PREFIX "")
		if(WIN32)
			set_target_properties(hypervr_python PROPERTIES SUFFIX ".pyd")
		endif()
	else()
		message(WARNING "Python3 not found; skipping hypervr Python bindings")
	endif()
endif()
