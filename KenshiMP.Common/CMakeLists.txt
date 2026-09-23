# ES: KenshiMP.Common: librería estática compartida por todos los proyectos (protocolo,
#     mensajes, tipos, compresión y configuración JSON). Exporta include/ a quien la enlace.
# EN: KenshiMP.Common: static library shared by every project (protocol, messages,
#     types, compression and JSON config). Exports include/ to its consumers.
add_library(KenshiMP.Common STATIC
    src/protocol.cpp
    src/serialization.cpp
    src/compression.cpp
    src/config.cpp
)

target_include_directories(KenshiMP.Common PUBLIC include)
target_link_libraries(KenshiMP.Common PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog)
