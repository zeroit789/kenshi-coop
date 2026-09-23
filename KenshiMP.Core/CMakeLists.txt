# ES: KenshiMP.Core: la DLL del mod. Kenshi la carga como plugin de Ogre (Plugins_x64.cfg);
#     instala los hooks sobre funciones del juego, sincroniza el estado por red con el
#     servidor y dibuja la interfaz con MyGUI.
# EN: KenshiMP.Core: the mod DLL. Kenshi loads it as an Ogre plugin (Plugins_x64.cfg);
#     it installs hooks on game functions, syncs state over the network with the server
#     and draws the UI with MyGUI.
add_library(KenshiMP.Core SHARED
    dllmain.cpp
    core.cpp
    # ES: Hooks (desvíos de funciones del juego hacia las nuestras)
    # EN: Hooks
    hooks/render_hooks.cpp
    hooks/input_hooks.cpp
    hooks/entity_hooks.cpp
    hooks/movement_hooks.cpp
    hooks/combat_hooks.cpp
    hooks/world_hooks.cpp
    hooks/save_hooks.cpp
    hooks/time_hooks.cpp
    hooks/game_tick_hooks.cpp
    hooks/inventory_hooks.cpp
    hooks/squad_hooks.cpp
    hooks/faction_hooks.cpp
    hooks/building_hooks.cpp
    hooks/ai_hooks.cpp
    hooks/resource_hooks.cpp
    hooks/squad_spawn_hooks.h
    hooks/squad_spawn_hooks.cpp
    hooks/char_tracker_hooks.h
    hooks/char_tracker_hooks.cpp
    # ES: Envoltorios de estructuras del juego (personajes, escuadras, mundo, inventario...)
    # EN: Game wrappers
    game/game_character.cpp
    game/game_squad.cpp
    game/game_world.cpp
    game/game_inventory.cpp
    game/game_building.cpp
    game/game_faction.cpp
    game/game_stats.cpp
    game/game_offset_prober.cpp
    game/spawn_manager.cpp
    game/player_controller.cpp
    game/loading_orchestrator.cpp
    game/asset_facilitator.cpp
    game/lobby_manager.h
    game/lobby_manager.cpp
    game/shared_save_sync.cpp
    # ES: Red (cliente ENet, manejo de paquetes, consulta de servidores)
    # EN: Networking
    net/client.cpp
    net/packet_handler.cpp
    net/server_query.cpp
    # ES: Sincronización (registro de entidades, interpolación, propiedad, zonas, orquestadores)
    # EN: Sync
    sync/entity_registry.cpp
    sync/interpolation.cpp
    sync/ownership.cpp
    sync/zone_interest.cpp
    sync/entity_resolver.cpp
    sync/zone_engine.cpp
    sync/player_engine.cpp
    sync/sync_orchestrator.cpp
    sync/pipeline_orchestrator.cpp
    sync/sync_facilitator.cpp
    # FIX (2026-06-17):
    # ES: estos 3 .cpp existian pero faltaban en el build del cliente.
    #     Son los fixes de Steam de junio (cola de spawn diferida, cola de snapshots pendientes
    #     y validador de autoridad). Sin ellos el KenshiMP.Core.dll no enlazaba (LNK1120) y por eso
    #     el codigo nuevo NUNCA se habia llegado a compilar (los binarios de release eran viejos).
    # EN: these 3 .cpp files existed but were missing from the client build.
    #     They are the June Steam fixes (deferred spawn queue, pending snapshot queue and
    #     authority validator). Without them KenshiMP.Core.dll didn't link (LNK1120), so the
    #     new code had NEVER actually been compiled (release binaries were stale).
    sync/deferred_spawn_queue.cpp
    sync/pending_snapshot_queue.cpp
    sync/authority_validator.cpp
    # ES: SDK (estado del juego por sondeo + proxy visual)
    # EN: SDK (polling-based game state + visual proxy)
    sdk/kenshi_sdk.cpp
    sdk/visual_proxy.cpp
    # ES: Sistema / orquestador de tareas y comandos
    # EN: System / orchestrator
    sys/task_orchestrator.cpp
    sys/command_registry.cpp
    sys/builtin_commands.cpp
    # ES: Interfaz (solo MyGUI nativo del juego)
    # EN: UI (native MyGUI only)
    ui/overlay.cpp
    ui/mygui_bridge.cpp
    ui/native_menu.cpp
    ui/native_hud.cpp
)

# ES: Includes y librerías: Common/Scanner propias, ENet, spdlog, json y DirectX 11.
# EN: Includes and libraries: our Common/Scanner, ENet, spdlog, json and DirectX 11.
target_include_directories(KenshiMP.Core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KenshiMP.Core PRIVATE
    KenshiMP.Common
    KenshiMP.Scanner
    enet
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    d3dcompiler
)

# ES: Exportar los símbolos del plugin de Ogre (dllStartPlugin/dllStopPlugin)
# EN: Export Ogre plugin symbols
target_compile_definitions(KenshiMP.Core PRIVATE KMP_EXPORTS)

# ES: Generar fichero .map para depurar crasheos (dirección → función)
# EN: Generate MAP file for crash debugging
target_link_options(KenshiMP.Core PRIVATE /MAP)

# ES: Copiar la DLL automáticamente a la carpeta de Kenshi para que Ogre la cargue como plugin.
#     Supone que el repo está DENTRO de la carpeta del juego (KENSHI_DIR = carpeta padre);
#     si allí no hay kenshi_x64.exe no se copia nada.
# EN: Auto-copy DLL to Kenshi game directory so Ogre can load it as a plugin
#     Assumes the repo lives INSIDE the game folder (KENSHI_DIR = parent folder);
#     if there's no kenshi_x64.exe there, nothing is copied.
set(KENSHI_DIR "${CMAKE_SOURCE_DIR}/..")
if(EXISTS "${KENSHI_DIR}/kenshi_x64.exe")
    add_custom_command(TARGET KenshiMP.Core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:KenshiMP.Core>"
            "${KENSHI_DIR}/KenshiMP.Core.dll"
        COMMENT "Copying KenshiMP.Core.dll to Kenshi directory"
    )

    # ES: Desplegar los layouts de MyGUI (dist/*.layout) en data/gui/layout del juego
    # EN: Auto-deploy MyGUI layout files to Kenshi data directories
    set(LAYOUT_SRC "${CMAKE_SOURCE_DIR}/dist")
    set(LAYOUT_DST "${KENSHI_DIR}/data/gui/layout")
    foreach(LAYOUT_FILE Kenshi_MainMenu.layout Kenshi_MultiplayerPanel.layout Kenshi_MultiplayerHUD.layout)
        if(EXISTS "${LAYOUT_SRC}/${LAYOUT_FILE}")
            add_custom_command(TARGET KenshiMP.Core POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LAYOUT_SRC}/${LAYOUT_FILE}"
                    "${LAYOUT_DST}/${LAYOUT_FILE}"
                COMMENT "Deploying ${LAYOUT_FILE} to Kenshi data"
            )
        endif()
    endforeach()
endif()
