cmake_minimum_required(VERSION 3.20)
project(KenshiMP_Rebuild VERSION 0.2.0 LANGUAGES CXX C)

# ─────────────────────────────────────────────────────────────────────────────
# Kenshi-Online CLEAN REBUILD — Phase 1 vertical slice.
#
# This tree builds two NEW targets that do NOT collide with the old tree:
#     KenshiMP.Core2   (SHARED / Ogre plugin DLL)   ← src/
#     KenshiMP.Server2 (EXE / dedicated server)     ← server/
#
# It REUSES IN PLACE the proven, already-building shared static libs from the
# old tree — KenshiMP.Common and KenshiMP.Scanner — and the lib/ submodules
# (enet, minhook, spdlog, json). Nothing in the old tree is modified.
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Force x64 (ABI match with Kenshi's 64-bit release binaries)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "KenshiMP rebuild requires a 64-bit build")
endif()

# Windows + ABI defines — IDENTICAL to the old root CMake so container layout
# (std::string/std::vector) matches Kenshi's Release DLLs. _ITERATOR_DEBUG_LEVEL=0
# strips the 8-byte _Container_proxy that would break cross-DLL container passing.
add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# Absolute path to the old tree (the salvage / reuse source root).
set(KMP_OLD_ROOT "C:/Program Files (x86)/Steam/steamapps/common/Kenshi/KenshiMP")

# ── Third-party dependencies (reuse the old tree's lib/ submodules in place) ──
# Each add_subdirectory(SOURCE BINARY) gives an explicit binary dir so the build
# artifacts land under THIS build tree, never inside the read-only old tree.

add_subdirectory("${KMP_OLD_ROOT}/lib/enet"    "${CMAKE_BINARY_DIR}/enet_build")
target_include_directories(enet PUBLIC "${KMP_OLD_ROOT}/lib/enet/include")

add_subdirectory("${KMP_OLD_ROOT}/lib/minhook" "${CMAKE_BINARY_DIR}/minhook_build")

add_subdirectory("${KMP_OLD_ROOT}/lib/spdlog"  "${CMAKE_BINARY_DIR}/spdlog_build" EXCLUDE_FROM_ALL)

set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory("${KMP_OLD_ROOT}/lib/json"    "${CMAKE_BINARY_DIR}/json_build"   EXCLUDE_FROM_ALL)

# ── Proven shared static libs (REUSE IN PLACE — do NOT copy or rewrite) ──
# Explicit binary dirs keep their artifacts in this build tree.
add_subdirectory("${KMP_OLD_ROOT}/KenshiMP.Common"  "${CMAKE_BINARY_DIR}/common_build")
add_subdirectory("${KMP_OLD_ROOT}/KenshiMP.Scanner" "${CMAKE_BINARY_DIR}/scanner_build")

# ═════════════════════════════════════════════════════════════════════════════
#  KenshiMP.Core2 — the client DLL (Ogre plugin)
# ═════════════════════════════════════════════════════════════════════════════
file(GLOB_RECURSE CORE2_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(KenshiMP.Core2 SHARED ${CORE2_SOURCES})

target_include_directories(KenshiMP.Core2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(KenshiMP.Core2 PRIVATE
    KenshiMP.Common
    KenshiMP.Scanner
    enet
    minhook
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    d3dcompiler
)
target_compile_definitions(KenshiMP.Core2 PRIVATE KMP_EXPORTS)
target_link_options(KenshiMP.Core2 PRIVATE /MAP)

# ═════════════════════════════════════════════════════════════════════════════
#  KenshiMP.Server2 — the dedicated server EXE
# ═════════════════════════════════════════════════════════════════════════════
file(GLOB_RECURSE SERVER2_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/server/*.cpp"
)

add_executable(KenshiMP.Server2 ${SERVER2_SOURCES})

target_include_directories(KenshiMP.Server2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(KenshiMP.Server2 PRIVATE
    KenshiMP.Common
    enet
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ws2_32
    winmm
    ole32
    oleaut32
    iphlpapi
)
