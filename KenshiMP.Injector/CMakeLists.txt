# ES: KenshiMP.Injector: lanzador gráfico (aplicación WIN32 sin consola) que instala el mod
#     como plugin de Ogre, escribe client.json y arranca Kenshi. Compilado en modo UNICODE.
# EN: KenshiMP.Injector: graphical launcher (WIN32 app, no console) that installs the mod
#     as an Ogre plugin, writes client.json and starts Kenshi. Built in UNICODE mode.
add_executable(KenshiMP.Injector WIN32
    main.cpp
    injector.cpp
    process.cpp
    resource.rc
)

target_include_directories(KenshiMP.Injector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KenshiMP.Injector PRIVATE
    KenshiMP.Common
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    shlwapi
    comctl32
)

target_compile_definitions(KenshiMP.Injector PRIVATE UNICODE _UNICODE)
