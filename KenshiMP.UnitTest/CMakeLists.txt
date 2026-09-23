# ES: KenshiMP.UnitTest: tests unitarios que compilan directamente fuentes del Core
#     (acceso a personajes, registro de entidades, interpolación) y las prueban sobre
#     memoria falsa, sin cargar Kenshi.
# EN: KenshiMP.UnitTest: unit tests that compile Core sources directly (character access,
#     entity registry, interpolation) and exercise them on fake memory, without Kenshi.
add_executable(KenshiMP.UnitTest
    main.cpp

    # ES: Compilar dentro del test los fuentes que tocan el juego.
    #     Son los MISMOS ficheros que corren dentro de Kenshi en KenshiMP.Core.dll.
    # EN: Compile game-facing source files directly into the test.
    #     These are the SAME files that run inside Kenshi via KenshiMP.Core.dll.
    ${CMAKE_SOURCE_DIR}/KenshiMP.Core/game/game_character.cpp
    ${CMAKE_SOURCE_DIR}/KenshiMP.Core/game/game_offset_prober.cpp
    ${CMAKE_SOURCE_DIR}/KenshiMP.Core/sync/entity_registry.cpp
    ${CMAKE_SOURCE_DIR}/KenshiMP.Core/sync/interpolation.cpp

    # ES: Stubs de SpawnManager::ReadKenshiString y ai_hooks::IsRemoteControlled,
    #     que game_character.cpp necesita pero cuyas unidades no se compilan en el test.
    # EN: Stubs for SpawnManager::ReadKenshiString and ai_hooks::IsRemoteControlled,
    #     needed by game_character.cpp but whose units aren't built into the test.
    test_stubs.cpp
)

# ES: Rutas de include para que los fuentes del Core encuentren sus cabeceras.
# EN: Include paths so the Core sources find their headers.
target_include_directories(KenshiMP.UnitTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/KenshiMP.Core          # for game/game_types.h, sync/entity_registry.h
    ${CMAKE_SOURCE_DIR}/KenshiMP.Core/game      # for game_types.h direct includes
    ${CMAKE_SOURCE_DIR}/KenshiMP.Core/sync      # for entity_registry.h, interpolation.h
)

target_link_libraries(KenshiMP.UnitTest PRIVATE
    KenshiMP.Common
    KenshiMP.Scanner
    spdlog::spdlog
)
