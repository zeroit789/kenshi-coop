// ES: Gestor de interés por zonas en el cliente: calcula la zona del jugador local
//     y avisa al servidor (C2S_ZoneRequest) cuando cambia. OJO: esta clase está
//     definida solo en este .cpp, sin cabecera, y no se usa en ningún otro sitio
//     (código muerto); la lógica de zonas vigente está en ZoneEngine (zone_engine.h).
// EN: Client-side zone interest manager: computes the local player's zone and
//     notifies the server (C2S_ZoneRequest) when it changes. NOTE: this class is
//     defined only in this .cpp, with no header, and is not used anywhere else
//     (dead code); the active zone logic lives in ZoneEngine (zone_engine.h).
#include "entity_registry.h"
#include "kmp/types.h"
#include "kmp/constants.h"
#include "kmp/protocol.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <functional>

namespace kmp {

// ES: Gestión de interés por zonas.
//     El mundo es una rejilla de zonas. Cada cliente solo recibe actualizaciones de
//     entidades en las zonas adyacentes a la suya (rejilla 3x3).
//     Esto reduce mucho el ancho de banda para entidades lejanas.
//
// EN:
// Zone-based interest management.
// The world is a grid of zones. Each client only receives updates for
// entities in zones adjacent to the client's current zone (3x3 grid).
// This dramatically reduces bandwidth for distant entities.

// ES: Singleton con la zona local actual y la función de envío de paquetes.
// EN: Singleton holding the current local zone and the packet send function.
class ZoneInterestManager {
public:
    static ZoneInterestManager& Get() {
        static ZoneInterestManager instance;
        return instance;
    }

    // ES: Fija la función usada para enviar paquetes (evita dependencia circular con Core).
    // Set the callback used to send packets (avoids circular dependency with Core)
    using SendFn = std::function<void(const uint8_t* data, size_t size, int channel, bool reliable)>;
    void SetSendCallback(SendFn fn) { m_sendFn = std::move(fn); }

    // ES: Recalcula la zona local según la posición de cámara/personaje y, si ha
    //     cambiado, avisa al servidor.
    // Update the local player's zone based on camera/character position
    void UpdateLocalZone(const Vec3& position) {
        ZoneCoord newZone = ZoneCoord::FromWorldPos(position, KMP_ZONE_SIZE);
        if (newZone != m_localZone) {
            spdlog::debug("ZoneInterest: Player moved to zone ({}, {})", newZone.x, newZone.y);
            m_localZone = newZone;
            RebuildInterestSet();
        }
    }

    // ES: ¿Está una zona dentro de nuestro rango de interés (adyacente)?
    // Check if an entity is within our interest range
    bool IsInRange(const ZoneCoord& entityZone) const {
        return m_localZone.IsAdjacent(entityZone);
    }

    // ES: ¿Debe sincronizarse esta entidad con nosotros (su zona es adyacente)?
    // Check if a specific entity should be synced to us
    bool ShouldSync(EntityID entityId, const EntityRegistry& registry) const {
        auto infoCopy = registry.GetInfo(entityId);
        if (!infoCopy) return false;
        return m_localZone.IsAdjacent(infoCopy->zone);
    }

    // ES: Devuelve las zonas de interés (3x3 alrededor del jugador si el radio es 1).
    // Get the set of zones we're interested in (3x3 around player)
    std::vector<ZoneCoord> GetInterestZones() const {
        std::vector<ZoneCoord> zones;
        for (int dx = -KMP_INTEREST_RADIUS; dx <= KMP_INTEREST_RADIUS; dx++) {
            for (int dy = -KMP_INTEREST_RADIUS; dy <= KMP_INTEREST_RADIUS; dy++) {
                zones.emplace_back(m_localZone.x + dx, m_localZone.y + dy);
            }
        }
        return zones;
    }

    // ES: Zona local actual.
    // EN: Current local zone.
    ZoneCoord GetLocalZone() const { return m_localZone; }

private:
    ZoneInterestManager() = default;

    // ES: Envía al servidor un C2S_ZoneRequest con la nueva zona (canal fiable ordenado).
    // EN: Sends the server a C2S_ZoneRequest with the new zone (reliable ordered channel).
    void RebuildInterestSet() {
        // ES: Avisar al servidor de que hemos cambiado de zona para que nos mande
        //     las entidades de nuestra nueva área de interés.
        // Notify the server that we've changed zones so it can send us
        // entities in our new interest area.
        if (!m_sendFn) return;

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_ZoneRequest);
        writer.WriteI32(m_localZone.x);
        writer.WriteI32(m_localZone.y);
        m_sendFn(writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, true);

        spdlog::info("ZoneInterest: Sent zone request ({}, {}) to server", m_localZone.x, m_localZone.y);
    }

    ZoneCoord m_localZone;
    SendFn m_sendFn;
};

} // namespace kmp
