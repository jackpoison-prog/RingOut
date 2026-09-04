cmake_minimum_required(VERSION 3.20)
if(APPLE)
    project(ModernGekko LANGUAGES C CXX OBJCXX)
else()
    project(ModernGekko LANGUAGES C CXX)
endif()

include(CTest)
include(CheckIncludeFileCXX)

# The version the player sees, from ONE place. Both the window title and the
# in-game menu header used to carry their own literal, and both still said
# "Ver 1.0" at release 1.5 -- the runtime's #ifndef was written for a build-time
# define that was never added, and the menu had a hard-coded string. The file is
# outside this tree because the packaging scripts default from the same one, so
# a release cannot be half-numbered. Standalone builds of ModernGekko have no
# such file and keep the in-source fallbacks.
set(MODERNGEKKO_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
# file(READ) happens at CONFIGURE time, so editing VERSION alone changes
# nothing: ninja has no reason to re-run cmake and rebuilds every object with
# the PREVIOUS number still compiled in. Bumping 1.5.1 -> 1.5.2 did exactly
# that, and the only thing that noticed was the packaging guard, long after.
# Naming the file as a configure dependency is what makes "edit VERSION,
# rebuild" true.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${MODERNGEKKO_VERSION_FILE}")
if(EXISTS "${MODERNGEKKO_VERSION_FILE}")
    file(READ "${MODERNGEKKO_VERSION_FILE}" MODERNGEKKO_VERSION_RAW)
    string(STRIP "${MODERNGEKKO_VERSION_RAW}" MODERNGEKKO_VERSION_RAW)
    if(MODERNGEKKO_VERSION_RAW MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
        add_compile_definitions(MODERNGEKKO_PROJECT_VERSION="Ver ${MODERNGEKKO_VERSION_RAW}")
        message(STATUS "moderngekko: version Ver ${MODERNGEKKO_VERSION_RAW} (from ${MODERNGEKKO_VERSION_FILE})")
    else()
        message(WARNING "moderngekko: ignoring unparseable VERSION '${MODERNGEKKO_VERSION_RAW}'")
    endif()
endif()

# Keep the builder's absolute paths out of shipped binaries. __FILE__ literals
# from asserts/logging live in .rodata, so `strip` does NOT remove them -- a
# release build otherwise leaks the full home directory of whoever built it
# (440 such strings were found in one). -ffile-prefix-map rewrites them at
# compile time; harmless on compilers that lack it.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/ringout)
endif()

check_include_file_cxx(dlfcn.h MODERNGEKKO_HAS_DLFCN)
set(MODERNGEKKO_DYNAMIC_MODULES_DEFAULT ${MODERNGEKKO_HAS_DLFCN})
option(MODERNGEKKO_ENABLE_DYNAMIC_MODULES
       "Load game modules through the host dynamic-library API"
       ${MODERNGEKKO_DYNAMIC_MODULES_DEFAULT})

option(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME
       "Build the RecompCore/Dolphin production chassis" ON)
option(MODERNGEKKO_ENABLE_DOLPHIN_TESTS
       "Build the imported Dolphin unit-test targets" OFF)
# dolphin-tool reads the disc formats DolRecomp's own extractor does not (RVZ,
# GCZ, WIA). Off by default -- it is a development tool, not part of the chassis.
option(MODERNGEKKO_ENABLE_DOLPHIN_CLI_TOOL
       "Build the imported dolphin-tool disc utility" OFF)
set(MODERNGEKKO_FRONTEND_SDK "" CACHE PATH
    "Optional non-root sysroot containing X11 and audio development packages")

if(MODERNGEKKO_FRONTEND_SDK)
    set(_moderngekko_sdk_usr "${MODERNGEKKO_FRONTEND_SDK}/usr")
    set(_moderngekko_sdk_lib "${_moderngekko_sdk_usr}/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
    if(NOT IS_DIRECTORY "${_moderngekko_sdk_usr}/include" OR
       NOT IS_DIRECTORY "${_moderngekko_sdk_lib}")
        message(FATAL_ERROR
            "MODERNGEKKO_FRONTEND_SDK is not a usable sysroot: ${MODERNGEKKO_FRONTEND_SDK}")
    endif()

    list(PREPEND CMAKE_PREFIX_PATH "${_moderngekko_sdk_usr}")
    list(PREPEND CMAKE_INCLUDE_PATH "${_moderngekko_sdk_usr}/include")
    list(PREPEND CMAKE_LIBRARY_PATH "${_moderngekko_sdk_lib}")
    link_directories("${_moderngekko_sdk_lib}")

    set(_moderngekko_pkgconfig
        "${_moderngekko_sdk_lib}/pkgconfig:${_moderngekko_sdk_usr}/share/pkgconfig")
    if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
        string(APPEND _moderngekko_pkgconfig ":$ENV{PKG_CONFIG_PATH}")
    endif()
    set(ENV{PKG_CONFIG_PATH} "${_moderngekko_pkgconfig}")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${MODERNGEKKO_FRONTEND_SDK}")

    # FindALSA does not use pkg-config on all supported CMake releases.
    if(EXISTS "${_moderngekko_sdk_lib}/libasound.so")
        set(ALSA_INCLUDE_DIR "${_moderngekko_sdk_usr}/include" CACHE PATH "" FORCE)
        set(ALSA_LIBRARY "${_moderngekko_sdk_lib}/libasound.so" CACHE FILEPATH "" FORCE)
    endif()
endif()

if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
    set(MODERNGEKKO_ENABLE_IMGUI_LAUNCHER ON CACHE BOOL "" FORCE)
    set(ENABLE_QT OFF CACHE BOOL "" FORCE)
    set(ENABLE_NOGUI OFF CACHE BOOL "" FORCE)
    set(ENABLE_CLI_TOOL ${MODERNGEKKO_ENABLE_DOLPHIN_CLI_TOOL} CACHE BOOL "" FORCE)
    set(ENABLE_TESTS ${MODERNGEKKO_ENABLE_DOLPHIN_TESTS} CACHE BOOL "" FORCE)
    set(ENABLE_AUTOUPDATE OFF CACHE BOOL "" FORCE)
    set(ENABLE_ANALYTICS OFF CACHE BOOL "" FORCE)
    set(USE_RETRO_ACHIEVEMENTS OFF CACHE BOOL "" FORCE)
    set(USE_DISCORD_PRESENCE OFF CACHE BOOL "" FORCE)
    set(USE_MGBA OFF CACHE BOOL "" FORCE)
    set(USE_UPNP OFF CACHE BOOL "" FORCE)
    set(ENCODE_FRAMEDUMPS OFF CACHE BOOL "" FORCE)
    set(ENABLE_LLVM OFF CACHE BOOL "" FORCE)
    # Cubeb is Dolphin's only audio backend on macOS
    if(APPLE)
        set(ENABLE_CUBEB ON CACHE BOOL "" FORCE)
    else()
        set(ENABLE_CUBEB OFF CACHE BOOL "" FORCE)
    endif()
    set(ENABLE_HWDB OFF CACHE BOOL "" FORCE)
    set(ENABLE_EVDEV OFF CACHE BOOL "" FORCE)
    set(ENABLE_WAYLAND ON CACHE BOOL "" FORCE)
    set(ENABLE_EGL OFF CACHE BOOL "" FORCE)
    # Vendored by default, as every other dependency here is: the shipped
    # builds must not vary with whatever SDL a build machine happens to have.
    # But a SteamOS-native build cannot use the bundled copy -- SDL 3.4.4's
    # X11 backend re-typedefs XGenericEventCookie, which collides with
    # libx11 >= 1.8.12 (Debian 12 ships 1.8.4, so the container never sees it).
    # Guarded rather than FORCEd so an explicit -DUSE_SYSTEM_SDL3=ON can win;
    # with no -D the result is byte-identical to before.
    if(NOT DEFINED USE_SYSTEM_SDL3)
        set(USE_SYSTEM_SDL3 OFF CACHE BOOL "" FORCE)
    endif()
    # SDL bundles its own copy of the macOS hidapi backend
    if(APPLE)
        set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
    endif()
    set(LINUX_LOCAL_DEV ON CACHE BOOL "" FORCE)
    set(DISTRIBUTOR "ModernGekko" CACHE STRING "" FORCE)
    # zstd defaults ZSTD_BUILD_TESTS to BUILD_TESTING, so enabling our own tests
    # also registered fullbench/fuzzer/zstreamtest with ctest. Their executables
    # are EXCLUDE_FROM_ALL (nothing links them), so those three could only ever
    # report "Not Run" -- three permanent failures in every ctest run, which is
    # how a real regression gets ignored. Set here rather than in
    # vendor/dolphin/Externals/zstd/CMakeLists.txt alongside its siblings, so an
    # upstream sync cannot quietly revert it.
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(vendor/dolphin EXCLUDE_FROM_ALL)
    if(MODERNGEKKO_FRONTEND_SDK)
        # These two imported targets historically rely on X11 headers living
        # in the compiler's default /usr/include search path.
        target_include_directories(common SYSTEM PRIVATE "${_moderngekko_sdk_usr}/include")
        if(TARGET videovulkan)
            target_include_directories(videovulkan SYSTEM PRIVATE
                "${_moderngekko_sdk_usr}/include")
        endif()
    endif()
else()
    set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
    set(FMT_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(vendor/fmt/fmt EXCLUDE_FROM_ALL)
endif()

# Build the WORKING recompiler at ../DolRecomp, not the copy under vendor/.
#
# vendor/dolphin carries DolRecomp as a submodule of its own (ExpansionPak/
# DolRecomp), and those files came along when Dolphin was vendored. Dolphin's
# build never references it, nothing here links its libraries, and its binary is
# used by nothing -- it is pinned two upstream commits behind ../DolRecomp and
# carries NONE of this project's recompiler work: no --idle-pc, no lazy FPRF, no
# --chain-calls / --dispatch-pc / --ca-liveness / --leader-cases. 3177 lines apart.
#
# Building it meant ten of this suite's tests -- opcodes, dispatch,
# codegen_compile, jumptables, pc_reference and friends -- exercised a
# recompiler nobody ships, while the one that DOES ship (packaged as
# tools/dolrecomp, mirrored into module-src/deps/dolrecomp-src, and guarded by
# package-dist.sh against ../DolRecomp/src) had no coverage here at all. The
# names read like recompiler coverage, which is what made it worth fixing.
#
# The fallback matters: the GPL source shipment archives ModernGekko and
# DolRecomp separately, so an extraction of ModernGekko alone has no sibling.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../DolRecomp/CMakeLists.txt")
    set(_moderngekko_dolrecomp "${CMAKE_CURRENT_SOURCE_DIR}/../DolRecomp")
else()
    set(_moderngekko_dolrecomp "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/DolRecomp")
    message(STATUS "DolRecomp: no sibling ../DolRecomp; using the vendored copy")
endif()
if(BUILD_TESTING)
    add_subdirectory("${_moderngekko_dolrecomp}" dolrecomp-build)
else()
    add_subdirectory("${_moderngekko_dolrecomp}" dolrecomp-build EXCLUDE_FROM_ALL)
endif()

add_library(moderngekko_abi INTERFACE)
add_library(ModernGekko::ABI ALIAS moderngekko_abi)

target_include_directories(moderngekko_abi
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

add_library(moderngekko_legacy STATIC
    src/audio/audio_system.cpp
    src/hardware/mmio_bus.cpp
    src/hardware/disc_interface.cpp
    src/hardware/processor_interface.cpp
    src/gpu/gx_command_processor.cpp
    src/gpu/gx_state_backend.cpp
    src/gpu/gx_vertex_loader.cpp
    src/gpu/gx_texture_decoder.cpp
    src/gpu/gx_pipeline.cpp
    src/input/controller.cpp
    src/memory/address_space.cpp
    src/runtime/module.cpp
    src/runtime/module_loader.cpp
    src/runtime/game.cpp
    src/runtime/legacy_runtime.cpp
    src/timing/event_scheduler.cpp
    vendor/dolphin_legacy/Core/Boot/BootMemory.cpp
    vendor/dolphin_legacy/Core/Boot/DolReader.cpp
    vendor/dolphin_legacy/Core/PowerPC/Interpreter/IntegerBranch.cpp
    vendor/dolphin_legacy/Core/PowerPC/ConditionRegister.cpp
)
add_library(ModernGekko::LegacyRuntime ALIAS moderngekko_legacy)

add_library(moderngekko STATIC
    src/runtime/module.cpp
    src/runtime/module_loader.cpp
    src/runtime/game.cpp
)
add_library(ModernGekko::Runtime ALIAS moderngekko)

add_library(moderngekko_dolphin_video STATIC
    src/gpu/dolphin_video_host.cpp
    src/gpu/dolphin_shader_compiler.cpp
    vendor/dolphin_legacy/VideoCommon/BPMemory.cpp
    vendor/dolphin_legacy/VideoCommon/XFMemory.cpp
    vendor/dolphin_legacy/VideoCommon/ShaderGenCommon.cpp
    vendor/dolphin_legacy/VideoCommon/LightingShaderGen.cpp
    vendor/dolphin_legacy/VideoCommon/PixelShaderGen.cpp
    vendor/dolphin_legacy/VideoCommon/VertexShaderGen.cpp
    vendor/dolphin_legacy/VideoCommon/GeometryShaderGen.cpp
    vendor/dolphin_legacy/VideoCommon/DriverDetails.cpp
    vendor/dolphin_legacy/VideoCommon/UberShaderCommon.cpp
    vendor/dolphin_legacy/VideoCommon/UberShaderPixel.cpp
    vendor/dolphin_legacy/VideoCommon/UberShaderVertex.cpp
)
if(APPLE)
    target_sources(moderngekko_dolphin_video PRIVATE
        vendor/dolphin_legacy/Common/CommonFuncsObjC.mm)
    target_link_libraries(moderngekko_dolphin_video PUBLIC "-framework Foundation")
endif()
add_library(ModernGekko::DolphinVideo ALIAS moderngekko_dolphin_video)
set_target_properties(moderngekko_dolphin_video PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_features(moderngekko_dolphin_video PUBLIC cxx_std_23)
target_include_directories(moderngekko_dolphin_video
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
           "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin_legacy"
           "${CMAKE_CURRENT_SOURCE_DIR}/vendor/picojson"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/dolphin_shims"
)
target_link_libraries(moderngekko_dolphin_video PUBLIC fmt::fmt)

target_link_libraries(moderngekko_legacy PUBLIC ModernGekko::ABI fmt::fmt)
target_compile_features(moderngekko_legacy PUBLIC cxx_std_23)
target_include_directories(moderngekko_legacy PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin_legacy")

target_link_libraries(moderngekko PUBLIC ModernGekko::ABI PRIVATE fmt::fmt)
if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
    target_sources(moderngekko PRIVATE
        src/runtime/dolphin_runtime.cpp
        vendor/dolphin/Source/Core/DolphinNoGUI/Platform.cpp
        vendor/dolphin/Source/Core/DolphinNoGUI/PlatformHeadless.cpp
    )
    if(APPLE)
        # Dolphin's windowed backend for macOS
        target_sources(moderngekko PRIVATE
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformMacos.mm)
        set_source_files_properties(
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformMacos.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(moderngekko PRIVATE
            "${APPKIT_LIBRARY}" "${COREFOUNDATION_LIBRARY}" "${IOK_LIBRARY}")
        target_compile_definitions(moderngekko PRIVATE MODERNGEKKO_HAVE_COCOA=1)
    endif()
    target_include_directories(moderngekko PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/include")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_compile_definitions(moderngekko PRIVATE _ARCH_64=1)
    else()
        target_compile_definitions(moderngekko PRIVATE _ARCH_32=1)
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x64|x86_64|amd64|AMD64")
        target_compile_definitions(moderngekko PRIVATE _M_X86_64=1)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        target_compile_definitions(moderngekko PRIVATE _M_ARM_64=1)
    else()
        target_compile_definitions(moderngekko PRIVATE _M_GENERIC=1)
    endif()
    target_link_libraries(moderngekko PRIVATE core uicommon)
    if(TARGET PkgConfig::X11)
        target_sources(moderngekko PRIVATE
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformX11.cpp)
        target_link_libraries(moderngekko PRIVATE PkgConfig::X11)
        if(TARGET PkgConfig::XRANDR)
            target_link_libraries(moderngekko PRIVATE PkgConfig::XRANDR)
            target_compile_definitions(moderngekko PRIVATE HAVE_XRANDR=1)
        endif()
        target_compile_definitions(moderngekko PRIVATE HAVE_X11=1)
    endif()
    if(TARGET PkgConfig::WAYLAND)
        find_program(MODERNGEKKO_WAYLAND_SCANNER wayland-scanner
            HINTS "${_moderngekko_sdk_usr}/bin" REQUIRED)
        find_file(MODERNGEKKO_XDG_SHELL_XML xdg-shell.xml
            HINTS "${_moderngekko_sdk_usr}/share/wayland-protocols/stable/xdg-shell"
                  "/usr/share/wayland-protocols/stable/xdg-shell"
            REQUIRED)
        find_file(MODERNGEKKO_XDG_DECORATION_XML xdg-decoration-unstable-v1.xml
            HINTS "${_moderngekko_sdk_usr}/share/wayland-protocols/unstable/xdg-decoration"
                  "/usr/share/wayland-protocols/unstable/xdg-decoration"
            REQUIRED)
        find_file(MODERNGEKKO_IDLE_INHIBIT_XML idle-inhibit-unstable-v1.xml
            HINTS "${_moderngekko_sdk_usr}/share/wayland-protocols/unstable/idle-inhibit"
                  "/usr/share/wayland-protocols/unstable/idle-inhibit"
            REQUIRED)
        find_library(MODERNGEKKO_WAYLAND_CURSOR_LIBRARY wayland-cursor
            HINTS "${_moderngekko_sdk_lib}" REQUIRED)
        find_library(MODERNGEKKO_XKBCOMMON_LIBRARY xkbcommon
            HINTS "${_moderngekko_sdk_lib}" REQUIRED)
        set(_moderngekko_wayland_generated
            "${CMAKE_CURRENT_BINARY_DIR}/generated/wayland")
        set(_moderngekko_xdg_header
            "${_moderngekko_wayland_generated}/xdg-shell-client-protocol.h")
        set(_moderngekko_xdg_code
            "${_moderngekko_wayland_generated}/xdg-shell-protocol.c")
        set(_moderngekko_decoration_header
            "${_moderngekko_wayland_generated}/xdg-decoration-client-protocol.h")
        set(_moderngekko_decoration_code
            "${_moderngekko_wayland_generated}/xdg-decoration-protocol.c")
        set(_moderngekko_idle_inhibit_header
            "${_moderngekko_wayland_generated}/idle-inhibit-unstable-v1-client-protocol.h")
        set(_moderngekko_idle_inhibit_code
            "${_moderngekko_wayland_generated}/idle-inhibit-unstable-v1-protocol.c")
        add_custom_command(
            OUTPUT "${_moderngekko_xdg_header}" "${_moderngekko_xdg_code}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${_moderngekko_wayland_generated}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" client-header
                    "${MODERNGEKKO_XDG_SHELL_XML}" "${_moderngekko_xdg_header}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" private-code
                    "${MODERNGEKKO_XDG_SHELL_XML}" "${_moderngekko_xdg_code}"
            DEPENDS "${MODERNGEKKO_XDG_SHELL_XML}"
            VERBATIM)
        add_custom_command(
            OUTPUT "${_moderngekko_decoration_header}" "${_moderngekko_decoration_code}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${_moderngekko_wayland_generated}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" client-header
                    "${MODERNGEKKO_XDG_DECORATION_XML}" "${_moderngekko_decoration_header}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" private-code
                    "${MODERNGEKKO_XDG_DECORATION_XML}" "${_moderngekko_decoration_code}"
            DEPENDS "${MODERNGEKKO_XDG_DECORATION_XML}"
            VERBATIM)
        add_custom_command(
            OUTPUT "${_moderngekko_idle_inhibit_header}" "${_moderngekko_idle_inhibit_code}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${_moderngekko_wayland_generated}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" client-header
                    "${MODERNGEKKO_IDLE_INHIBIT_XML}" "${_moderngekko_idle_inhibit_header}"
            COMMAND "${MODERNGEKKO_WAYLAND_SCANNER}" private-code
                    "${MODERNGEKKO_IDLE_INHIBIT_XML}" "${_moderngekko_idle_inhibit_code}"
            DEPENDS "${MODERNGEKKO_IDLE_INHIBIT_XML}"
            VERBATIM)
        target_sources(moderngekko PRIVATE
            vendor/dolphin/Source/Core/DolphinNoGUI/PlatformWayland.cpp
            "${_moderngekko_xdg_code}"
            "${_moderngekko_decoration_code}"
            "${_moderngekko_idle_inhibit_code}")
        target_include_directories(moderngekko PRIVATE
            "${_moderngekko_wayland_generated}")
        if(MODERNGEKKO_FRONTEND_SDK)
            target_link_libraries(moderngekko PRIVATE
                "${_moderngekko_sdk_lib}/libwayland-client.so")
        else()
            target_link_libraries(moderngekko PRIVATE PkgConfig::WAYLAND)
        endif()
        target_link_libraries(moderngekko PRIVATE
            "${MODERNGEKKO_WAYLAND_CURSOR_LIBRARY}"
            "${MODERNGEKKO_XKBCOMMON_LIBRARY}")
        target_compile_definitions(moderngekko PRIVATE HAVE_WAYLAND=1)
        target_compile_definitions(videovulkan PRIVATE HAVE_WAYLAND=1)
        if(MODERNGEKKO_FRONTEND_SDK)
            target_link_directories(moderngekko PRIVATE "${_moderngekko_sdk_lib}")
        endif()
    endif()
endif()
if(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
    target_sources(moderngekko_legacy PRIVATE vendor/dolphin_legacy/Common/DynamicLibrary.cpp)
    target_link_libraries(moderngekko_legacy PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(moderngekko_legacy PRIVATE MODERNGEKKO_ENABLE_DYNAMIC_MODULES=1)
    if(NOT MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
        target_sources(moderngekko PRIVATE vendor/dolphin_legacy/Common/DynamicLibrary.cpp)
        target_link_libraries(moderngekko PRIVATE ${CMAKE_DL_LIBS})
    endif()
    target_compile_definitions(moderngekko PRIVATE MODERNGEKKO_ENABLE_DYNAMIC_MODULES=1)
endif()
target_compile_features(moderngekko PUBLIC cxx_std_23)
if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
    target_include_directories(moderngekko PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core")
else()
    target_include_directories(moderngekko PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin_legacy")
endif()

add_executable(moderngekko-module-info
    tools/module_info.cpp
)
target_link_libraries(moderngekko-module-info
    PRIVATE ModernGekko::Runtime
)

if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
    if(TARGET SDL3-static)
        set(_moderngekko_sdl_target SDL3-static)
    elseif(TARGET SDL3::SDL3-shared)
        set(_moderngekko_sdl_target SDL3::SDL3-shared)
    elseif(TARGET SDL3::SDL3)
        set(_moderngekko_sdl_target SDL3::SDL3)
    else()
        message(FATAL_ERROR "No usable SDL3 target is available for the frontend")
    endif()
    add_executable(moderngekko-run
        tools/moderngekko_run.cpp
        tools/netplay_compatibility.cpp
        tools/netplay_session.cpp
        tools/frontend_config.cpp
        vendor/dolphin/Externals/imgui/imgui/backends/imgui_impl_sdl3.cpp
        vendor/dolphin/Externals/imgui/imgui/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(moderngekko-run PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Externals/imgui/imgui/backends")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_compile_definitions(moderngekko-run PRIVATE _ARCH_64=1)
    else()
        target_compile_definitions(moderngekko-run PRIVATE _ARCH_32=1)
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x64|x86_64|amd64|AMD64")
        target_compile_definitions(moderngekko-run PRIVATE _M_X86_64=1)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        target_compile_definitions(moderngekko-run PRIVATE _M_ARM_64=1)
    else()
        target_compile_definitions(moderngekko-run PRIVATE _M_GENERIC=1)
    endif()
    if(MODERNGEKKO_FRONTEND_SDK)
        set_property(TARGET moderngekko-run PROPERTY BUILD_RPATH
            "${_moderngekko_sdk_lib}")
    endif()
    target_link_libraries(moderngekko-run PRIVATE
        ModernGekko::Runtime core uicommon imgui ${_moderngekko_sdl_target})
    # Portability: this tree builds on bleeding-edge toolchains (GCC 16 /
    # glibc 2.44 here), which makes the binary demand symbol versions that
    # conservative targets like SteamOS do not have. Folding libstdc++ and
    # libgcc in statically drops our direct GLIBCXX_*/CXXABI_* requirements.
    # NOTE: it does NOT fix glibc itself (no link flag can -- ld.so is
    # version-locked to libc), nor the GLIBCXX needed by C++ shared libs we
    # bundle (SFML, fmt, SPIRV-Tools ...). A genuinely portable build has to
    # be produced against an older glibc, e.g. in a Steam Runtime container.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(moderngekko-run PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    add_custom_command(TARGET moderngekko-run POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Data/Sys"
                "$<TARGET_FILE_DIR:moderngekko-run>/Sys")

    add_executable(moderngekko-launcher
        tools/moderngekko_launcher.cpp
        tools/frontend_config.cpp
        vendor/dolphin/Externals/imgui/imgui/backends/imgui_impl_sdl3.cpp
        vendor/dolphin/Externals/imgui/imgui/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(moderngekko-launcher PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Externals/imgui/imgui/backends")
    # The launcher only calls the game-inspection surface directly, but the
    # Dolphin static libraries still require the Host_* callbacks implemented
    # in the runtime object. Keep that object from being discarded by the
    # static linker.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        target_link_libraries(moderngekko-launcher PRIVATE
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,moderngekko>"
            discio imgui ${_moderngekko_sdl_target})
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(moderngekko-launcher PRIVATE
            "-Wl,--whole-archive" moderngekko "-Wl,--no-whole-archive"
            discio imgui ${_moderngekko_sdl_target})
    else()
        target_link_libraries(moderngekko-launcher PRIVATE
            ModernGekko::Runtime discio imgui ${_moderngekko_sdl_target})
    endif()
    set_target_properties(moderngekko-launcher PROPERTIES OUTPUT_NAME "ModernGekko")
    if(MODERNGEKKO_FRONTEND_SDK)
        set_property(TARGET moderngekko-launcher PROPERTY BUILD_RPATH
            "${_moderngekko_sdk_lib}")
    endif()
    if(UNIX AND NOT APPLE)
        set_target_properties(moderngekko-run moderngekko-launcher PROPERTIES
            INSTALL_RPATH "$ORIGIN/lib")
    endif()
    add_dependencies(moderngekko-launcher moderngekko-run)

    add_executable(moderngekko-port tools/moderngekko_port.cpp)
    target_link_libraries(moderngekko-port PRIVATE ModernGekko::Runtime)
    target_compile_definitions(moderngekko-port PRIVATE
        MODERNGEKKO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(moderngekko-port dolrecomp moderngekko-run)
    add_custom_command(TARGET moderngekko-port POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:dolrecomp>"
                "$<TARGET_FILE_DIR:moderngekko-port>")
endif()

if(BUILD_TESTING)
    add_executable(moderngekko_frontend_config_test
        tests/frontend_config_test.cpp
        tools/frontend_config.cpp)
    target_include_directories(moderngekko_frontend_config_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools")
    target_compile_features(moderngekko_frontend_config_test PRIVATE cxx_std_23)
    # frontend_config.cpp enumerates gamepads through SDL. The SDL target only
    # exists with the Dolphin runtime enabled; without it the pad detection
    # compiles out and every caller falls back to a keyboard profile.
    if(DEFINED _moderngekko_sdl_target)
        target_link_libraries(moderngekko_frontend_config_test
            PRIVATE ${_moderngekko_sdl_target})
    else()
        target_compile_definitions(moderngekko_frontend_config_test
            PRIVATE MODERNGEKKO_NO_SDL_GAMEPADS)
    endif()
    add_test(NAME moderngekko.frontend_config COMMAND moderngekko_frontend_config_test)

    if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
        add_executable(moderngekko_netplay_protocol_test
            tests/netplay_protocol_test.cpp
            tools/netplay_compatibility.cpp)
        target_include_directories(moderngekko_netplay_protocol_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tools"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/GXRuntime/include")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE _ARCH_64=1)
        else()
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE _ARCH_32=1)
        endif()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x64|x86_64|amd64|AMD64")
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE _M_X86_64=1)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE _M_ARM_64=1)
        else()
            target_compile_definitions(moderngekko_netplay_protocol_test PRIVATE _M_GENERIC=1)
        endif()
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
            target_link_libraries(moderngekko_netplay_protocol_test PRIVATE
                "$<LINK_LIBRARY:WHOLE_ARCHIVE,moderngekko>" core uicommon)
        elseif(UNIX AND NOT APPLE)
            target_link_libraries(moderngekko_netplay_protocol_test PRIVATE
                "-Wl,--whole-archive" moderngekko "-Wl,--no-whole-archive" core uicommon)
        else()
            target_link_libraries(moderngekko_netplay_protocol_test PRIVATE
                ModernGekko::Runtime core uicommon)
        endif()
        add_test(NAME moderngekko.netplay_protocol COMMAND moderngekko_netplay_protocol_test)
    endif()

    if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
        # Which discs get a cheat list is decided by the packaging scripts; this
        # asserts the runtime plumbing they rely on, per region.
        add_executable(moderngekko_game_cheats_test tests/game_cheats_test.cpp)
        target_include_directories(moderngekko_game_cheats_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core")
        # Dolphin's headers use std::to_underlying, so this needs the same
        # standard the runtime is built with, not the default.
        target_compile_features(moderngekko_game_cheats_test PRIVATE cxx_std_23)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            target_compile_definitions(moderngekko_game_cheats_test PRIVATE _ARCH_64=1)
        else()
            target_compile_definitions(moderngekko_game_cheats_test PRIVATE _ARCH_32=1)
        endif()
        # Same link shape as the netplay test: `core` calls Host_* hooks that the
        # frontend provides, so the runtime archive has to come in whole.
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
            target_link_libraries(moderngekko_game_cheats_test PRIVATE
                "$<LINK_LIBRARY:WHOLE_ARCHIVE,moderngekko>" core uicommon)
        elseif(UNIX AND NOT APPLE)
            target_link_libraries(moderngekko_game_cheats_test PRIVATE
                "-Wl,--whole-archive" moderngekko "-Wl,--no-whole-archive" core uicommon)
        else()
            target_link_libraries(moderngekko_game_cheats_test PRIVATE
                ModernGekko::Runtime core uicommon)
        endif()
        add_test(NAME moderngekko.game_cheats COMMAND moderngekko_game_cheats_test)
    endif()

    if(MODERNGEKKO_ENABLE_DOLPHIN_RUNTIME)
        # The in-game menu, driven through its own keystroke API. No window and
        # no ImGui context: the actions live in OnKey, not Draw.
        add_executable(moderngekko_menu_test tests/menu_test.cpp)
        target_include_directories(moderngekko_menu_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core")
        target_compile_features(moderngekko_menu_test PRIVATE cxx_std_23)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            target_compile_definitions(moderngekko_menu_test PRIVATE _ARCH_64=1)
        else()
            target_compile_definitions(moderngekko_menu_test PRIVATE _ARCH_32=1)
        endif()
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
            target_link_libraries(moderngekko_menu_test PRIVATE
                "$<LINK_LIBRARY:WHOLE_ARCHIVE,moderngekko>" core uicommon videocommon)
        elseif(UNIX AND NOT APPLE)
            target_link_libraries(moderngekko_menu_test PRIVATE
                "-Wl,--whole-archive" moderngekko "-Wl,--no-whole-archive"
                core uicommon videocommon)
        else()
            target_link_libraries(moderngekko_menu_test PRIVATE
                ModernGekko::Runtime core uicommon videocommon)
        endif()
        add_test(NAME moderngekko.menu COMMAND moderngekko_menu_test)
    endif()

    add_executable(moderngekko_game_inspect_test tests/game_inspect_test.cpp)
    target_link_libraries(moderngekko_game_inspect_test PRIVATE ModernGekko::LegacyRuntime)
    add_test(NAME moderngekko.game_inspect COMMAND moderngekko_game_inspect_test)

    add_executable(moderngekko_module_abi_test
        tests/module_abi_test.c
    )
    target_link_libraries(moderngekko_module_abi_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.module_abi
             COMMAND moderngekko_module_abi_test)

    add_library(moderngekko_test_module SHARED
        tests/fixtures/test_module.c
    )
    target_link_libraries(moderngekko_test_module
        PRIVATE ModernGekko::ABI
    )
    set_target_properties(moderngekko_test_module PROPERTIES
        PREFIX ""
        OUTPUT_NAME "moderngekko_test_module"
    )
    add_library(moderngekko_missing_export_module SHARED
        tests/fixtures/missing_export.c)
    set_target_properties(moderngekko_missing_export_module PROPERTIES
        PREFIX "" OUTPUT_NAME "moderngekko_missing_export_module")

    add_executable(moderngekko_module_loader_test
        tests/module_loader_test.cpp
    )
    target_link_libraries(moderngekko_module_loader_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    target_compile_definitions(moderngekko_module_loader_test
        PRIVATE
            MODERNGEKKO_TEST_MODULE_PATH="$<TARGET_FILE:moderngekko_test_module>"
            MODERNGEKKO_MISSING_EXPORT_MODULE_PATH="$<TARGET_FILE:moderngekko_missing_export_module>"
    )
    add_dependencies(moderngekko_module_loader_test moderngekko_test_module
                     moderngekko_missing_export_module)
    add_test(NAME moderngekko.module_loader
             COMMAND moderngekko_module_loader_test)

    add_executable(moderngekko_cpu_primitives_test
        tests/cpu_primitives_test.cpp
    )
    target_link_libraries(moderngekko_cpu_primitives_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    target_include_directories(moderngekko_cpu_primitives_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin_legacy"
    )
    add_test(NAME moderngekko.cpu_primitives
             COMMAND moderngekko_cpu_primitives_test)

    add_executable(moderngekko_address_space_test
        tests/address_space_test.cpp
    )
    target_link_libraries(moderngekko_address_space_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.address_space
             COMMAND moderngekko_address_space_test)

    add_library(moderngekko_runtime_test_module SHARED
        tests/fixtures/runtime_test_module.c
    )
    target_link_libraries(moderngekko_runtime_test_module
        PRIVATE ModernGekko::ABI
    )
    set_target_properties(moderngekko_runtime_test_module PROPERTIES
        PREFIX ""
        OUTPUT_NAME "moderngekko_runtime_test_module"
    )

    add_executable(moderngekko_runtime_test
        tests/runtime_test.cpp
    )
    target_link_libraries(moderngekko_runtime_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    target_compile_definitions(moderngekko_runtime_test
        PRIVATE
            MODERNGEKKO_RUNTIME_TEST_MODULE_PATH="$<TARGET_FILE:moderngekko_runtime_test_module>"
    )
    add_dependencies(moderngekko_runtime_test moderngekko_runtime_test_module)
    add_test(NAME moderngekko.runtime
             COMMAND moderngekko_runtime_test)

    add_executable(moderngekko_dol_loader_test
        tests/dol_loader_test.cpp
    )
    target_link_libraries(moderngekko_dol_loader_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.dol_loader
             COMMAND moderngekko_dol_loader_test)

    add_executable(moderngekko_boot_environment_test
        tests/boot_environment_test.cpp
    )
    target_link_libraries(moderngekko_boot_environment_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.boot_environment
             COMMAND moderngekko_boot_environment_test)

    add_executable(moderngekko_gekko_definitions_test
        tests/gekko_definitions_test.cpp
    )
    target_link_libraries(moderngekko_gekko_definitions_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    target_include_directories(moderngekko_gekko_definitions_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin_legacy"
    )
    add_test(NAME moderngekko.gekko_definitions
             COMMAND moderngekko_gekko_definitions_test)

    add_executable(moderngekko_interpreter_test
        tests/interpreter_test.cpp
    )
    target_link_libraries(moderngekko_interpreter_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.interpreter
             COMMAND moderngekko_interpreter_test)

    add_executable(moderngekko_hardware_core_test
        tests/hardware_core_test.cpp
    )
    target_link_libraries(moderngekko_hardware_core_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.hardware_core
             COMMAND moderngekko_hardware_core_test)

    add_executable(moderngekko_io_services_test
        tests/io_services_test.cpp
    )
    target_link_libraries(moderngekko_io_services_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.io_services
             COMMAND moderngekko_io_services_test)

    add_executable(moderngekko_audio_system_test
        tests/audio_system_test.cpp
    )
    target_link_libraries(moderngekko_audio_system_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.audio_system
             COMMAND moderngekko_audio_system_test)

    add_executable(moderngekko_gx_commands_test
        tests/gx_commands_test.cpp
    )
    target_link_libraries(moderngekko_gx_commands_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_commands
             COMMAND moderngekko_gx_commands_test)

    add_executable(moderngekko_gx_state_test
        tests/gx_state_test.cpp
    )
    target_link_libraries(moderngekko_gx_state_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_state
             COMMAND moderngekko_gx_state_test)

    add_executable(moderngekko_gx_vertex_test
        tests/gx_vertex_test.cpp
    )
    target_link_libraries(moderngekko_gx_vertex_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_vertex
             COMMAND moderngekko_gx_vertex_test)

    add_executable(moderngekko_gx_conformance_test
        tests/gx_conformance_test.cpp
    )
    target_link_libraries(moderngekko_gx_conformance_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_conformance
             COMMAND moderngekko_gx_conformance_test)

    add_executable(moderngekko_gx_texture_test
        tests/gx_texture_test.cpp
    )
    target_link_libraries(moderngekko_gx_texture_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_texture
             COMMAND moderngekko_gx_texture_test)

    add_executable(moderngekko_gx_pipeline_test
        tests/gx_pipeline_test.cpp
    )
    target_link_libraries(moderngekko_gx_pipeline_test
        PRIVATE ModernGekko::LegacyRuntime
    )
    add_test(NAME moderngekko.gx_pipeline
             COMMAND moderngekko_gx_pipeline_test)

    add_executable(moderngekko_dolphin_shader_test
        tests/dolphin_shader_test.cpp
    )
    target_link_libraries(moderngekko_dolphin_shader_test
        PRIVATE ModernGekko::DolphinVideo
    )
    add_test(NAME moderngekko.dolphin_shader
             COMMAND moderngekko_dolphin_shader_test)

    # One target so CI can build the suite without either naming twenty-odd
    # executables (the list drifts, and a dropped name fails as a silent gap in
    # coverage) or falling back to `cmake --build`, which reaches the tests only
    # by also compiling the imgui launcher, its SDL dependencies and the port
    # tool -- none of which a test run needs.
    #
    # Discovered rather than listed: every test executable here ends in _test,
    # and DolRecomp's live in their own directory. The fixture shared objects
    # (moderngekko_test_module and friends) are already add_dependencies of the
    # tests that load them, so they come along.
    add_custom_target(moderngekko-tests)
    get_property(_moderngekko_test_targets
                 DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_dolrecomp_test_targets
                 DIRECTORY "${_moderngekko_dolrecomp}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _moderngekko_test_targets _dolrecomp_test_targets)
        if(_target MATCHES "_test$|^test_")
            add_dependencies(moderngekko-tests ${_target})
        endif()
    endforeach()
    # Not caught by the pattern, and not optional: the disc_extract test takes
    # $<TARGET_FILE:dolrecomp> as an argument and runs it, so building only the
    # test executables leaves that one failing on a missing binary.
    add_dependencies(moderngekko-tests dolrecomp)
endif()
