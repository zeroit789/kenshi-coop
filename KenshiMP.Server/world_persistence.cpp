// ES: world_persistence.cpp - Guardado y carga del mundo del servidor en JSON (formato versión 2).
//     Guarda hora, clima, todas las entidades (tipo, dueño, plantilla, facción, posición, rotación,
//     vida por parte del cuerpo, nombre de plantilla y equipo) y el mapa nombre de jugador ->
//     entidades, para que un jugador que reconecta recupere las suyas. Escritura atómica.
// EN: world_persistence.cpp - Saving and loading the server world as JSON (format version 2).
//     Stores time, weather, every entity (type, owner, template, faction, position, rotation,
//     per-body-part health, template name and equipment) and the player name -> entities map,
//     so a reconnecting player gets theirs back. Atomic write.
#include "server.h"
#include "kmp/constants.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdio>
#include <cmath>
#include <spdlog/spdlog.h>

// ES: windows.h solo hace falta para MoveFileExA (reemplazo atómico del fichero).
// EN: windows.h is only needed for MoveFileExA (atomic file replacement).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace kmp {

using json = nlohmann::json;

// ES: Guarda/carga el estado del mundo en JSON para que un servidor (p. ej. en un VPS) lo conserve entre reinicios.
// EN: Save/load world state to JSON for the dedicated server.
// This allows VPS-hosted servers to persist world state between restarts.

// ES: Serializa el mundo a JSON y lo escribe en 'path'. Devuelve false si no se pudo escribir.
// EN: Serializes the world to JSON and writes it to 'path'. Returns false if it could not be written.
bool SaveWorldToFile(const std::string& path,
                     const std::unordered_map<EntityID, ServerEntity>& entities,
                     const std::unordered_map<std::string, SavedPlayer>& savedPlayers,
                     float timeOfDay, int weatherState) {
    json j;
    j["version"] = 2;
    j["timeOfDay"] = timeOfDay;
    j["weather"] = weatherState;

    // ES: Una entrada por entidad. La rotación se guarda como [w, x, y, z]; health tiene 7 partes
    //     del cuerpo y equipment 14 ranuras.
    // EN: One entry per entity. Rotation is stored as [w, x, y, z]; health has 7 body parts and
    //     equipment 14 slots.
    json entityArray = json::array();
    for (auto& [id, entity] : entities) {
        json e;
        e["id"] = entity.id;
        e["type"] = static_cast<int>(entity.type);
        e["owner"] = entity.owner;
        e["templateId"] = entity.templateId;
        e["factionId"] = entity.factionId;
        e["position"] = {entity.position.x, entity.position.y, entity.position.z};
        e["rotation"] = {entity.rotation.w, entity.rotation.x, entity.rotation.y, entity.rotation.z};
        e["alive"] = entity.alive;

        json healthArr = json::array();
        for (int i = 0; i < 7; i++) healthArr.push_back(entity.health[i]);
        e["health"] = healthArr;

        e["templateName"] = entity.templateName;

        json equipArr = json::array();
        for (int i = 0; i < 14; i++) equipArr.push_back(entity.equipment[i]);
        e["equipment"] = equipArr;

        entityArray.push_back(e);
    }
    j["entities"] = entityArray;

    // ES: Guarda el mapa jugador -> entidades para que al reconectar reclame sus entidades.
    // EN: Save player→entity mapping so reconnecting players can reclaim entities
    json playersObj = json::object();
    for (auto& [name, sp] : savedPlayers) {
        json ids = json::array();
        for (EntityID eid : sp.entityIds) ids.push_back(eid);
        playersObj[name] = ids;
    }
    j["players"] = playersObj;

    // ES: Escribe primero en un fichero temporal (.tmp) y luego reemplaza el destino de forma atómica:
    //     si el proceso muere en cualquier punto, en disco queda entero el fichero viejo o el nuevo.
    // EN: Write to temp file first, then atomically replace the destination.
    // This ensures no data-loss window: if we crash at any point, either
    // the old file or the new file exists in full on disk.
    std::string tmpPath = path + ".tmp";
    {
        std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            spdlog::error("SaveWorld: Failed to open '{}'", tmpPath);
            return false;
        }
        file << j.dump(2);
        file.flush();
        if (!file.good()) {
            spdlog::error("SaveWorld: Write/flush failed for '{}'", tmpPath);
            file.close();
            std::remove(tmpPath.c_str());
            return false;
        }
        file.close();
    }

    // ES: MoveFileExA con MOVEFILE_REPLACE_EXISTING es atómico en NTFS (una sola operación) y
    //     MOVEFILE_WRITE_THROUGH espera a que el movimiento llegue a disco.
    // EN: MoveFileExA with MOVEFILE_REPLACE_EXISTING is atomic on NTFS:
    // it replaces the destination in a single filesystem operation.
    // MOVEFILE_WRITE_THROUGH ensures the move is flushed to disk before returning.
    if (!MoveFileExA(tmpPath.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        spdlog::error("SaveWorld: MoveFileExA failed (error {}), falling back to manual rename", err);

        // ES: Plan B si MoveFileExA falla: viejo -> .bak, temporal -> destino. No es del todo atómico,
        //     pero conserva el viejo como copia en vez de borrarlo primero.
        // EN: Fallback: rotate old -> backup, temp -> target.
        // Not fully atomic, but still safer than remove-then-rename because
        // the old file is preserved as a backup rather than deleted first.
        std::string backupPath = path + ".bak";
        std::remove(backupPath.c_str());

        // ES: Mueve el fichero actual a la copia (no pasa nada si aún no existe).
        // EN: Move current file to backup (OK if it doesn't exist yet)
        std::rename(path.c_str(), backupPath.c_str());

        if (std::rename(tmpPath.c_str(), path.c_str()) != 0) {
            spdlog::error("SaveWorld: Fallback rename '{}' -> '{}' also failed", tmpPath, path);
            // ES: Intenta restaurar la copia para no perder el estado anterior.
            // EN: Try to restore backup so we don't lose the old state
            std::rename(backupPath.c_str(), path.c_str());
            return false;
        }
        // ES: Si salió bien, borra la copia.
        // EN: Clean up backup on success
        std::remove(backupPath.c_str());
    }

    spdlog::info("SaveWorld: Saved {} entities to '{}'", entities.size(), path);
    return true;
}

// ES: Carga el mundo desde 'path'. Todas las entidades quedan con owner = 0 (sin dueño) hasta
//     que su jugador reconecte. Limita a KMP_MAX_SYNC_ENTITIES, descarta posiciones inválidas y
//     calcula nextEntityId = id máximo + 1. Devuelve false si no existe o el JSON es inválido
//     (atrapa la excepción, no revienta).
// EN: Loads the world from 'path'. Every entity gets owner = 0 (unowned) until its player
//     reconnects. Caps at KMP_MAX_SYNC_ENTITIES, drops invalid positions and computes
//     nextEntityId = max id + 1. Returns false if missing or the JSON is invalid (the exception
//     is caught, it does not crash).
bool LoadWorldFromFile(const std::string& path,
                       std::unordered_map<EntityID, ServerEntity>& entities,
                       std::unordered_map<std::string, SavedPlayer>& savedPlayers,
                       float& timeOfDay, int& weatherState,
                       EntityID& nextEntityId) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    try {
        json j;
        file >> j;

        timeOfDay = j.value("timeOfDay", 0.5f);
        weatherState = j.value("weather", 0);

        entities.clear();
        savedPlayers.clear();
        EntityID maxId = 0;

        int loadedCount = 0;
        int skippedBadPos = 0;
        for (auto& e : j["entities"]) {
            if (loadedCount >= KMP_MAX_SYNC_ENTITIES) {
                spdlog::warn("LoadWorld: Entity cap reached ({} max), skipping remaining",
                             KMP_MAX_SYNC_ENTITIES);
                break;
            }
            ServerEntity entity;
            entity.id = e["id"];
            entity.type = static_cast<EntityType>(e["type"].get<int>());
            entity.owner = 0; // Mark all loaded entities as unowned until a player reconnects
            entity.templateId = e["templateId"];
            entity.factionId = e["factionId"];

            auto& pos = e["position"];
            entity.position = Vec3(pos[0], pos[1], pos[2]);

            // ES: Valida la posición: salta entidades con NaN, infinito o coordenadas extremas (harían petar al cliente).
            // EN: Validate position — skip entities with NaN, inf, or extreme coordinates
            if (std::isnan(entity.position.x) || std::isnan(entity.position.y) || std::isnan(entity.position.z) ||
                std::isinf(entity.position.x) || std::isinf(entity.position.y) || std::isinf(entity.position.z) ||
                std::abs(entity.position.x) > 1000000.f || std::abs(entity.position.y) > 1000000.f ||
                std::abs(entity.position.z) > 1000000.f) {
                skippedBadPos++;
                spdlog::warn("LoadWorld: Skipping entity {} — bad position ({:.1f}, {:.1f}, {:.1f})",
                             entity.id, entity.position.x, entity.position.y, entity.position.z);
                // ES: Aun así se cuenta su ID para que no se reutilice y choque.
                // EN: Still track max ID to prevent collisions
                if (entity.id > maxId) maxId = entity.id;
                continue;
            }

            auto& rot = e["rotation"];
            entity.rotation = Quat(rot[0], rot[1], rot[2], rot[3]);

            entity.zone = ZoneCoord::FromWorldPos(entity.position);
            entity.alive = e.value("alive", true);

            auto& health = e["health"];
            for (int i = 0; i < 7 && i < static_cast<int>(health.size()); i++) {
                entity.health[i] = health[i];
            }

            entity.templateName = e.value("templateName", std::string{});

            if (e.contains("equipment")) {
                auto& equip = e["equipment"];
                for (int i = 0; i < 14 && i < static_cast<int>(equip.size()); i++) {
                    entity.equipment[i] = equip[i];
                }
            }

            entities[entity.id] = entity;
            if (entity.id > maxId) maxId = entity.id;
            loadedCount++;
        }

        // ES: Carga el mapa jugador -> entidades (versión 2+), quedándose solo con entidades que sí se cargaron.
        // EN: Load player→entity mapping (version 2+)
        if (j.contains("players") && j["players"].is_object()) {
            for (auto& [name, ids] : j["players"].items()) {
                SavedPlayer sp;
                sp.name = name;
                for (auto& eid : ids) {
                    EntityID id = eid.get<EntityID>();
                    // ES: Solo referencias a entidades cargadas de verdad.
                    // EN: Only keep references to entities that actually loaded
                    if (entities.count(id)) sp.entityIds.push_back(id);
                }
                if (!sp.entityIds.empty()) {
                    savedPlayers[name] = std::move(sp);
                }
            }
            spdlog::info("LoadWorld: Loaded {} saved player records", savedPlayers.size());
        }

        nextEntityId = maxId + 1;
        if (skippedBadPos > 0) {
            spdlog::warn("LoadWorld: Skipped {} entities with bad positions", skippedBadPos);
        }
        spdlog::info("LoadWorld: Loaded {} valid entities from '{}'", entities.size(), path);
        return true;
    } catch (const std::exception& ex) {
        spdlog::error("LoadWorld: Failed to parse '{}': {}", path, ex.what());
        return false;
    }
}

} // namespace kmp
