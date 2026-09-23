// ES: Gestor de propiedad (ownership) de entidades en el cliente: quién controla
//     cada entidad. OJO: esta clase está definida solo en este .cpp, sin cabecera, y
//     no se usa en ningún otro sitio del código (código muerto); la propiedad la
//     gestiona hoy EntityResolver (ver entity_resolver.h, "replaces OwnershipManager").
// EN: Client-side entity ownership manager: who controls each entity.
//     NOTE: this class is defined only in this .cpp, with no header, and is not used
//     anywhere else in the code (dead code); ownership is handled today by
//     EntityResolver (see entity_resolver.h, "replaces OwnershipManager").
#include "entity_registry.h"
#include "kmp/types.h"
#include <spdlog/spdlog.h>

namespace kmp {

// ES: Gestión de propiedad: determina quién controla cada entidad.
//     Reglas de propiedad:
//     - Personajes de jugador: del jugador que los controla
//     - NPCs: del servidor (host)
//     - Edificios: del jugador que los colocó (o del servidor si son del mundo)
//     - Objetos en el suelo: del servidor hasta que alguien los recoge
//     Cuando un jugador se desconecta, sus entidades pasan al servidor.
//
// EN:
// Ownership management: determines who controls each entity.
//
// Ownership rules:
// - Player characters: owned by the player who controls them
// - NPCs: owned by the server (host)
// - Buildings: owned by the player who placed them (or server for world buildings)
// - Items on ground: owned by server until picked up
//
// When a player disconnects, their entities transfer to the server.

// ES: Singleton que consulta el EntityRegistry para responder preguntas de propiedad.
// EN: Singleton that queries the EntityRegistry to answer ownership questions.
class OwnershipManager {
public:
    // ES: Devuelve la instancia única.
    // EN: Returns the single instance.
    static OwnershipManager& Get() {
        static OwnershipManager instance;
        return instance;
    }

    // ES: ¿El jugador local es dueño de esta entidad?
    // Check if the local player owns this entity
    bool IsLocallyOwned(EntityID entityId) const {
        auto infoCopy = m_registry->GetInfo(entityId);
        if (!infoCopy) return false;
        return infoCopy->ownerPlayerId == m_localPlayerId;
    }

    // ES: ¿El servidor (host, dueño 0) es dueño de esta entidad?
    // Check if the server (host) owns this entity
    bool IsServerOwned(EntityID entityId) const {
        auto infoCopy = m_registry->GetInfo(entityId);
        if (!infoCopy) return false;
        return infoCopy->ownerPlayerId == 0;
    }

    // ES: Transfiere al servidor (dueño 0) todas las entidades de un jugador.
    // Transfer ownership of all entities from one player to server (owner=0)
    void TransferToServer(PlayerID playerId) {
        auto entities = m_registry->GetPlayerEntities(playerId);
        for (EntityID id : entities) {
            m_registry->UpdateOwner(id, 0);
            spdlog::info("Ownership: Transferred entity {} from player {} to server", id, playerId);
        }
        if (!entities.empty()) {
            spdlog::info("Ownership: Transferred {} entities from player {} to server",
                        entities.size(), playerId);
        }
    }

    // ES: Inyección de dependencias: registro de entidades e id del jugador local.
    // EN: Dependency injection: entity registry and local player id.
    void SetRegistry(EntityRegistry* registry) { m_registry = registry; }
    void SetLocalPlayerId(PlayerID id) { m_localPlayerId = id; }

private:
    OwnershipManager() = default;
    EntityRegistry* m_registry = nullptr;
    PlayerID m_localPlayerId = 0;
};

} // namespace kmp
