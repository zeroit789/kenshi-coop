# ES: KenshiMP.MasterServer: ejecutable de consola del registro central de servidores
#     (navegador de servidores) sobre ENet.
# EN: KenshiMP.MasterServer: console executable for the central server registry
#     (server browser) over ENet.
add_executable(KenshiMP.MasterServer
    main.cpp
)

target_include_directories(KenshiMP.MasterServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KenshiMP.MasterServer PRIVATE
    KenshiMP.Common
    enet
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ws2_32
    winmm
)
