// ES: Implementación del KenshiSDK: sondeo de personajes, instantáneas, cálculo de
//     diferencias y escrituras directas en memoria del juego.
// EN: KenshiSDK implementation: character polling, snapshots, diff computation and
//     direct writes into game memory.
#include "kenshi_sdk.h"
#include "../game/game_types.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cmath>

namespace kmp::sdk {

// ES: DIFERENCIA DE UNA ENTIDAD.
// ═══════════════════════════════════════════════════════════════════════════
//  ENTITY SNAPSHOT DIFF
// ═══════════════════════════════════════════════════════════════════════════

// ES: Marca como sucios posición (>0.1 u), rotación (>0.01), animación, salud (>0.5 en
//     alguna parte) o estado vivo/muerto (también cuenta como salud).
// EN: Marks position (>0.1 u), rotation (>0.01), animation, health (>0.5 on any part) or
//     alive/dead state (also counted as health) as dirty.
uint16_t EntitySnapshot::DiffAgainst(const EntitySnapshot& prev) const {
    uint16_t flags = Dirty_None;

    // ES: Posición: umbral de 0.1 unidades (comparando distancias al cuadrado).
    // Position: threshold 0.1 units (same as KMP_POS_CHANGE_THRESHOLD)
    float dx = position.x - prev.position.x;
    float dy = position.y - prev.position.y;
    float dz = position.z - prev.position.z;
    if (dx * dx + dy * dy + dz * dz > 0.01f)
        flags |= Dirty_Position;

    // ES: Rotación: umbral 0.01.
    // Rotation: threshold 0.01 (same as KMP_ROT_CHANGE_THRESHOLD)
    float dw = rotation.w - prev.rotation.w;
    float drx = rotation.x - prev.rotation.x;
    float dry = rotation.y - prev.rotation.y;
    float drz = rotation.z - prev.rotation.z;
    if (dw * dw + drx * drx + dry * dry + drz * drz > 0.0001f)
        flags |= Dirty_Rotation;

    // ES: Estado de animación.
    // Animation state
    if (animState != prev.animState)
        flags |= Dirty_Animation;

    // ES: Salud: alguna parte cambió más de 0.5.
    // Health: any body part changed by > 0.5
    for (int i = 0; i < 7; i++) {
        if (std::abs(health[i] - prev.health[i]) > 0.5f) {
            flags |= Dirty_Health;
            break;
        }
    }

    // ES: Vivo/muerto.
    // Alive state
    if (alive != prev.alive)
        flags |= Dirty_Health;

    return flags;
}

// ES: INSTANTÁNEA DEL MUNDO.
// ═══════════════════════════════════════════════════════════════════════════
//  WORLD SNAPSHOT
// ═══════════════════════════════════════════════════════════════════════════

// ES: Búsqueda lineal por puntero.
// EN: Linear search by pointer.
const EntitySnapshot* WorldSnapshot::FindByPtr(uintptr_t ptr) const {
    for (const auto& e : entities) {
        if (e.gamePtr == ptr) return &e;
    }
    return nullptr;
}

// ES: IMPLEMENTACIÓN DEL SDK.
// ═══════════════════════════════════════════════════════════════════════════
//  SDK IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════════════

// ES: Solo comprueba que se ha resuelto PlayerBase o GameWorld (punteros globales del
//     juego encontrados por el escáner); si no, no se puede enumerar entidades.
// EN: Only checks that PlayerBase or GameWorld (global game pointers found by the
//     scanner) are resolved; otherwise entities cannot be enumerated.
bool KenshiSDK::Initialize() {
    // ES: Verificar que existen los globales necesarios para enumerar entidades.
    // Verify we have the globals needed for entity enumeration
    uintptr_t playerBase = game::GetResolvedPlayerBase();
    uintptr_t gameWorld = game::GetResolvedGameWorld();

    if (playerBase == 0 && gameWorld == 0) {
        spdlog::warn("KenshiSDK: Cannot initialize — no PlayerBase or GameWorld resolved");
        return false;
    }

    m_initialized = true;
    m_frameNumber = 0;
    spdlog::info("KenshiSDK: Initialized (PlayerBase=0x{:X}, GameWorld=0x{:X})",
                 playerBase, gameWorld);
    return true;
}

// ES: Un tick: mueve la instantánea actual a "anterior", sondea una nueva, calcula la
//     diferencia y mide el tiempo empleado. El sondeo va fuera del mutex.
// EN: One tick: moves the current snapshot to "previous", polls a new one, computes the
//     diff and measures the time taken. Polling happens outside the mutex.
void KenshiSDK::Update() {
    if (!m_initialized) return;

    auto start = std::chrono::steady_clock::now();

    // ES: Pasar la actual a anterior.
    // Swap previous/current
    {
        std::lock_guard lock(m_mutex);
        m_previous = std::move(m_current);
    }

    // ES: Sondear el estado nuevo.
    // Poll fresh state
    WorldSnapshot newSnap;
    newSnap.frameNumber = ++m_frameNumber;
    PollEntities(newSnap);

    // ES: Calcular la diferencia.
    // Compute diff
    WorldDiff diff = ComputeDiff(m_previous, newSnap);

    // ES: Guardar resultados.
    // Store results
    {
        std::lock_guard lock(m_mutex);
        m_current = std::move(newSnap);
        m_lastDiff = std::move(diff);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    m_lastPollTimeMs = std::chrono::duration<float, std::milli>(elapsed).count();
}

// ES: Getters protegidos por mutex (devuelven copias).
// EN: Mutex-protected getters (return copies).
WorldSnapshot KenshiSDK::GetCurrentSnapshot() const {
    std::lock_guard lock(m_mutex);
    return m_current;
}

WorldDiff KenshiSDK::GetLastDiff() const {
    std::lock_guard lock(m_mutex);
    return m_lastDiff;
}

bool KenshiSDK::GetEntityState(uintptr_t gamePtr, EntitySnapshot& out) const {
    std::lock_guard lock(m_mutex);
    const EntitySnapshot* found = m_current.FindByPtr(gamePtr);
    if (!found) return false;
    out = *found;
    return true;
}

std::vector<uintptr_t> KenshiSDK::GetAllEntityPtrs() const {
    std::lock_guard lock(m_mutex);
    std::vector<uintptr_t> ptrs;
    ptrs.reserve(m_current.entities.size());
    for (const auto& e : m_current.entities) {
        ptrs.push_back(e.gamePtr);
    }
    return ptrs;
}

size_t KenshiSDK::GetEntityCount() const {
    std::lock_guard lock(m_mutex);
    return m_current.entities.size();
}

// ES: Escritura de estado.
// ── State Write ──

// ES: Escribe la posición mediante CharacterAccessor::WritePosition.
// EN: Writes the position through CharacterAccessor::WritePosition.
bool KenshiSDK::WritePosition(uintptr_t gamePtr, const Vec3& pos) {
    if (gamePtr == 0) return false;
    game::CharacterAccessor accessor(reinterpret_cast<void*>(gamePtr));
    return accessor.WritePosition(pos);
}

// ES: Escribe la salud (carne/flesh) de una parte del cuerpo siguiendo la cadena MedicalSystem.
// EN: Writes a body part's health (flesh) following the MedicalSystem chain.
bool KenshiSDK::WriteHealth(uintptr_t gamePtr, BodyPart part, float value) {
    if (gamePtr == 0) return false;
    auto& offsets = game::GetOffsets().character;
    // Cadena canónica MedicalSystem (ver game_types.h):
    // partArray = [char+0x5F8] → part_i = [partArray + part*8] → flesh @ part_i+0x40
    // EN: Canonical MedicalSystem chain (see game_types.h):
    //     partArray = [char+0x5F8] -> part_i = [partArray + part*8] -> flesh @ part_i+0x40
    uintptr_t partArray = 0;
    if (!Memory::Read(gamePtr + offsets.healthPartArray, partArray) || partArray == 0)
        return false;
    uintptr_t partPtr = 0;
    if (!Memory::Read(partArray + static_cast<int>(part) * offsets.healthStride, partPtr) || partPtr == 0)
        return false;
    return Memory::Write(partPtr + offsets.healthBase, value);
}

// ES: Escribe el nombre mediante CharacterAccessor::WriteName.
// EN: Writes the name through CharacterAccessor::WriteName.
bool KenshiSDK::WriteName(uintptr_t gamePtr, const std::string& name) {
    if (gamePtr == 0) return false;
    game::CharacterAccessor accessor(reinterpret_cast<void*>(gamePtr));
    return accessor.WriteName(name);
}

// ES: Sondeo.
// ── Polling ──

// ES: Lectura protegida por SEH del puntero index-ésimo de una lista de punteros (sin
//     objetos C++ con destructor dentro del __try). No se usa en ningún sitio del
//     proyecto (código muerto; PollEntities usa CharacterIterator).
// EN: (Not used anywhere in the project: dead code; PollEntities uses CharacterIterator.)
// SEH filter — no C++ objects with destructors allowed in __try functions.
// We only wrap the raw pointer read that could fault, not the C++ containers.
static int SEH_ReadCharPtr(uintptr_t listBase, int index, uintptr_t* outPtr) {
    __try {
        uintptr_t elemAddr = listBase + static_cast<uintptr_t>(index) * 8;
        *outPtr = *reinterpret_cast<uintptr_t*>(elemAddr);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outPtr = 0;
        return 1;
    }
}

// ES: Recorre todos los personajes con CharacterIterator, lee cada uno y añade la hora
//     del día y la velocidad del juego desde GameWorld.
// EN: Walks every character with CharacterIterator, reads each one and adds time of day
//     and game speed from GameWorld.
void KenshiSDK::PollEntities(WorldSnapshot& snapshot) {
    // ES: Usar CharacterIterator (no hace falta SEH a este nivel).
    // Use CharacterIterator (C++ friendly, no SEH needed at this level)
    game::CharacterIterator iter;
    int count = iter.Count();
    snapshot.entities.reserve(static_cast<size_t>(count));

    while (iter.HasNext()) {
        game::CharacterAccessor accessor = iter.Next();
        if (!accessor.IsValid()) continue;

        EntitySnapshot entity = ReadEntity(accessor.GetPtr());
        if (entity.gamePtr != 0) {
            snapshot.entities.push_back(std::move(entity));
        }
    }

    // ES: Leer el estado del mundo.
    // Read world state
    uintptr_t gameWorld = game::GetResolvedGameWorld();
    if (gameWorld != 0) {
        game::GameWorldAccessor world(gameWorld);
        snapshot.timeOfDay = world.GetTimeOfDay();
        snapshot.gameSpeed = world.GetGameSpeed();
    }
}

// ES: Lee todos los campos de una entidad. Nota: playerControlled no se rellena aquí
//     (queda en false).
// EN: Reads every field of an entity. Note: playerControlled is not filled in here
//     (stays false).
EntitySnapshot KenshiSDK::ReadEntity(uintptr_t charPtr) const {
    EntitySnapshot snap;
    snap.gamePtr = charPtr;

    game::CharacterAccessor accessor(reinterpret_cast<void*>(charPtr));
    if (!accessor.IsValid()) return snap;

    snap.position = accessor.GetPosition();
    snap.rotation = accessor.GetRotation();
    snap.factionPtr = accessor.GetFactionPtr();
    snap.name = accessor.GetName();
    snap.alive = accessor.IsAlive();
    snap.animState = accessor.GetAnimState();
    snap.moveSpeed = accessor.GetMoveSpeed();

    // ES: ID de facción leído a través del puntero de facción (offset faction.id).
    // Read faction ID from faction pointer
    {
        const int fIdOff = game::GetOffsets().faction.id;
        if (snap.factionPtr != 0 && fIdOff >= 0) {
            Memory::Read(snap.factionPtr + fIdOff, snap.factionId);
        }
    }

    // ES: Salud de las 7 partes del cuerpo.
    // Read all 7 body part health values
    for (int i = 0; i < 7; i++) {
        snap.health[i] = accessor.GetHealth(static_cast<BodyPart>(i));
    }

    return snap;
}

// ES: Diferencia.
// ── Diff ──

// ES: Compara dos instantáneas: las entidades nuevas van a added y a changed (con
//     Dirty_All), las existentes a changed solo si algo cambió, y las que ya no están a
//     removed.
// EN: Compares two snapshots: new entities go to added and changed (with Dirty_All),
//     existing ones to changed only if something changed, and missing ones to removed.
WorldDiff KenshiSDK::ComputeDiff(const WorldSnapshot& oldSnap,
                                  const WorldSnapshot& newSnap) const {
    WorldDiff diff;

    // ES: Índice de la instantánea vieja.
    // Build lookup from old snapshot
    std::unordered_map<uintptr_t, const EntitySnapshot*> oldMap;
    oldMap.reserve(oldSnap.entities.size());
    for (const auto& e : oldSnap.entities) {
        oldMap[e.gamePtr] = &e;
    }

    // ES: Comparar las nuevas con las viejas.
    // Check new entities against old
    std::unordered_map<uintptr_t, bool> seen;
    seen.reserve(newSnap.entities.size());

    for (const auto& e : newSnap.entities) {
        seen[e.gamePtr] = true;

        auto it = oldMap.find(e.gamePtr);
        if (it == oldMap.end()) {
            // ES: Entidad nueva.
            // New entity
            diff.added.push_back(e.gamePtr);
            EntityDelta delta;
            delta.gamePtr = e.gamePtr;
            delta.dirtyFlags = Dirty_All;
            delta.snapshot = e;
            diff.changed.push_back(std::move(delta));
        } else {
            // ES: Entidad existente: ¿ha cambiado?
            // Existing entity — check for changes
            uint16_t flags = e.DiffAgainst(*it->second);
            if (flags != Dirty_None) {
                EntityDelta delta;
                delta.gamePtr = e.gamePtr;
                delta.dirtyFlags = flags;
                delta.snapshot = e;
                diff.changed.push_back(std::move(delta));
            }
        }
    }

    // ES: Entidades eliminadas.
    // Check for removed entities
    for (const auto& e : oldSnap.entities) {
        if (seen.find(e.gamePtr) == seen.end()) {
            diff.removed.push_back(e.gamePtr);
        }
    }

    return diff;
}

} // namespace kmp::sdk
