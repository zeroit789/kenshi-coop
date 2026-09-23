# ES: KenshiMP.LiveTest: test de integración en vivo que lanza servidor, TestClient y Kenshi
#     y vigila sus logs. Enlaza con Shlwapi/Version/Shell32 para rutas, versiones y ShellExecute.
#     (El include de Common es redundante: ya llega vía KenshiMP.Common PUBLIC.)
# EN: KenshiMP.LiveTest: live integration test that launches server, TestClient and Kenshi
#     and watches their logs. Links Shlwapi/Version/Shell32 for paths, versions and ShellExecute.
#     (The Common include is redundant: it already comes via KenshiMP.Common PUBLIC.)
project(KenshiMP.LiveTest LANGUAGES CXX)

add_executable(KenshiMP.LiveTest main.cpp)

target_link_libraries(KenshiMP.LiveTest PRIVATE
    KenshiMP.Common
    spdlog::spdlog
    Shlwapi.lib
    Version.lib
    Shell32.lib
)

target_include_directories(KenshiMP.LiveTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../KenshiMP.Common/include
)

set_target_properties(KenshiMP.LiveTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
