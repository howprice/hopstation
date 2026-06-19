# CMake 3.22 is installed on Ubuntu 22.04 LTS, so don't want to go higher than that.
# CMake 3.24 CMAKE_COMPILE_WARNING_AS_ERROR, but is ignored on lower versions.
# CMake 3.26 made USE_FOLDERS global property default to true
cmake_minimum_required(VERSION 3.22)

# "When using vcpkg as a submodule of your project, you can add the following to your CMakeLists.txt 
# before the first project() call, instead of passing CMAKE_TOOLCHAIN_FILE to the cmake invocation.
# This will still allow people to not use vcpkg, by passing the CMAKE_TOOLCHAIN_FILE directly, but 
# it will make the configure-build step slightly easier."
# https://github.com/microsoft/vcpkg/blob/master/README.md#vcpkg-as-a-submodule-with-cmake
#
# Only set the toolchain file if vcpkg is initialized
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake")
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "Vcpkg toolchain file")
else()
  message(WARNING "vcpkg not found. Run 'git submodule update --init --recursive' to initialize vcpkg.")
endif()

# project() should be called ASAP following cmake_minimum_required()
# Sets PROJECT_NAME. Defaults to LANGUAGES C CXX. This will be the Visual Studio Solution name.
project(hopstation)

if(CMAKE_SYSTEM MATCHES ".*rpi.*")
	set(RPI ON)
	message(VERBOSE, "Building for Raspberry Pi. Set RPI = ${RPI}")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)

# C++17 is required for terse static_assert
# C++20 is required for designated initializers. Very handy, but I can live without.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Define DEBUG in Debug build, and RELEASE in Release build.
# Don't define either DEBUG or RELEASE in RelWithDebInfo build (CMAKE_CXX_FLAGS_RELWITHDEBINFO) which is used for profiling
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DRELEASE")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -DPROFILE")
set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} -DRELEASE")

# ----------------------------------------------------------------------------

set(APP_TARGET "hopstation")

add_executable(${APP_TARGET})

set(SHADER_LIST
	"shaders/Rect.vert.hlsl"
	"shaders/Texture.frag.hlsl"
)

set(SRC_LIST
	"../core/ArrayHelpers.h"
	"../core/ClassHelpers.h"
	"../core/FileHelpers.cpp"
	"../core/FileHelpers.h"
	"../core/Helpers.cpp"
	"../core/Helpers.h"
	"../core/hp_assert.cpp"
	"../core/hp_assert.h"
	"../core/Log.cpp"
	"../core/Log.h"
	"../core/MathsHelpers.h"
	"../core/Parse.cpp"
	"../core/Parse.h"
	"../core/RingBuffer.cpp"
	"../core/RingBuffer.h"
	"../core/StringHelpers.cpp"
	"../core/StringHelpers.h"
	"../core/Types.h"
	"../psx/BIOS.cpp"
	"../psx/BIOS.h"
	"../psx/Bus.cpp"
	"../psx/Bus.h"
	"../psx/CD.cpp"
	"../psx/CD.h"
	"../psx/CDROM.cpp"
	"../psx/CDROM.h"
	"../psx/Controller.cpp"
	"../psx/Controller.h"
	"../psx/ControllerPort.cpp"
	"../psx/ControllerPort.h"
	"../psx/DMA.cpp"
	"../psx/DMA.h"
	"../psx/GPU.cpp"
	"../psx/GPU.h"
	"../psx/GTE.cpp"
	"../psx/GTE.h"
	"../psx/INTC.cpp"
	"../psx/INTC.h"
	"../psx/MDEC.cpp"
	"../psx/MDEC.h"
	"../psx/MemoryCard.cpp"
	"../psx/MemoryCard.h"
	"../psx/R3000.cpp"
	"../psx/R3000.h"
	"../psx/R3000Dasm.cpp"
	"../psx/R3000Dasm.h"
	"../psx/RAM.cpp"
	"../psx/RAM.h"
	"../psx/Scheduler.cpp"
	"../psx/Scheduler.h"
	"../psx/SIO.cpp"
	"../psx/SIO.h"
	"../psx/SPU.cpp"
	"../psx/SPU.h"
	"../psx/Timers.cpp"
	"../psx/Timers.h"
	"../psx/Timing.h"
	"../psx-utils/R3000Utils.cpp"
	"../psx-utils/R3000Utils.h"
	"../psx-utils/Sideload.cpp"
	"../psx-utils/Sideload.h"
	"../psx-utils/Snapshot.cpp"
	"../psx-utils/Snapshot.h"
	"../psx-utils/TTYLogger.cpp"
	"../psx-utils/TTYLogger.h"
	"../psx-utils/VRAMConvert.cpp"
	"../psx-utils/VRAMConvert.h"
	"../.editorconfig"
	"../README.md"
	"src/GUI/CDROMWindow.cpp"
	"src/GUI/CDROMWindow.h"
	"src/GUI/CDWindow.cpp"
	"src/GUI/CDWindow.h"
	"src/GUI/CPUWindow.cpp"
	"src/GUI/CPUWindow.h"
	"src/GUI/DMAWindow.cpp"
	"src/GUI/DMAWindow.h"
	"src/GUI/GPUWindow.cpp"
	"src/GUI/GPUWindow.h"
	"src/GUI/HostWindow.cpp"
	"src/GUI/HostWindow.h"
	"src/GUI/ImGuiDemoWindow.cpp"
	"src/GUI/ImGuiDemoWindow.h"
	"src/GUI/ImGuiWrap.cpp"
	"src/GUI/ImGuiWrap.h"
	"src/GUI/InsertDiscDialog.cpp"
	"src/GUI/InsertDiscDialog.h"
	"src/GUI/MemoryCardFileDialog.cpp"
	"src/GUI/MemoryCardFileDialog.h"
	"src/GUI/MemoryCardWindow.cpp"
	"src/GUI/MemoryCardWindow.h"
	"src/GUI/SideloadDialog.cpp"
	"src/GUI/SideloadDialog.h"
	"src/GUI/SnapshotDialog.cpp"
	"src/GUI/SnapshotDialog.h"
	"src/GUI/SPUWindow.cpp"
	"src/GUI/SPUWindow.h"
	"src/imgui/backends/imgui_impl_sdl3.cpp"
	"src/imgui/backends/imgui_impl_sdl3.h"
	"src/imgui/backends/imgui_impl_sdlgpu3.cpp"
	"src/imgui/backends/imgui_impl_sdlgpu3.h"
	"src/imgui/backends/imgui_impl_sdlgpu3_shaders.h"
	"src/imgui/.editorconfig"
	"src/imgui/imconfig.h"
	"src/imgui/imgui.cpp"
	"src/imgui/imgui.h"
	"src/imgui/imgui.natvis"
	"src/imgui/imgui_demo.cpp"
	"src/imgui/imgui_draw.cpp"
	"src/imgui/imgui_internal.h"
	"src/imgui/imgui_tables.cpp"
	"src/imgui/imgui_widgets.cpp"
	"src/imgui/imstb_rectpack.h"
	"src/imgui/imstb_textedit.h"
	"src/imgui/imstb_truetype.h"
	"src/AudioDevice.cpp"
	"src/AudioDevice.h"
	"src/HopStation.cpp"
	"src/Host.cpp"
	"src/Host.h"
	"src/HostAudioBuffer.cpp"
	"src/HostAudioBuffer.h"
	"src/Input.cpp"
	"src/Input.h"
	"src/Renderer.cpp"
	"src/Renderer.h"
	"src/Texture.cpp"
	"src/Texture.h"
	${SHADER_LIST}
)

set_property(SOURCE ${SHADER_LIST} PROPERTY VS_SETTINGS "ExcludedFromBuild=true")

target_sources(${APP_TARGET} PRIVATE ${SRC_LIST} )

target_include_directories(
	${APP_TARGET} 
	PRIVATE
	"src"
	"src/imgui"
	".."
)

set_property(TARGET ${APP_TARGET} PROPERTY COMPILE_WARNING_AS_ERROR ON)

# Compiler-specific options
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")

	target_compile_options(
		${APP_TARGET} 
		PRIVATE
		/W4 
		/wd4127 # conditional expression is constant
		/we4062 # enumerator 'identifier' in switch of enum 'enumeration' is not handled
	)

	# Allow use of portable fopen (fopen_s is Windows only)
	target_compile_definitions(${APP_TARGET} PUBLIC _CRT_SECURE_NO_WARNINGS _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU") # GCC

	target_compile_options(
		${APP_TARGET} 
		PRIVATE 
		-Wall 
		-Wextra 

		# Unfortunately seem to have to disable -pendantic to use anonymous structs (R3000 CauseRegister) :-(
		# -pedantic
		
		-Wno-unknown-pragmas # Can't do in source file. See https://stackoverflow.com/questions/12842306/suppress-wunknown-pragmas-warning-in-gcc 
		-Werror
		-Wno-gnu-zero-variadic-macro-arguments

		# TODO: Try to enable Microsoft extensions and reinstate -pedantic above
		# Suppress error: ISO C++ prohibits anonymous structs [-Werror=pedantic] for R3000 CauseRegister union
		#-fms-extensions
	)

elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
	
	target_compile_options(
		${APP_TARGET} 
		PRIVATE 
		-Wall 
		-Wextra 
		-pedantic
		-Wno-unknown-pragmas # Can't do in source file. See https://stackoverflow.com/questions/12842306/suppress-wunknown-pragmas-warning-in-gcc 
		-Werror
		-Wno-gnu-zero-variadic-macro-arguments
		
		# R3000 kOpcodes[] TODO: Remove this
		-Wno-unused-const-variable

		# Suppress error: ISO C++ prohibits anonymous structs [-Werror=pedantic] for R3000 CauseRegister union
		-Wno-gnu-anonymous-struct -Wno-nested-anon-types
		)	
else()
	message(FATAL_ERROR, "Unknown compiler")
endif()

target_link_libraries(${APP_TARGET} PRIVATE SDL3::SDL3)

# Define grouping for source files in IDE project generation. This is just for Visual Studio IDE convenience.
# Without this, all .cpp files are in the "Source Files" filter, and all .h files are in the "Header files" filter.
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/.. FILES ${SRC_LIST}) # Requires CMake 3.8

# Keep all data for the repo in a single folder for convenience
set(DATA_DIR "${PROJECT_SOURCE_DIR}/../data")

set_target_properties(
	${APP_TARGET} PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY ${DATA_DIR}
)

# Default Visual Studio start up project
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${APP_TARGET})

# Add custom target to build shaders
if(WIN32)
    set(SHADER_BUILD_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/shaders/build-shaders.bat")
elseif(APPLE)
    set(SHADER_BUILD_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/shaders/build-shaders-macos.sh")
else()
    set(SHADER_BUILD_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/shaders/build-shaders-linux.sh")
endif()

add_custom_target(build_shaders
    COMMAND ${SHADER_BUILD_SCRIPT}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    COMMENT "Building shaders..."
    VERBATIM
)

# Make the executable depend on shaders being built
add_dependencies(${APP_TARGET} build_shaders)
