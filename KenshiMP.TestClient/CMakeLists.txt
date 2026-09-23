# ES: KenshiMP.TestClient: cliente de consola que se conecta al servidor como jugador falso
#     (sirve para probar el multijugador sin una segunda instancia de Kenshi).
# EN: KenshiMP.TestClient: console client that connects to the server as a fake player
#     (used to test multiplayer without a second Kenshi instance).
add_executable(KenshiMP.TestClient
    main.cpp
)

target_include_directories(KenshiMP.TestClient PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KenshiMP.TestClient PRIVATE
    KenshiMP.Common
    enet
    ws2_32
    winmm
)
