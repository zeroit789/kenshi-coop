# ES: KenshiMP.IntegrationTest: ejecutable de consola de pruebas de integración de red
#     contra el servidor (ENet + protocolo de Common).
# EN: KenshiMP.IntegrationTest: console executable for network integration tests
#     against the server (ENet + Common protocol).
add_executable(KenshiMP.IntegrationTest
    main.cpp
)

target_include_directories(KenshiMP.IntegrationTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KenshiMP.IntegrationTest PRIVATE
    KenshiMP.Common
    enet
    ws2_32
    winmm
)
