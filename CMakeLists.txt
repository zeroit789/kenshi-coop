# ES: CMake raíz de KenshiMP: configura C++17, carpetas de salida, opciones de Windows,
#     dependencias de terceros (lib/) y añade todos los subproyectos KenshiMP.*.
# EN: KenshiMP root CMake: sets C++17, output folders, Windows options,
#     third-party dependencies (lib/) and adds every KenshiMP.* subproject.
cmake_minimum_required(VERSION 3.20)
project(KenshiMP VERSION 0.1.0 LANGUAGES CXX C)

# ES: C++17 obligatorio; ejecutables, DLL y librerías dinámicas en build/bin, estáticas en build/lib.
# EN: C++17 required; executables, DLLs and shared libraries in build/bin, static ones in build/lib.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ES: Forzar x64 (kenshi_x64.exe es de 64 bits; la DLL tiene que serlo también)
# EN: Force x64
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "KenshiMP requires a 64-bit build")
endif()

# ES: Específico de Windows: <windows.h> reducido, sin macros min/max y sin avisos CRT "inseguros".
# EN: Windows-specific
add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)

# ES: Forzar el layout de contenedores de Release incluso en Debug.
#     Las DLL de Kenshi (MyGUI, Ogre, etc.) son builds Release: su std::string/vector no
#     tiene puntero _Container_proxy (24/32 bytes). Debug añade un proxy de 8 bytes y
#     rompe el ABI al pasar contenedores entre DLL.
# EN: Force Release-mode container layout even in Debug builds.
#     Kenshi's DLLs (MyGUI, Ogre, etc.) are Release builds — their std::string/vector
#     have no _Container_proxy pointer (24/32 bytes). Debug mode adds an 8-byte proxy,
#     causing ABI mismatch when passing containers across DLL boundaries.
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# ES: ── Dependencias de terceros ──
# EN: ── Third-party dependencies ──

# ES: ENet (red UDP; el proyecto original no exporta sus includes a quien lo usa, se añaden aquí)
# EN: ENet (upstream doesn't export include dirs to consumers)
add_subdirectory(lib/enet)
target_include_directories(enet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib/enet/include)

# ES: MinHook (librería para instalar hooks en funciones del juego)
# EN: MinHook
add_subdirectory(lib/minhook)

# ES: spdlog (logging)
# EN: spdlog (header-only mode)
add_subdirectory(lib/spdlog EXCLUDE_FROM_ALL)

# ES: nlohmann/json (solo cabecera; sin compilar sus tests)
# EN: nlohmann/json (header-only)
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(lib/json EXCLUDE_FROM_ALL)


# ES: ── Nuestros proyectos ──
#     Common = protocolo/tipos compartidos, Scanner = escáner de patrones y hooks,
#     Core = DLL/plugin de Ogre dentro de Kenshi, Server = servidor dedicado,
#     MasterServer = registro de servidores, Injector = lanzador, y herramientas de test.
# EN: ── Our projects ──
#     Common = shared protocol/types, Scanner = pattern scanner and hooks,
#     Core = DLL/Ogre plugin inside Kenshi, Server = dedicated server,
#     MasterServer = server registry, Injector = launcher, plus test tools.
add_subdirectory(KenshiMP.Common)
add_subdirectory(KenshiMP.Scanner)
add_subdirectory(KenshiMP.Core)
add_subdirectory(KenshiMP.Server)
add_subdirectory(KenshiMP.MasterServer)
add_subdirectory(KenshiMP.Injector)
add_subdirectory(KenshiMP.TestClient)
add_subdirectory(KenshiMP.IntegrationTest)
add_subdirectory(KenshiMP.UnitTest)
add_subdirectory(KenshiMP.LiveTest)
