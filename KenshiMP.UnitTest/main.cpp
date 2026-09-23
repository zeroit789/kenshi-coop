// ═══════════════════════════════════════════════════════════════════════════
//  KenshiMP Unit Test — Validates game-facing logic with fake game memory
// ═══════════════════════════════════════════════════════════════════════════
// ES: Test unitario de KenshiMP: valida la lógica que toca el juego usando memoria falsa.
//     Comprueba que el MISMO código que corre dentro de Kenshi funciona:
//       - CharacterAccessor lee/escribe posiciones, nombres y facciones correctamente
//       - EntityRegistry sigue las entidades por ID, dueño y puntero al objeto del juego
//       - El sistema de interpolación suaviza el movimiento de jugadores remotos
//       - La serialización de paquetes conserva los datos en ida y vuelta
//     Se reservan "objetos de juego" falsos en la memoria de este proceso. Memory::Read/Write
//     desreferencian punteros crudos (con SEH), así que funcionan igual sobre estos búferes
//     que sobre objetos reales de Kenshi. Si el test pasa, el código dentro del juego
//     funcionará (suponiendo que los offsets sean correctos). Devuelve 1 si falla algún test.
// EN: This test proves the EXACT code that runs inside Kenshi actually works:
//   - CharacterAccessor reads/writes positions, names, factions correctly
//   - EntityRegistry tracks entities by ID, owner, game object pointer
//   - Interpolation system smooths remote player movement
//   - Packet serialization round-trips data correctly
//
// We allocate fake "game objects" in our own process memory. The Memory::Read/Write
// functions use raw pointer dereference (with SEH), so they work identically
// on our fake buffers as they do on real Kenshi game objects.
// If this test passes, the in-game code WILL work (assuming correct offsets).

#include "game/game_types.h"
#include "sync/entity_registry.h"
#include "sync/interpolation.h"
#include "kmp/types.h"
#include "kmp/constants.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/memory.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>

// ES: ── Mini framework de test: contadores de aprobados/fallidos y aserción que imprime PASS/FAIL ──
// EN: ── Test framework ──
static int g_passed = 0;
static int g_failed = 0;

// ES: Registra el resultado de una comprobación y lo imprime (no aborta al fallar).
// EN: Records a check result and prints it (does not abort on failure).
static void TestAssert(bool condition, const char* name) {
    if (condition) {
        printf("  [PASS] %s\n", name);
        g_passed++;
    } else {
        printf("  [FAIL] %s\n", name);
        g_failed++;
    }
}

// ES: Compara dos float con tolerancia eps.
// EN: Compares two floats within eps tolerance.
static bool FloatEq(float a, float b, float eps = 0.001f) {
    return std::abs(a - b) < eps;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Fake Kenshi character — laid out in memory exactly like the real thing
// ═══════════════════════════════════════════════════════════════════════════
// ES: Personaje falso de Kenshi con el mismo layout en memoria que el real.
//     Se reserva un búfer y se escriben valores en los offsets que espera CharacterAccessor
//     (offsets de GetOffsets().character: posición, nombre, facción...). Simula lo que
//     produce la función CharacterCreate de Kenshi.
// EN: We allocate a buffer and write values at the offsets that CharacterAccessor
//     expects. This simulates what Kenshi's CharacterCreate function produces.

struct FakeCharacter {
    uint8_t data[0x1000];  // 4KB — more than enough for a character struct

    FakeCharacter() { memset(data, 0, sizeof(data)); }
    // ES: Dirección y puntero del búfer (lo que el código del mod trataría como el objeto del juego).
    // EN: Buffer address and pointer (what mod code treats as the game object).
    uintptr_t Addr() const { return reinterpret_cast<uintptr_t>(data); }
    void* Ptr() const { return const_cast<uint8_t*>(data); }

    // ES: Escribe un valor de cualquier tipo en un offset
    // EN: Write a value at an offset (templated)
    template<typename T>
    void Set(int offset, const T& val) {
        memcpy(data + offset, &val, sizeof(T));
    }

    // ES: Escribe una cadena SSO de Kenshi (layout de std::string de MSVC) en el offset.
    //     Layout: [búfer interno de 16 bytes][uint64 tamaño][uint64 capacidad].
    //     Solo soporta cadenas de hasta 15 caracteres (modo SSO); más largas quedan vacías.
    // EN: Write a Kenshi SSO string (MSVC std::string layout) at offset
    //     Layout: [16 bytes inline buffer][uint64 size][uint64 capacity]
    //     Only supports strings up to 15 chars (SSO mode); longer ones are left empty.
    void SetSSOString(int offset, const char* str) {
        size_t len = strlen(str);
        // ES: Poner a cero los 32 bytes del std::string
        // EN: Zero the whole 32-byte std::string area
        memset(data + offset, 0, 32);
        if (len <= 15) {
            // ES: SSO: copiar al búfer interno
            // EN: SSO: copy into inline buffer
            memcpy(data + offset, str, len);
            // ES: Tamaño en +0x10
            // EN: Size at +0x10
            uint64_t size = len;
            memcpy(data + offset + 0x10, &size, sizeof(size));
            // ES: Capacidad en +0x18 = 15 (modo SSO)
            // EN: Capacity at +0x18 = 15 (SSO mode)
            uint64_t cap = 15;
            memcpy(data + offset + 0x18, &cap, sizeof(cap));
        }
    }

    // ES: Escribe un Vec3 (3 float) en el offset
    // EN: Write a Vec3 at offset
    void SetVec3(int offset, float x, float y, float z) {
        Set<float>(offset, x);
        Set<float>(offset + 4, y);
        Set<float>(offset + 8, z);
    }

    // ES: Escribe un puntero a facción en el offset
    // EN: Write a faction pointer at offset
    void SetFactionPtr(int offset, uintptr_t factionAddr) {
        Set<uintptr_t>(offset, factionAddr);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 1: CharacterAccessor reads position from fake game memory
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 1: comprueba que CharacterAccessor::GetPosition lee la posición escrita en el
//     offset de posición del personaje falso y que el accessor se considera válido.
// EN: TEST 1: checks CharacterAccessor::GetPosition reads the position written at the fake
//     character's position offset and that the accessor is valid.
static void Test_ReadPosition() {
    printf("\n=== Test: CharacterAccessor::GetPosition ===\n");

    auto& offsets = kmp::game::GetOffsets().character;
    FakeCharacter fake;
    fake.SetVec3(offsets.position, -51200.0f, 1600.0f, 2700.0f);

    kmp::game::CharacterAccessor accessor(fake.Ptr());
    TestAssert(accessor.IsValid(), "Accessor is valid");

    kmp::Vec3 pos = accessor.GetPosition();
    TestAssert(FloatEq(pos.x, -51200.0f), "Position.x matches (-51200)");
    TestAssert(FloatEq(pos.y, 1600.0f),   "Position.y matches (1600)");
    TestAssert(FloatEq(pos.z, 2700.0f),   "Position.z matches (2700)");
    printf("    Read position: (%.1f, %.1f, %.1f)\n", pos.x, pos.y, pos.z);
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 2: CharacterAccessor reads name (SSO mode) from fake game memory
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 2: comprueba que GetName lee nombres en modo SSO (uno corto y uno de 14 caracteres).
// EN: TEST 2: checks GetName reads SSO-mode names (a short one and a 14-char one).
static void Test_ReadName() {
    printf("\n=== Test: CharacterAccessor::GetName (SSO) ===\n");

    auto& offsets = kmp::game::GetOffsets().character;
    FakeCharacter fake;
    fake.SetSSOString(offsets.name, "Beep");

    kmp::game::CharacterAccessor accessor(fake.Ptr());
    std::string name = accessor.GetName();
    TestAssert(name == "Beep", "Name reads as 'Beep'");
    printf("    Read name: '%s'\n", name.c_str());

    // ES: Probar un nombre SSO más largo (máximo 15 caracteres)
    // EN: Test a longer SSO name (15 chars max)
    FakeCharacter fake2;
    fake2.SetSSOString(offsets.name, "Cat-Lon_Master");

    kmp::game::CharacterAccessor accessor2(fake2.Ptr());
    std::string name2 = accessor2.GetName();
    TestAssert(name2 == "Cat-Lon_Master", "14-char name reads correctly");
    printf("    Read name: '%s'\n", name2.c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 3: CharacterAccessor writes name to fake game memory
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 3: comprueba que WriteName sobrescribe el nombre y que se relee igual.
// EN: TEST 3: checks WriteName overwrites the name and it reads back the same.
static void Test_WriteName() {
    printf("\n=== Test: CharacterAccessor::WriteName ===\n");

    auto& offsets = kmp::game::GetOffsets().character;
    FakeCharacter fake;
    fake.SetSSOString(offsets.name, "OldName");

    kmp::game::CharacterAccessor accessor(fake.Ptr());

    // ES: Escribir un nombre nuevo
    // EN: Write a new name
    bool ok = accessor.WriteName("RemotePlayer");
    TestAssert(ok, "WriteName returned true");

    // ES: Releerlo
    // EN: Read it back
    std::string result = accessor.GetName();
    TestAssert(result == "RemotePlayer", "Name written and read back correctly");
    printf("    Wrote 'RemotePlayer', read back: '%s'\n", result.c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 4: CharacterAccessor writes position (fallback mode)
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 4: comprueba WritePosition en modo alternativo (sin objeto físico real escribe en
//     la posición cacheada del personaje) y que GetPosition devuelve lo escrito.
// EN: TEST 4: checks WritePosition in fallback mode (with no real physics object it writes
//     to the character's cached position) and that GetPosition returns what was written.
static void Test_WritePosition() {
    printf("\n=== Test: CharacterAccessor::WritePosition (fallback) ===\n");

    auto& offsets = kmp::game::GetOffsets().character;
    FakeCharacter fake;
    fake.SetVec3(offsets.position, 0.0f, 0.0f, 0.0f);

    kmp::game::CharacterAccessor accessor(fake.Ptr());

    // ES: Escribir una posición nueva (usa el modo alternativo: escribe en la posición cacheada)
    // EN: Write a new position (will use fallback: writes to cached position)
    kmp::Vec3 newPos{-5000.0f, 200.0f, 8000.0f};
    bool ok = accessor.WritePosition(newPos);
    TestAssert(ok, "WritePosition returned true");

    // ES: Releerla
    // EN: Read it back
    kmp::Vec3 readPos = accessor.GetPosition();
    TestAssert(FloatEq(readPos.x, -5000.0f), "Written position.x matches");
    TestAssert(FloatEq(readPos.y, 200.0f),   "Written position.y matches");
    TestAssert(FloatEq(readPos.z, 8000.0f),  "Written position.z matches");
    printf("    Wrote (-5000, 200, 8000), read back: (%.1f, %.1f, %.1f)\n",
           readPos.x, readPos.y, readPos.z);
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 5: CharacterAccessor reads/writes faction pointer
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 5: comprueba que GetFactionPtr lee el puntero de facción y que WriteFaction lo cambia.
// EN: TEST 5: checks GetFactionPtr reads the faction pointer and WriteFaction changes it.
static void Test_FactionReadWrite() {
    printf("\n=== Test: CharacterAccessor Faction Read/Write ===\n");

    auto& offsets = kmp::game::GetOffsets().character;
    FakeCharacter fake;

    // ES: Simular un puntero a facción (una dirección cualquiera del heap; no se desreferencia)
    // EN: Simulate a faction pointer (some heap address)
    uintptr_t fakeFaction = 0x0000020000001234;
    fake.SetFactionPtr(offsets.faction, fakeFaction);

    kmp::game::CharacterAccessor accessor(fake.Ptr());

    uintptr_t readFaction = accessor.GetFactionPtr();
    TestAssert(readFaction == fakeFaction, "Faction pointer reads correctly");
    printf("    Read faction ptr: 0x%llX\n", (unsigned long long)readFaction);

    // ES: Escribir otra facción
    // EN: Write a different faction
    uintptr_t newFaction = 0x0000020000005678;
    bool ok = accessor.WriteFaction(newFaction);
    TestAssert(ok, "WriteFaction returned true");

    uintptr_t readBack = accessor.GetFactionPtr();
    TestAssert(readBack == newFaction, "New faction ptr reads back correctly");
    printf("    Wrote 0x%llX, read back: 0x%llX\n",
           (unsigned long long)newFaction, (unsigned long long)readBack);
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 6: EntityRegistry — register, lookup, unregister
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 6: CRUD del EntityRegistry: registrar dos entidades locales (IDs distintos),
//     buscarlas por puntero y por ID, leer su info, listar las del jugador y dar de baja una.
// EN: TEST 6: EntityRegistry CRUD: register two local entities (distinct IDs), look them up
//     by pointer and by ID, read their info, list the player's ones and unregister one.
static void Test_EntityRegistry() {
    printf("\n=== Test: EntityRegistry CRUD ===\n");

    kmp::EntityRegistry registry;
    FakeCharacter fake1, fake2;

    // ES: Registrar dos entidades locales
    // EN: Register two local entities
    kmp::EntityID id1 = registry.Register(fake1.Ptr(), kmp::EntityType::PlayerCharacter, 1);
    kmp::EntityID id2 = registry.Register(fake2.Ptr(), kmp::EntityType::PlayerCharacter, 1);
    TestAssert(id1 != id2, "Two entities get different IDs");
    TestAssert(registry.GetEntityCount() == 2, "Registry has 2 entities");

    // ES: Búsqueda por puntero al objeto del juego
    // EN: Lookup by game object pointer
    kmp::EntityID lookup1 = registry.GetNetId(fake1.Ptr());
    TestAssert(lookup1 == id1, "Lookup by pointer returns correct ID");

    // ES: Búsqueda por ID de red
    // EN: Lookup by network ID
    void* obj = registry.GetGameObject(id2);
    TestAssert(obj == fake2.Ptr(), "Lookup by netId returns correct game object");

    // ES: Información de la entidad (dueño, local/remota)
    // EN: Get entity info
    auto info = registry.GetInfo(id1);
    TestAssert(info.has_value(), "GetInfo returns non-empty optional");
    TestAssert(info->ownerPlayerId == 1, "Owner matches");
    TestAssert(!info->isRemote, "Local entity is not remote");

    // ES: Entidades del jugador 1
    // EN: Get player entities
    auto playerEnts = registry.GetPlayerEntities(1);
    TestAssert(playerEnts.size() == 2, "Player 1 has 2 entities");

    // ES: Dar de baja una
    // EN: Unregister one
    registry.Unregister(id1);
    TestAssert(registry.GetEntityCount() == 1, "After unregister: 1 entity");
    TestAssert(registry.GetGameObject(id1) == nullptr, "Unregistered entity returns null");

    printf("    Registry CRUD: all operations verified\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 7: EntityRegistry — remote entity lifecycle (what happens in MP)
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 7: ciclo de vida de una entidad remota en multijugador: alta con el ID del
//     servidor, enlace del objeto del juego, actualización de posición y limpieza total.
// EN: TEST 7: remote entity lifecycle in multiplayer: registration with the server ID,
//     game object linking, position update and full cleanup.
static void Test_RemoteEntityLifecycle() {
    printf("\n=== Test: Remote Entity Lifecycle ===\n");

    kmp::EntityRegistry registry;
    FakeCharacter fakeRemote;

    // ES: Paso 1: el servidor nos avisa de una entidad remota (mensaje EntitySpawn)
    // EN: Step 1: Server tells us about a remote entity (EntitySpawn message)
    kmp::Vec3 spawnPos{-51200.0f, 1600.0f, 2700.0f};
    kmp::EntityID remoteId = registry.RegisterRemote(100, kmp::EntityType::PlayerCharacter, 2, spawnPos);
    TestAssert(remoteId == 100, "Remote entity registered with server-assigned ID");
    TestAssert(registry.GetRemoteCount() == 1, "1 remote entity in registry");
    TestAssert(registry.GetSpawnedRemoteCount() == 0, "0 spawned remotes (no game object yet)");

    // ES: Paso 2: entity_hooks repite CharacterCreate y obtiene un objeto del juego.
    //     Aquí se simula enlazando un objeto falso.
    // EN: Step 2: entity_hooks replays CharacterCreate, gets a game object
    //     We simulate this by linking a fake game object
    registry.SetGameObject(100, fakeRemote.Ptr());
    TestAssert(registry.GetSpawnedRemoteCount() == 1, "1 spawned remote (game object linked)");
    TestAssert(registry.GetGameObject(100) == fakeRemote.Ptr(), "Game object lookup works");

    // ES: Paso 3: actualizaciones de posición desde el servidor
    // EN: Step 3: Position updates from server
    kmp::Vec3 newPos{-51180.0f, 1605.0f, 2720.0f};
    registry.UpdatePosition(100, newPos);
    auto info = registry.GetInfo(100);
    TestAssert(info.has_value(), "Info still valid after position update");
    TestAssert(FloatEq(info->lastPosition.x, -51180.0f), "Updated position.x stored");

    // ES: Paso 4: el jugador se desconecta y el servidor envía PlayerLeft
    // EN: Step 4: Player disconnects, server sends PlayerLeft
    size_t cleared = registry.ClearRemoteEntities();
    TestAssert(cleared == 1, "ClearRemoteEntities removed 1 entity");
    TestAssert(registry.GetRemoteCount() == 0, "No remote entities after clear");
    TestAssert(registry.GetGameObject(100) == nullptr, "Game object removed after clear");

    printf("    Full remote entity lifecycle: register -> link -> update -> cleanup\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 8: EntityRegistry — ID remapping (server assigns new ID)
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 8: RemapEntityId cambia el ID local por el asignado por el servidor: el ID viejo
//     desaparece y el nuevo apunta al mismo objeto (también en la búsqueda por puntero).
// EN: TEST 8: RemapEntityId swaps the local ID for the server-assigned one: the old ID is
//     gone and the new one points to the same object (also in pointer lookup).
static void Test_EntityRemap() {
    printf("\n=== Test: Entity ID Remapping ===\n");

    kmp::EntityRegistry registry;
    FakeCharacter fake;

    // ES: Registrar con ID local
    // EN: Register with local ID
    kmp::EntityID localId = registry.Register(fake.Ptr(), kmp::EntityType::PlayerCharacter, 1);

    // ES: El servidor asigna un ID nuevo
    // EN: Server assigns new ID
    kmp::EntityID serverId = 42;
    bool ok = registry.RemapEntityId(localId, serverId);
    TestAssert(ok, "RemapEntityId succeeded");

    // ES: El ID viejo ya no debe existir
    // EN: Old ID should be gone
    TestAssert(registry.GetGameObject(localId) == nullptr, "Old ID no longer exists");

    // ES: El ID nuevo debe funcionar
    // EN: New ID should work
    TestAssert(registry.GetGameObject(serverId) == fake.Ptr(), "New ID maps to same game object");

    // ES: La búsqueda por puntero debe devolver el ID nuevo
    // EN: Pointer lookup should return new ID
    TestAssert(registry.GetNetId(fake.Ptr()) == serverId, "Pointer lookup returns new server ID");

    printf("    Remap: local ID %u -> server ID %u\n", localId, serverId);
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 9: Interpolation — smooth remote player movement
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 9: el sistema de interpolación devuelve la posición intermedia correcta entre
//     snapshots (teniendo en cuenta el retardo adaptativo) y deja de responder tras RemoveEntity.
// EN: TEST 9: the interpolation system returns the right midpoint between snapshots
//     (accounting for the adaptive delay) and stops answering after RemoveEntity.
static void Test_Interpolation() {
    printf("\n=== Test: Interpolation System ===\n");

    kmp::Interpolation interp;

    // ES: Simula al servidor enviando posiciones cada 50 ms (KMP_TICK_RATE = 20 Hz).
    //     Se usan deltas pequeños (2 unidades) para que no salte la corrección brusca
    //     (KMP_SNAP_THRESHOLD_MIN = 5.0). El primer snapshot fija la velocidad de referencia.
    // EN: Simulate server sending positions at 50ms intervals (matching KMP_TICK_RATE=20Hz).
    // Use small deltas (2 units) so the snap correction system doesn't activate
    // (KMP_SNAP_THRESHOLD_MIN=5.0). First snapshot establishes velocity baseline.
    kmp::Vec3 pos0{0.0f, 0.0f, 0.0f};
    kmp::Vec3 pos1{2.0f, 0.0f, 0.0f};
    kmp::Vec3 pos2{4.0f, 0.0f, 0.0f};
    kmp::Vec3 pos3{6.0f, 0.0f, 0.0f};
    kmp::Quat rot{};

    // ES: Snapshot de calentamiento: fija la velocidad para que la corrección no salte en pos1
    // EN: Warm-up snapshot establishes velocity so snap correction doesn't fire on pos1
    interp.AddSnapshot(1, 1.00f, pos0, rot);
    interp.AddSnapshot(1, 1.05f, pos1, rot);
    interp.AddSnapshot(1, 1.10f, pos2, rot);
    interp.AddSnapshot(1, 1.15f, pos3, rot);

    // ES: La interpolación resta un retardo adaptativo (~50 ms sin jitter) al renderTime.
    //     Para interpolar en el punto medio de [pos1(t=1.05), pos2(t=1.10)] hace falta
    //     interpTime = 1.075, es decir renderTime = 1.075 + 0.05 = 1.125.
    // EN: The interpolation system subtracts an adaptive delay (~50ms when jitter is zero)
    // from renderTime before interpolating. So to get interpTime at the midpoint of
    // [pos1(t=1.05), pos2(t=1.10)], we need interpTime=1.075 → renderTime=1.075+0.05=1.125
    const float delay = kmp::KMP_INTERP_DELAY_MIN; // 50ms — expected with zero-jitter snapshots

    // ES: Consulta en el punto medio entre pos1 (x=2) y pos2 (x=4) → se espera x≈3.0
    // EN: Query at midpoint between pos1 (x=2) and pos2 (x=4) → expect x≈3.0
    kmp::Vec3 result;
    kmp::Quat resultRot;
    float queryTime1 = 1.075f + delay;
    bool ok = interp.GetInterpolated(1, queryTime1, result, resultRot);
    TestAssert(ok, "GetInterpolated returns true");
    TestAssert(FloatEq(result.x, 3.0f, 0.5f), "Interpolated X is ~3.0 (midpoint)");
    printf("    At renderTime=%.3f: interpolated X = %.2f (expected ~3.0)\n", queryTime1, result.x);

    // ES: Consulta en el punto medio entre pos2 (x=4) y pos3 (x=6) → se espera x≈5.0
    // EN: Query at midpoint between pos2 (x=4) and pos3 (x=6) → expect x≈5.0
    float queryTime2 = 1.125f + delay;
    ok = interp.GetInterpolated(1, queryTime2, result, resultRot);
    TestAssert(ok, "GetInterpolated at midpoint returns true");
    TestAssert(FloatEq(result.x, 5.0f, 0.5f), "Interpolated X is ~5.0");
    printf("    At renderTime=%.3f: interpolated X = %.2f (expected ~5.0)\n", queryTime2, result.x);

    // ES: Quitar la entidad: ya no debe interpolarse
    // EN: Remove entity
    interp.RemoveEntity(1);
    ok = interp.GetInterpolated(1, 1.2f, result, resultRot);
    TestAssert(!ok, "GetInterpolated returns false after RemoveEntity");
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 10: Packet round-trip — write and read position update
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 10: ida y vuelta de un paquete C2S_PositionUpdate con PacketWriter/PacketReader:
//     cabecera, contador y CharacterPosition deben leerse igual que se escribieron.
// EN: TEST 10: C2S_PositionUpdate round-trip through PacketWriter/PacketReader:
//     header, count and CharacterPosition must read back as written.
static void Test_PacketRoundTrip() {
    printf("\n=== Test: Packet Round-Trip (Position Update) ===\n");

    // ES: Simula lo que envía OnGameTick: una actualización de posición C2S con un personaje
    // EN: Simulate what OnGameTick sends: a C2S position update with one character
    kmp::PacketWriter writer;
    writer.WriteHeader(kmp::MessageType::C2S_PositionUpdate);

    kmp::MsgC2SPositionUpdate header_msg{};
    header_msg.characterCount = 1;
    writer.WriteRaw(&header_msg, sizeof(header_msg));

    kmp::CharacterPosition charPos{};
    charPos.entityId = 42;
    charPos.posX = -51200.0f;
    charPos.posY = 1600.0f;
    charPos.posZ = 2700.0f;
    charPos.moveSpeed = 128; // ~7.5 m/s
    charPos.animStateId = 1; // walking
    writer.WriteRaw(&charPos, sizeof(charPos));

    // ES: Releerlo (simula al servidor recibiendo el paquete)
    // EN: Read it back (simulating server receiving the packet)
    kmp::PacketReader reader(writer.Data(), writer.Size());
    kmp::PacketHeader header;
    TestAssert(reader.ReadHeader(header), "Reader reads header");
    TestAssert(header.type == kmp::MessageType::C2S_PositionUpdate, "Message type matches");

    kmp::MsgC2SPositionUpdate readHeader{};
    TestAssert(reader.ReadRaw(&readHeader, sizeof(readHeader)), "Reader reads update header");
    TestAssert(readHeader.characterCount == 1, "Character count is 1");

    kmp::CharacterPosition readPos{};
    TestAssert(reader.ReadRaw(&readPos, sizeof(readPos)), "Reader reads character position");
    TestAssert(readPos.entityId == 42, "Entity ID matches");
    TestAssert(FloatEq(readPos.posX, -51200.0f), "Position.x matches");
    TestAssert(FloatEq(readPos.posY, 1600.0f),   "Position.y matches");
    TestAssert(FloatEq(readPos.posZ, 2700.0f),   "Position.z matches");
    TestAssert(readPos.moveSpeed == 128, "MoveSpeed matches");
    TestAssert(readPos.animStateId == 1, "AnimState matches");
    printf("    Position update packet: entity=%u pos=(%.1f, %.1f, %.1f)\n",
           readPos.entityId, readPos.posX, readPos.posY, readPos.posZ);
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 11: Packet round-trip — entity spawn message
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 11: ida y vuelta de un S2C_EntitySpawn escrito como struct MsgEntitySpawn crudo +
//     nombre de plantilla. Ojo: el servidor real serializa este mensaje campo a campo y sin
//     'generation', así que este test no valida el formato real que viaja por la red.
// EN: TEST 11: round-trip of an S2C_EntitySpawn written as a raw MsgEntitySpawn struct +
//     template name. Note: the real server serializes this message field by field and without
//     'generation', so this test doesn't validate the actual wire format.
static void Test_SpawnPacketRoundTrip() {
    printf("\n=== Test: Packet Round-Trip (Entity Spawn) ===\n");

    kmp::PacketWriter writer;
    writer.WriteHeader(kmp::MessageType::S2C_EntitySpawn);

    kmp::MsgEntitySpawn msg{};
    msg.entityId = 100;
    msg.ownerId = 2;
    msg.type = kmp::EntityType::PlayerCharacter;
    msg.posX = -51200.0f;
    msg.posY = 1600.0f;
    msg.posZ = 2700.0f;
    msg.templateId = 12345;
    writer.WriteRaw(&msg, sizeof(msg));
    // ES: Tras el struct va el nombre de plantilla (longitud variable)
    // EN: Variable-length template name follows the struct
    std::string templateName = "Greenlander";
    writer.WriteString(templateName);

    // ES: Releerlo
    // EN: Read it back
    kmp::PacketReader reader(writer.Data(), writer.Size());
    kmp::PacketHeader header;
    TestAssert(reader.ReadHeader(header), "Reader reads spawn header");
    TestAssert(header.type == kmp::MessageType::S2C_EntitySpawn, "Spawn message type matches");

    kmp::MsgEntitySpawn readMsg{};
    TestAssert(reader.ReadRaw(&readMsg, sizeof(readMsg)), "Reader reads spawn payload");
    TestAssert(readMsg.entityId == 100, "Spawn entity ID matches");
    TestAssert(readMsg.ownerId == 2, "Spawn owner matches");
    TestAssert(readMsg.templateId == 12345, "Template ID matches");
    TestAssert(FloatEq(readMsg.posX, -51200.0f), "Spawn position.x matches");

    std::string readName;
    TestAssert(reader.ReadString(readName), "Reader reads template name");
    TestAssert(readName == "Greenlander", "Template name matches");
    printf("    Spawn packet: entity=%u owner=%u template='%s'\n",
           readMsg.entityId, readMsg.ownerId, readName.c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 12: Full spawn flow — server spawn message → registry → accessor
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 12: flujo completo de aparición de un jugador remoto: mensaje del servidor →
//     registro → enlace del objeto del juego → renombrado → facción → movimiento.
// EN: TEST 12: full remote player spawn flow: server message → registry →
//     game object link → rename → faction → movement.
static void Test_FullSpawnFlow() {
    printf("\n=== Test: Full Spawn Flow (Server → Registry → Game Memory) ===\n");

    // ES: Paso 1: recibir el paquete EntitySpawn del servidor
    // EN: Step 1: Receive EntitySpawn packet from server
    kmp::MsgEntitySpawn spawnMsg{};
    spawnMsg.entityId = 200;
    spawnMsg.ownerId = 3;
    spawnMsg.type = kmp::EntityType::PlayerCharacter;
    spawnMsg.posX = -51200.0f;
    spawnMsg.posY = 1600.0f;
    spawnMsg.posZ = 2700.0f;

    // ES: Paso 2: registrar en el EntityRegistry (lo que hace packet_handler.cpp)
    // EN: Step 2: Register in entity registry (what packet_handler.cpp does)
    kmp::EntityRegistry registry;
    kmp::Vec3 spawnPos{spawnMsg.posX, spawnMsg.posY, spawnMsg.posZ};
    kmp::EntityID eid = registry.RegisterRemote(
        spawnMsg.entityId,
        spawnMsg.type,
        spawnMsg.ownerId,
        spawnPos
    );
    TestAssert(eid == 200, "Remote entity registered with server ID");
    TestAssert(registry.GetRemoteCount() == 1, "1 remote entity");

    // ES: Paso 3: entity_hooks repite CharacterCreate → obtenemos un objeto del juego
    // EN: Step 3: entity_hooks replays CharacterCreate → we get a game object
    FakeCharacter fakeGameObj;
    auto& offsets = kmp::game::GetOffsets().character;

    // ES: El objeto empieza con valores por defecto (como los pondría Kenshi)
    // EN: Game object starts with default values (like Kenshi would set)
    fakeGameObj.SetSSOString(offsets.name, "Greenlander");
    fakeGameObj.SetVec3(offsets.position, -51200.0f, 1600.0f, 2700.0f);

    // ES: Enlazar el objeto al registro (lo que hace entity_hooks tras la repetición)
    // EN: Link the game object to the registry (what entity_hooks does after replay)
    registry.SetGameObject(200, fakeGameObj.Ptr());
    TestAssert(registry.GetSpawnedRemoteCount() == 1, "Game object linked to remote entity");

    // ES: Paso 4: PlayerController renombra el personaje con el nombre del jugador remoto
    // EN: Step 4: PlayerController renames the character to the remote player's name
    kmp::game::CharacterAccessor accessor(fakeGameObj.Ptr());
    bool nameOk = accessor.WriteName("Player3");
    TestAssert(nameOk, "WriteName succeeded");
    std::string readName = accessor.GetName();
    TestAssert(readName == "Player3", "Character renamed to remote player's name");

    // ES: Paso 5: escribir el puntero de facción (aliado del jugador local)
    // EN: Step 5: Write faction pointer (allies with local player)
    uintptr_t localFaction = 0x0000020000AABB00;
    bool factionOk = accessor.WriteFaction(localFaction);
    TestAssert(factionOk, "WriteFaction succeeded");
    TestAssert(accessor.GetFactionPtr() == localFaction, "Faction set to local player's faction");

    // ES: Paso 6: el servidor envía posición → interpolación → escritura en el objeto del juego
    // EN: Step 6: Server sends position update → interpolation → write to game object
    kmp::Vec3 newPos{-51180.0f, 1605.0f, 2720.0f};
    bool posOk = accessor.WritePosition(newPos);
    TestAssert(posOk, "WritePosition succeeded");

    kmp::Vec3 readPos = accessor.GetPosition();
    TestAssert(FloatEq(readPos.x, -51180.0f), "Updated position.x in game memory");
    TestAssert(FloatEq(readPos.y, 1605.0f),   "Updated position.y in game memory");
    TestAssert(FloatEq(readPos.z, 2720.0f),   "Updated position.z in game memory");

    printf("    Full flow: spawn -> register -> link -> rename -> faction -> move\n");
    printf("    Character '%s' at (%.1f, %.1f, %.1f) with faction 0x%llX\n",
           readName.c_str(), readPos.x, readPos.y, readPos.z, (unsigned long long)localFaction);
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEST 13: Multiple players — simulate 4-player session
// ═══════════════════════════════════════════════════════════════════════════
// ES: TEST 13: sesión simulada de 4 jugadores (1 local + 3 remotos): aparición, nombres,
//     movimiento, salida de un jugador y limpieza de remotos conservando la entidad local.
// EN: TEST 13: simulated 4-player session (1 local + 3 remote): spawn, names, movement,
//     one player leaving and remote cleanup while keeping the local entity.
static void Test_MultiPlayerSession() {
    printf("\n=== Test: 4-Player Session Simulation ===\n");

    kmp::EntityRegistry registry;
    kmp::Interpolation interp;
    auto& offsets = kmp::game::GetOffsets().character;

    // ES: Jugador local (nosotros) — Jugador 1
    // EN: Local player (us) — Player 1
    FakeCharacter localChar;
    localChar.SetSSOString(offsets.name, "LocalPlayer");
    localChar.SetVec3(offsets.position, 0.0f, 100.0f, 0.0f);
    kmp::EntityID localId = registry.Register(localChar.Ptr(), kmp::EntityType::PlayerCharacter, 1);

    // ES: Jugadores remotos 2, 3 y 4
    // EN: Remote players 2, 3, 4
    FakeCharacter remote2, remote3, remote4;
    const char* names[] = {"Alice", "Bob", "Charlie"};
    float xPositions[] = {100.0f, 200.0f, 300.0f};

    kmp::EntityID remoteIds[3];
    FakeCharacter* remotes[] = {&remote2, &remote3, &remote4};

    for (int i = 0; i < 3; i++) {
        kmp::Vec3 pos{xPositions[i], 100.0f, 0.0f};
        remoteIds[i] = registry.RegisterRemote(50 + i, kmp::EntityType::PlayerCharacter, 2 + i, pos);

        // ES: Simular la repetición del spawn: preparar el objeto del juego
        // EN: Simulate spawn replay: set up game object
        remotes[i]->SetSSOString(offsets.name, "Greenlander");
        remotes[i]->SetVec3(offsets.position, xPositions[i], 100.0f, 0.0f);
        registry.SetGameObject(remoteIds[i], remotes[i]->Ptr());

        // ES: Renombrar con el nombre del jugador
        // EN: Rename to player name
        kmp::game::CharacterAccessor acc(remotes[i]->Ptr());
        acc.WriteName(names[i]);

        // ES: Añadir snapshots de interpolación
        // EN: Add interpolation snapshots
        interp.AddSnapshot(remoteIds[i], 1.0f, pos, kmp::Quat{});
    }

    TestAssert(registry.GetEntityCount() == 4, "4 entities total in registry");
    TestAssert(registry.GetRemoteCount() == 3, "3 remote entities");
    TestAssert(registry.GetSpawnedRemoteCount() == 3, "3 spawned remotes");

    // ES: Verificar todos los nombres
    // EN: Verify all names
    for (int i = 0; i < 3; i++) {
        kmp::game::CharacterAccessor acc(remotes[i]->Ptr());
        TestAssert(acc.GetName() == names[i], (std::string("Remote ") + names[i] + " name correct").c_str());
    }

    // ES: Simular movimiento: el servidor envía posiciones nuevas
    // EN: Simulate movement: server sends new positions
    for (int i = 0; i < 3; i++) {
        kmp::Vec3 movedPos{xPositions[i] + 50.0f, 100.0f, 10.0f};
        interp.AddSnapshot(remoteIds[i], 1.1f, movedPos, kmp::Quat{});

        // ES: Escribir en la memoria del juego (lo que hace OnGameTick tras interpolar)
        // EN: Write to game memory (what OnGameTick does after interpolation)
        kmp::game::CharacterAccessor acc(remotes[i]->Ptr());
        acc.WritePosition(movedPos);
    }

    // ES: Verificar que todos se movieron
    // EN: Verify all moved
    for (int i = 0; i < 3; i++) {
        kmp::game::CharacterAccessor acc(remotes[i]->Ptr());
        kmp::Vec3 pos = acc.GetPosition();
        TestAssert(FloatEq(pos.x, xPositions[i] + 50.0f), (std::string(names[i]) + " moved to correct X").c_str());
    }

    // ES: Simular que un jugador se desconecta
    // EN: Simulate one player disconnecting
    registry.Unregister(remoteIds[1]); // Bob leaves
    interp.RemoveEntity(remoteIds[1]);
    TestAssert(registry.GetRemoteCount() == 2, "After Bob leaves: 2 remotes");

    // ES: Limpiar todos los remotos (desconexión total)
    // EN: Clear all remotes (full disconnect)
    size_t cleared = registry.ClearRemoteEntities();
    TestAssert(cleared == 2, "ClearRemoteEntities removed 2 remaining");
    TestAssert(registry.GetRemoteCount() == 0, "No remotes after clear");
    // ES: La entidad local debe seguir existiendo
    // EN: Local entity should still exist
    TestAssert(registry.GetEntityCount() == 1, "Local entity survives remote clear");

    printf("    4-player session: spawn, move, disconnect, cleanup — all verified\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Host assignment protocol tests
// ═══════════════════════════════════════════════════════════════════════════
// ES: Tests del protocolo de asignación de host: valor 0x92 del enum, tamaño del struct
//     MsgHostAssignment (sin relleno) e ida y vuelta por un búfer de bytes.
// EN: Host assignment protocol tests: 0x92 enum value, MsgHostAssignment struct size
//     (no padding) and a round-trip through a byte buffer.
static void TestHostAssignmentProtocol() {
    printf("\nHost assignment protocol tests:\n");

    // ES: El valor del enum importa: clientes de builds distintas deben coincidir en 0x92.
    // EN: The enum value is load-bearing: clients of different builds must agree on 0x92.
    TestAssert(static_cast<uint8_t>(kmp::MessageType::S2C_HostAssignment) == 0x92,
               "S2C_HostAssignment enum value is 0x92");

    // ES: Layout del struct: un único campo PlayerID, sin relleno inesperado.
    // EN: Struct layout: one PlayerID field, no padding surprises.
    TestAssert(sizeof(kmp::MsgHostAssignment) == sizeof(kmp::PlayerID),
               "MsgHostAssignment is exactly sizeof(PlayerID)");

    // ES: Ida y vuelta: escribir un MsgHostAssignment, releerlo y comparar.
    // EN: Round-trip: write a MsgHostAssignment, read it back, compare.
    kmp::MsgHostAssignment original{};
    original.newHostPlayerId = 42;
    uint8_t buf[sizeof(original)];
    memcpy(buf, &original, sizeof(original));
    kmp::MsgHostAssignment copy{};
    memcpy(&copy, buf, sizeof(copy));
    TestAssert(copy.newHostPlayerId == 42, "MsgHostAssignment round-trips through byte buffer");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Loopback detection test
// ═══════════════════════════════════════════════════════════════════════════
// ES: Test de detección de loopback (127.0.0.1). Aquí no se enlaza ENet: se reimplementa
//     el predicado con la misma lógica que usa el servidor y se prueba el patrón de bits.
//     Ojo: al ser una copia, no detecta si la función real del servidor cambia.
// EN: We don't link to ENet here. Re-implement the predicate inline with the exact
//     same logic the server uses, then test the bit pattern.
//     Note: being a copy, it won't catch changes in the server's real function.
static bool TestOnly_IsLoopbackHost(uint32_t hostNetOrder) {
    // ES: ENet guarda address.host en orden de red. 127.0.0.1 en orden de host es
    //     0x7F000001; en orden de red en sistemas little-endian los bytes se invierten: 0x0100007F.
    // EN: ENet stores address.host in network byte order. 127.0.0.1 in host byte
    // order is 0x7F000001; in network byte order on little-endian systems, the
    // bytes reverse to 0x0100007F.
    return hostNetOrder == 0x0100007Fu;
}

// ES: Casos: 127.0.0.1 sí es loopback; 192.168.1.1, 0.0.0.0 y 127.0.0.2 no (comparación exacta).
// EN: Cases: 127.0.0.1 is loopback; 192.168.1.1, 0.0.0.0 and 127.0.0.2 are not (exact match).
static void TestLoopbackDetection() {
    printf("\nLoopback detection tests:\n");

    // ES: 127.0.0.1 en orden de red
    // EN: 127.0.0.1 in network byte order
    TestAssert(TestOnly_IsLoopbackHost(0x0100007Fu), "127.0.0.1 detected as loopback");

    // 192.168.1.1 (LAN) — should NOT be loopback
    TestAssert(!TestOnly_IsLoopbackHost(0x0101A8C0u), "192.168.1.1 detected as non-loopback");

    // 0.0.0.0 (INADDR_ANY) — should NOT be loopback
    TestAssert(!TestOnly_IsLoopbackHost(0u), "0.0.0.0 detected as non-loopback");

    // ES: 127.0.0.2 — otra IP del rango loopback; no debe coincidir con la comprobación exacta
    //     (la spec usa coincidencia exacta con 127.0.0.1; otras IPs loopback son raras en la práctica)
    // EN: 127.0.0.2 — different loopback-range IP, should still be non-match for our exact check
    // (spec uses exact 127.0.0.1 match; other loopback IPs are rare in practice)
    TestAssert(!TestOnly_IsLoopbackHost(0x0200007Fu), "127.0.0.2 not matched (exact 127.0.0.1 only)");
}

// ═══════════════════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════════════════
// ES: Ejecuta todos los tests en orden, imprime el resumen y devuelve 1 si alguno falló
//     (código de salida útil para CI/ctest).
// EN: Runs every test in order, prints the summary and returns 1 if any failed
//     (exit code usable by CI/ctest).
int main() {
    printf("======================================\n");
    printf("  KenshiMP Unit Test Suite\n");
    printf("  Tests game-facing code with fake memory\n");
    printf("======================================\n");

    Test_ReadPosition();
    Test_ReadName();
    Test_WriteName();
    Test_WritePosition();
    Test_FactionReadWrite();
    Test_EntityRegistry();
    Test_RemoteEntityLifecycle();
    Test_EntityRemap();
    Test_Interpolation();
    Test_PacketRoundTrip();
    Test_SpawnPacketRoundTrip();
    Test_FullSpawnFlow();
    Test_MultiPlayerSession();
    TestHostAssignmentProtocol();
    TestLoopbackDetection();

    printf("\n======================================\n");
    printf("  Results: %d passed, %d failed\n", g_passed, g_failed);
    printf("======================================\n");

    if (g_failed > 0) {
        printf("\nSome tests FAILED!\n");
    } else {
        printf("\nAll tests PASSED!\n");
    }

    return g_failed > 0 ? 1 : 0;
}
