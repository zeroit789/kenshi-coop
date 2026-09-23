# ES: KenshiMP.Server: servidor dedicado autoritativo (ENet/UDP): jugadores, entidades,
#     zonas, combate, persistencia del mundo, UPnP y validación de autoridad.
# EN: KenshiMP.Server: authoritative dedicated server (ENet/UDP): players, entities,
#     zones, combat, world persistence, UPnP and authority validation.
add_executable(KenshiMP.Server
    main.cpp
    server.cpp
    upnp.cpp
    game_state.cpp
    player_manager.cpp
    entity_manager.cpp
    zone_manager.cpp
    combat_resolver.cpp
    world_persistence.cpp
    authority_validator.cpp
)
# FIX (2026-06-17):
# ES: authority_validator.cpp faltaba en la lista de fuentes del
#     servidor. Sin el, el linker no resolvia ServerAuthorityValidator::CanClientCommandEntity
#     ni ::ValidatePositionUpdate (errores LNK2019/LNK1120). Anadido para que el servidor enlace.
# EN: authority_validator.cpp was missing from the server's source list. Without it the
#     linker couldn't resolve ServerAuthorityValidator::CanClientCommandEntity or
#     ::ValidatePositionUpdate (LNK2019/LNK1120 errors). Added so the server links.

# ES: Librerías: Common, ENet, spdlog, json, Winsock, winmm, COM/OLE (UPnP) e iphlpapi (interfaces de red).
# EN: Libraries: Common, ENet, spdlog, json, Winsock, winmm, COM/OLE (UPnP) and iphlpapi (network interfaces).
target_include_directories(KenshiMP.Server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KenshiMP.Server PRIVATE
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

# ES: Copiar el exe del servidor a la carpeta de Kenshi para que el botón "Host" lo encuentre
#     (igual que en Core: supone el repo dentro de la carpeta del juego)
# EN: Copy server exe to Kenshi directory so the Host button can find it
#     (same as Core: assumes the repo lives inside the game folder)
get_filename_component(KENSHI_DIR "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
if(EXISTS "${KENSHI_DIR}/kenshi_x64.exe")
    add_custom_command(TARGET KenshiMP.Server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:KenshiMP.Server>"
            "${KENSHI_DIR}/KenshiMP.Server.exe"
        COMMENT "Copying KenshiMP.Server.exe to Kenshi directory"
    )
endif()
