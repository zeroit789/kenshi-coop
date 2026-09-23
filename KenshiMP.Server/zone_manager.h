// ES: zone_manager.h - ZoneManager: gestión de interés por zonas. El mundo se divide en una
//     rejilla de zonas (ZoneCoord); un jugador solo "necesita" las entidades de su zona y las 8
//     adyacentes. Se compila en el servidor pero GameServer no lo usa: hace el filtrado en
//     línea en server.cpp (y el culling de posiciones está desactivado a propósito).
// EN: zone_manager.h - ZoneManager: zone-based interest management. The world is split into a
//     grid of zones (ZoneCoord); a player only "needs" entities in its zone and the 8 adjacent
//     ones. Compiled into the server but GameServer does not use it: it filters inline in
//     server.cpp (and position culling is intentionally disabled).
#pragma once
#include "kmp/types.h"
#include <vector>
#include <unordered_map>

namespace kmp {

// ES: Declaraciones adelantadas; las definiciones están en server.h.
// EN: Forward declarations; the definitions live in server.h.
struct ServerEntity;   // Forward from server.h
struct ConnectedPlayer; // Forward from server.h

// ES: Gestión de interés por zonas para el servidor dedicado. Lleva un índice de población
//     por zona y consultas espaciales para enviar posiciones solo a los jugadores relevantes.
// EN: Zone-based interest management for the dedicated server.
// Tracks zone populations and provides efficient spatial queries
// for broadcasting position updates only to relevant players.
class ZoneManager {
public:
    // ES: ── Seguimiento de población por zona ──
    // EN: ── Zone population tracking ──

    // ES: Reconstruye desde cero el índice zona -> nº de entidades.
    // EN: Rebuild zone population index from entity map.
    void RebuildIndex(const std::unordered_map<EntityID, ServerEntity>& entities);

    // ES: Número de entidades en una zona concreta.
    // EN: Get the number of entities in a specific zone.
    size_t GetZonePopulation(ZoneCoord zone) const;

    // ES: Todas las zonas con al menos una entidad.
    // EN: Get all populated zones.
    std::vector<ZoneCoord> GetPopulatedZones() const;

    // ES: ── Gestión de interés ──
    // EN: ── Interest management ──

    // ES: Entidades relevantes para un jugador (zonas adyacentes), excluyendo las suyas propias.
    // EN: Get entity IDs that are relevant to a player (in adjacent zones).
    // Excludes the player's own entities.
    static std::vector<EntityID> GetRelevantEntities(
        const std::unordered_map<EntityID, ServerEntity>& entities,
        const ConnectedPlayer& player);

    // ES: Indica si el jugador debe recibir actualizaciones de una entidad (no suya y en zona adyacente).
    // EN: Check if a player should receive updates about an entity.
    static bool ShouldReceiveUpdates(
        const ConnectedPlayer& player,
        const ServerEntity& entity);

    // ES: ── Cambios de zona ──
    // EN: ── Zone transitions ──

    // ES: true si la zona nueva es distinta de la anterior.
    // EN: Check if a player has changed zones. Returns true if zone changed.
    static bool HasChangedZone(ZoneCoord oldZone, ZoneCoord newZone);

    // ES: Zona que corresponde a una posición del mundo.
    // EN: Get the zone for a world position.
    static ZoneCoord GetZoneForPosition(Vec3 position);

    // ES: Las 9 zonas (3x3) centradas en la zona dada.
    // EN: Get all zones within render distance of a position.
    static std::vector<ZoneCoord> GetAdjacentZones(ZoneCoord center);

private:
    // ES: Índice zona -> nº de entidades (se reconstruye periódicamente).
    // EN: Zone -> entity count index (rebuilt periodically)
    std::unordered_map<uint64_t, size_t> m_zonePopulation;

    // ES: Empaqueta (x, y) de la zona en una clave de 64 bits: x en los 32 bits altos, y en los bajos.
    // EN: Packs the zone (x, y) into a 64-bit key: x in the high 32 bits, y in the low 32 bits.
    static uint64_t ZoneKey(ZoneCoord zone) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(zone.x)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(zone.y));
    }
};

} // namespace kmp
