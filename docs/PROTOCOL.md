# Kenshi-Online Network Protocol Specification

**Version:** 1  
**Last Updated:** 2026-06-04  
**Authors:** KenshiMP Development Team

---

## Table of Contents

1. [Overview](#overview)
2. [Transport Layer](#transport-layer)
3. [Message Types](#message-types)
4. [Message Structures](#message-structures)
5. [Packet Flow Diagrams](#packet-flow-diagrams)
6. [Channel Usage](#channel-usage)
7. [Serialization](#serialization)
8. [Constants and Limits](#constants-and-limits)

---

## 1. Overview

Kenshi-Online uses a client-server architecture with authoritative server design. The protocol is built on **ENet** for reliable UDP transport with support for:

- **16 concurrent players**
- **20Hz tick rate** (50ms intervals)
- **Client-side prediction with server reconciliation**
- **Zone-based interest management** (3×3 grid, 750m zones)
- **Entity generation counters** to prevent ghost control bugs

**Protocol Version:** `1`  
**Default Port:** `27800`

---

## 2. Transport Layer

### 2.1 ENet Configuration

```cpp
Host Configuration:
- Channels: 3
- Upstream limit: 2 MB/s (server → client fan-out)
- Downstream limit: 2 MB/s (client → server)
- Peer timeout: 30s (connect), 60s (session)
- Keepalive interval: 1s
```

### 2.2 Channels

| Channel | Type | Usage | ENet Flags |
|---------|------|-------|------------|
| **0** | Reliable Ordered | Connection lifecycle, entity spawn/despawn, world state, chat, admin | `ENET_PACKET_FLAG_RELIABLE` |
| **1** | Reliable Unordered | Combat events, stats, inventory, equipment | `ENET_PACKET_FLAG_RELIABLE` |
| **2** | Unreliable Sequenced | Position updates, movement commands | `0` (drops late packets) |

**Why Unreliable Sequenced?**  
Channel 2 uses `flags=0` (unreliable + sequenced). ENet automatically drops late/out-of-order packets, preventing stale position data from overwriting current state. `ENET_PACKET_FLAG_UNSEQUENCED` would deliver ALL packets regardless of order, causing jitter.

---

## 3. Message Types

All message types are prefixed with `C2S_` (Client-to-Server) or `S2C_` (Server-to-Client).

### 3.1 Connection Messages (Channel 0)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `C2S_Handshake` | `0x01` | C→S | Initial connection request with player name, protocol version, game version |
| `S2C_HandshakeAck` | `0x02` | S→C | Connection accepted, assigns player ID, sends world state |
| `S2C_HandshakeReject` | `0x03` | S→C | Connection rejected (full/version mismatch/banned) |
| `C2S_Disconnect` | `0x04` | C→S | Graceful disconnect notification |
| `S2C_PlayerJoined` | `0x05` | S→C | Another player joined the server |
| `S2C_PlayerLeft` | `0x06` | S→C | Another player left the server |
| `C2S_Keepalive` | `0x07` | C→S | Heartbeat to prevent timeout |
| `S2C_KeepaliveAck` | `0x08` | S→C | Keepalive acknowledgment |
| `C2S_PlayerReady` | `0x09` | C→S | Client is in-game and ready to spawn |
| `S2C_AllPlayersReady` | `0x0A` | S→C | All players ready, begin spawning entities |

### 3.2 World State Messages (Channel 0)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `S2C_WorldSnapshot` | `0x10` | S→C | Full world state snapshot (initial sync) |
| `S2C_TimeSync` | `0x11` | S→C | Game time, weather, speed synchronization |
| `S2C_ZoneData` | `0x12` | S→C | Zone data for client interest area |
| `C2S_ZoneRequest` | `0x13` | C→S | Request zone data (on zone transition) |
| `S2C_EntityHeartbeat` | `0x14` | S→C | Periodic entity presence list (every 5s) |
| `C2S_EntityAck` | `0x15` | C→S | Client confirms heartbeat receipt (optional) |

### 3.3 Entity Lifecycle (Channel 0)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `S2C_EntitySpawn` | `0x20` | S→C | Spawn entity with generation counter |
| `S2C_EntityDespawn` | `0x21` | S→C | Remove entity from world |
| `C2S_EntitySpawnReq` | `0x22` | C→S | Request entity spawn (squad member, summon) |
| `C2S_EntityDespawnReq` | `0x23` | C→S | Request entity removal |

### 3.4 Movement (Channel 2 - Unreliable)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `C2S_PositionUpdate` | `0x30` | C→S | Client position/rotation for owned entities |
| `S2C_PositionUpdate` | `0x31` | S→C | Broadcast position updates from other players |
| `C2S_MoveCommand` | `0x32` | C→S | Player issued move-to command |
| `S2C_MoveCommand` | `0x33` | S→C | Server-authoritative move command |

### 3.5 Combat (Channel 1 - Reliable Unordered)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `C2S_AttackIntent` | `0x40` | C→S | Player initiated attack |
| `S2C_CombatHit` | `0x41` | S→C | Server-resolved hit (damage, body part, block) |
| `S2C_CombatBlock` | `0x42` | S→C | Attack was blocked |
| `S2C_CombatDeath` | `0x43` | S→C | Entity died |
| `S2C_CombatKO` | `0x44` | S→C | Entity knocked out |
| `C2S_CombatStance` | `0x45` | C→S | Change combat stance (passive/defensive/aggressive) |
| `C2S_CombatDeath` | `0x46` | C→S | Client reports entity death |
| `C2S_CombatKO` | `0x47` | C→S | Client reports entity knockout |

### 3.6 Stats & Health (Channel 1)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `S2C_StatUpdate` | `0x50` | S→C | Stat level/XP change |
| `S2C_HealthUpdate` | `0x51` | S→C | Overall health update |
| `S2C_EquipmentUpdate` | `0x52` | S→C | Server broadcasts equipment change |
| `C2S_EquipmentUpdate` | `0x53` | C→S | Client changed equipment |
| `C2S_LimbHealth` | `0x54` | C→S | Client reports per-limb health |
| `S2C_LimbHealth` | `0x55` | S→C | Server broadcasts per-limb health |
| `C2S_StatusEffect` | `0x56` | C→S | Client reports status effect (bleeding, KO, crippled) |
| `S2C_StatusEffect` | `0x57` | S→C | Server broadcasts status effect |

### 3.7 Inventory & Trade (Channel 1)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `C2S_ItemPickup` | `0x60` | C→S | Character picked up item |
| `C2S_ItemDrop` | `0x61` | C→S | Character dropped item |
| `C2S_ItemTransfer` | `0x62` | C→S | Transfer item between inventories |
| `S2C_InventoryUpdate` | `0x63` | S→C | Inventory change notification |
| `C2S_TradeRequest` | `0x64` | C→S | Initiate trade with NPC/player |
| `S2C_TradeResult` | `0x65` | S→C | Trade success/failure |

### 3.8 Buildings (Channel 0)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `C2S_BuildRequest` | `0x70` | C→S | Place building blueprint |
| `S2C_BuildPlaced` | `0x71` | S→C | Building placed in world |
| `S2C_BuildProgress` | `0x72` | S→C | Construction progress update |
| `S2C_BuildDestroyed` | `0x73` | S→C | Building destroyed |
| `C2S_DoorInteract` | `0x74` | C→S | Open/close/lock door |
| `S2C_DoorState` | `0x75` | S→C | Door state changed |
| `C2S_BuildDismantle` | `0x76` | C→S | Dismantle building |
| `C2S_BuildRepair` | `0x77` | C→S | Repair building |

### 3.9 Chat & Admin (Channel 0)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `C2S_ChatMessage` | `0x80` | C→S | Send chat message |
| `S2C_ChatMessage` | `0x81` | S→C | Broadcast chat message |
| `S2C_SystemMessage` | `0x82` | S→C | System notification |
| `C2S_AdminCommand` | `0x90` | C→S | Admin command (kick, ban, time, weather) |
| `S2C_AdminResponse` | `0x91` | S→C | Admin command result |
| `S2C_HostAssignment` | `0x92` | S→C | Host identity assignment/reassignment |

### 3.10 Server Query (Channel 0)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `C2S_ServerQuery` | `0xA0` | C→S | Lightweight server info query (no handshake) |
| `S2C_ServerInfo` | `0xA1` | S→C | Server name, players, time, PvP status |

### 3.11 Squad & Faction (Channel 0)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `C2S_SquadCreate` | `0xB0` | C→S | Create new squad |
| `S2C_SquadCreated` | `0xB1` | S→C | Squad created (server-assigned ID) |
| `C2S_SquadAddMember` | `0xB2` | C→S | Add member to squad |
| `S2C_SquadMemberUpdate` | `0xB3` | S→C | Squad membership changed |
| `C2S_FactionRelation` | `0xC0` | C→S | Faction reputation change |
| `S2C_FactionRelation` | `0xC1` | S→C | Server broadcasts faction relation |

### 3.12 Master Server (Channel 0)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `MS_Register` | `0xD0` | GameServer→Master | Register game server |
| `MS_Heartbeat` | `0xD1` | GameServer→Master | Keepalive heartbeat |
| `MS_Deregister` | `0xD2` | GameServer→Master | Shutdown notification |
| `MS_QueryList` | `0xD3` | Client→Master | Request server list |
| `MS_ServerList` | `0xD4` | Master→Client | Full server list response |

### 3.13 Pipeline Debug (Channel 1)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `C2S_PipelineSnapshot` | `0xE0` | C→S | Spawn pipeline state snapshot |
| `S2C_PipelineSnapshot` | `0xE1` | S→C | Forwarded snapshot from peer |
| `C2S_PipelineEvent` | `0xE2` | C→S | Pipeline event batch |
| `S2C_PipelineEvent` | `0xE3` | S→C | Forwarded events from peer |

### 3.14 Lobby (Channel 0)

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `S2C_FactionAssignment` | `0xF0` | S→C | Server assigns faction string to client |
| `C2S_LobbyReady` | `0xF1` | C→S | Client confirms faction loaded |
| `S2C_LobbyStart` | `0xF2` | S→C | Server tells all clients to start |

---

## 4. Message Structures

### 4.1 Packet Header

```cpp
#pragma pack(push, 1)
struct PacketHeader {
    MessageType type;      // 1 byte
    uint8_t     flags;     // Bit 0: compressed, Bits 1-7: reserved
    uint16_t    sequence;  // Packet sequence number
    uint32_t    timestamp; // Server tick
};
#pragma pack(pop)
// Total: 8 bytes
```

### 4.2 Connection Messages

#### MsgHandshake (C2S_Handshake)
```cpp
struct MsgHandshake {
    uint32_t protocolVersion; // Must match KMP_PROTOCOL_VERSION (1)
    char     playerName[32];  // Null-terminated, max 31 chars
    uint8_t  gameVersionMajor;
    uint8_t  gameVersionMinor;
    uint8_t  gameVersionPatch;
    uint8_t  reserved;
};
```

#### MsgHandshakeAck (S2C_HandshakeAck)
```cpp
struct MsgHandshakeAck {
    PlayerID playerId;        // Assigned player ID (1-255)
    uint32_t serverTick;      // Current server tick
    float    timeOfDay;       // 0.0 to 1.0
    int32_t  weatherState;    // Weather enum value
    uint8_t  maxPlayers;      // Server capacity (16)
    uint8_t  currentPlayers;  // Current player count
    uint16_t reserved;
};
```

#### MsgHandshakeReject (S2C_HandshakeReject)
```cpp
struct MsgHandshakeReject {
    uint8_t reasonCode;    // 0=full, 1=version mismatch, 2=banned, 3=other
    char    reasonText[128]; // Human-readable reason
};
```

#### MsgPlayerJoined (S2C_PlayerJoined)
```cpp
struct MsgPlayerJoined {
    PlayerID playerId;
    char     playerName[32];
};
```

#### MsgPlayerLeft (S2C_PlayerLeft)
```cpp
struct MsgPlayerLeft {
    PlayerID playerId;
    uint8_t  reason; // 0=disconnect, 1=timeout, 2=kicked
};
```

### 4.3 Entity Messages

#### MsgEntitySpawn (S2C_EntitySpawn)
```cpp
struct MsgEntitySpawn {
    EntityID    entityId;        // Unique entity ID
    uint32_t    generation;      // Generation counter (prevents ghost control)
    EntityType  type;            // PlayerCharacter/NPC/Building/Item
    PlayerID    ownerId;         // Owner player ID (0 = server-owned NPC)
    uint32_t    templateId;      // Game data template reference
    float       posX, posY, posZ; // World position
    uint32_t    compressedQuat;  // Smallest-three quaternion
    uint32_t    factionId;       // Faction ID
    // Followed by variable-length data:
    // - uint16_t nameLength
    // - char name[nameLength]
    // - Equipment array (14 slots × uint32_t itemTemplateId)
    // - Stats array (dynamic)
};
```

**Generation Counter:**  
Prevents ghost control bugs. When entity 55 is destroyed and ID is reused, the generation increments. Old packets for entity 55 (generation 1) are rejected when entity 55 (generation 2) exists.

#### MsgEntityDespawn (S2C_EntityDespawn)
```cpp
struct MsgEntityDespawn {
    EntityID entityId;
    uint32_t generation; // Verify we're despawning the right generation
    uint8_t  reason;     // 0=normal, 1=killed, 2=out of range
};
```

### 4.4 Movement Messages

#### CharacterPosition (embedded in position updates)
```cpp
struct CharacterPosition {
    EntityID entityId;
    uint32_t generation;          // Verify entity identity
    float    posX, posY, posZ;
    uint32_t compressedQuat;      // Smallest-three quaternion (10 bits/component)
    uint8_t  animStateId;         // Animation state enum
    uint8_t  moveSpeed;           // 0-255 mapped to 0.0-15.0 m/s
    uint16_t flags;               // Bit 0: running, Bit 1: sneaking, Bit 2: in combat
};
```

#### MsgC2SPositionUpdate (C2S_PositionUpdate)
```cpp
struct MsgC2SPositionUpdate {
    uint8_t characterCount;
    // Followed by characterCount × CharacterPosition
};
```

#### MsgS2CPositionUpdate (S2C_PositionUpdate)
```cpp
struct MsgS2CPositionUpdate {
    PlayerID sourcePlayer;        // Which player sent this update
    uint8_t  characterCount;
    // Followed by characterCount × CharacterPosition
};
```

### 4.5 Combat Messages

#### MsgAttackIntent (C2S_AttackIntent)
```cpp
struct MsgAttackIntent {
    EntityID attackerId;
    EntityID targetId;
    uint8_t  attackType; // 0=melee, 1=ranged
};
```

#### MsgCombatHit (S2C_CombatHit)
```cpp
struct MsgCombatHit {
    EntityID attackerId;
    EntityID targetId;
    uint8_t  bodyPart;        // BodyPart enum (Head=0, Chest=1, etc.)
    float    cutDamage;
    float    bluntDamage;
    float    pierceDamage;
    float    resultHealth;    // Target's health after hit
    uint8_t  wasBlocked;      // 0=hit, 1=partial block, 2=full block
    uint8_t  wasKO;           // 1 if target knocked out
};
```

#### MsgCombatDeath (S2C_CombatDeath)
```cpp
struct MsgCombatDeath {
    EntityID entityId;
    EntityID killerId;        // 0 if environmental death
};
```

### 4.6 Stats & Health

#### MsgHealthUpdate (S2C_HealthUpdate)
```cpp
struct MsgHealthUpdate {
    EntityID entityId;
    float    health[7];       // Per body part: Head, Chest, Stomach, LArm, RArm, LLeg, RLeg
    float    bloodLevel;      // 0.0 to 100.0
};
```

#### MsgLimbHealth (C2S_LimbHealth / S2C_LimbHealth)
```cpp
struct MsgLimbHealth {
    EntityID entityId;
    float    health[7];       // Head, Chest, Stomach, LArm, RArm, LLeg, RLeg
};
```

#### MsgStatusEffect (C2S_StatusEffect / S2C_StatusEffect)
```cpp
enum StatusEffectType : uint8_t {
    StatusEffect_None        = 0,
    StatusEffect_Bleeding    = 1,
    StatusEffect_Unconscious = 2,
    StatusEffect_Crippled    = 3,
    StatusEffect_Bandaged    = 4,
};

struct MsgStatusEffect {
    EntityID entityId;
    uint8_t  effectType;      // StatusEffectType
    uint8_t  active;          // 0=inactive, 1=active
};
```

#### MsgEquipmentUpdate (S2C_EquipmentUpdate / C2S_EquipmentUpdate)
```cpp
struct MsgEquipmentUpdate {
    EntityID entityId;
    uint8_t  slot;            // EquipSlot enum (Weapon=0, Back=1, etc.)
    uint32_t itemTemplateId;  // 0 = empty
};
```

### 4.7 Chat

#### MsgChatMessage (C2S_ChatMessage / S2C_ChatMessage)
```cpp
struct MsgChatMessage {
    PlayerID senderId; // 0 = system message
    // Followed by:
    // - uint16_t messageLength
    // - char message[messageLength]
};
```

### 4.8 Time & World State

#### MsgTimeSync (S2C_TimeSync)
```cpp
struct MsgTimeSync {
    uint32_t serverTick;
    float    timeOfDay;       // 0.0 to 1.0
    int32_t  weatherState;    // Weather enum
    float    gameSpeed;       // 0.1 to 10.0 (time multiplier)
};
```

#### MsgEntityHeartbeat (S2C_EntityHeartbeat)
```cpp
struct MsgEntityHeartbeat {
    uint32_t serverTick;
    uint16_t entityCount;
    // Followed by entityCount × EntityID (uint32_t each)
};
```

**Purpose:**  
Server sends every 5 seconds with list of all entity IDs that should exist on client. Client compares against local state and cleans up orphaned entities or requests missing ones.

### 4.9 Admin Messages

#### MsgAdminCommand (C2S_AdminCommand)
```cpp
struct MsgAdminCommand {
    uint8_t  commandType;     // 0=kick, 1=ban, 2=setTime, 3=setWeather, 4=announce, 5=setSpeed
    PlayerID targetPlayerId;  // For kick/ban
    float    floatParam;      // For setTime/setWeather/setSpeed
    char     textParam[128];  // For announce, kick reason
};
```

#### MsgHostAssignment (S2C_HostAssignment)
```cpp
struct MsgHostAssignment {
    PlayerID newHostPlayerId; // Current host player ID
};
```

**Purpose:**  
Sent when host is first assigned or when current host disconnects and another player takes over. Clients use this to enable/disable admin UI.

---

## 5. Packet Flow Diagrams

### 5.1 Connection Handshake

```
Client                                Server
  |                                     |
  |-- C2S_Handshake ------------------>|
  |   (name, protocol v1, game ver)    |
  |                                     | [Validate version]
  |                                     | [Check capacity]
  |                                     | [Assign PlayerID]
  |<-- S2C_HandshakeAck ---------------|
  |   (PlayerID, tick, time, weather)  |
  |                                     |
  |<-- S2C_PlayerJoined (others) ------|
  |   (existing players)                |
  |                                     |
  |<-- S2C_HostAssignment -------------|
  |   (host PlayerID)                   |
  |                                     |
  |-- C2S_PlayerReady ---------------->|
  |   (game loaded, ready to spawn)    |
  |                                     | [Wait for all players ready]
  |<-- S2C_AllPlayersReady ------------|
  |                                     |
  |<-- S2C_EntitySpawn (×N) -----------|
  |   (spawn all existing entities)    |
  |                                     |
  [Normal gameplay begins]
```

### 5.2 Entity Spawn Sequence

```
Client A                    Server                     Client B
  |                           |                           |
  |-- C2S_EntitySpawnReq ---->|                           |
  |   (squad member)          |                           |
  |                           | [Allocate EntityID]       |
  |                           | [Assign generation]       |
  |<-- S2C_EntitySpawn -------|-- S2C_EntitySpawn ------->|
  |   (ID=55, gen=1, ...)     |   (ID=55, gen=1, ...)     |
  |                           |                           |
  [Both clients spawn entity 55 (gen 1)]
  |                           |                           |
  |-- C2S_PositionUpdate ---->|                           |
  |   (ID=55, gen=1, pos)     |                           |
  |                           |-- S2C_PositionUpdate ---->|
  |                           |   (ID=55, gen=1, pos)     |
  |                           |                           |
  [Entity 55 despawns]        |                           |
  |<-- S2C_EntityDespawn ------|-- S2C_EntityDespawn ----->|
  |   (ID=55, gen=1, reason)  |   (ID=55, gen=1, reason)  |
  |                           |                           |
  [ID 55 reused for new entity - generation increments]
  |<-- S2C_EntitySpawn -------|-- S2C_EntitySpawn ------->|
  |   (ID=55, gen=2, ...)     |   (ID=55, gen=2, ...)     |
  |                           |                           |
  [Old packet arrives late]   |                           |
  |<-- (stale packet) ---------|                          |
  |   (ID=55, gen=1, pos)     |                           |
  [REJECTED - generation mismatch]
```

### 5.3 Position Update (20Hz Loop)

```
Client A                    Server                     Client B
  |                           |                           |
  [Every 50ms]                |                           |
  |-- C2S_PositionUpdate ---->|                           |
  |   (ID=1, gen=1, pos/rot)  |                           |
  |                           |-- S2C_PositionUpdate ---->|
  |                           |   (player=A, ID=1, ...)   |
  |                           |                           |
  |                           |<-- C2S_PositionUpdate ----|
  |                           |   (ID=2, gen=1, pos/rot)  |
  |<-- S2C_PositionUpdate -----|                          |
  |   (player=B, ID=2, ...)   |                           |
```

**Channel:** 2 (Unreliable Sequenced)  
**Rate:** 20Hz (50ms interval)  
**Batching:** Up to 8 characters per packet (squad support)

### 5.4 Combat Resolution

```
Client A                    Server                     Client B
  |                           |                           |
  |-- C2S_AttackIntent ------>|                           |
  |   (attacker=1, target=2)  |                           |
  |                           | [Roll hit/miss]           |
  |                           | [Calculate damage]        |
  |                           | [Apply to target]         |
  |<-- S2C_CombatHit ---------|-- S2C_CombatHit --------->|
  |   (damage, bodyPart, etc) |   (damage, bodyPart, etc) |
  |                           |                           |
  |                           | [Check KO threshold]      |
  |<-- S2C_CombatKO ----------|-- S2C_CombatKO ---------->|
  |   (entityId=2, ...)       |   (entityId=2, ...)       |
  |                           |                           |
  |                           | [Check death threshold]   |
  |<-- S2C_CombatDeath -------|-- S2C_CombatDeath ------->|
  |   (entityId=2, killer=1)  |   (entityId=2, killer=1)  |
```

**Channel:** 1 (Reliable Unordered)  
**Authority:** Server-authoritative — client attacks are requests, server resolves outcome

### 5.5 Entity Heartbeat (Orphan Cleanup)

```
Server                     Client
  |                           |
  [Every 5 seconds]           |
  |-- S2C_EntityHeartbeat --->|
  |   (tick, count, [IDs...]) |
  |                           | [Compare against local entities]
  |                           | [Remove orphaned entities not in list]
  |                           | [Request missing entities via spawn req]
  |<-- C2S_EntityAck (opt) ---|
```

**Purpose:** Prevents entity desync due to dropped packets. Server sends canonical entity list, client cleans up ghosts.

---

## 6. Channel Usage

### Channel 0: Reliable Ordered (ENET_PACKET_FLAG_RELIABLE)

**Purpose:** Connection lifecycle, entity spawn/despawn, world state changes, chat, admin commands.

**Guarantees:**
- Delivery guaranteed
- Order preserved
- Retransmission on packet loss

**Messages:**
- Connection (Handshake, PlayerJoined/Left, Keepalive)
- Entity lifecycle (Spawn, Despawn)
- World state (TimeSync, EntityHeartbeat)
- Buildings (BuildPlaced, DoorState)
- Squad/Faction (SquadCreated, FactionRelation)
- Chat (ChatMessage, SystemMessage)
- Admin (AdminCommand, HostAssignment)

### Channel 1: Reliable Unordered (ENET_PACKET_FLAG_RELIABLE)

**Purpose:** Combat events, stats, inventory, equipment. Delivery guaranteed but order doesn't matter.

**Guarantees:**
- Delivery guaranteed
- Order NOT preserved (faster)
- Retransmission on packet loss

**Why unordered?**  
Combat hits can arrive out-of-order without breaking gameplay. Final health state is what matters, not the order of individual hits. Unordered delivery reduces head-of-line blocking.

**Messages:**
- Combat (AttackIntent, CombatHit, CombatKO, CombatDeath)
- Stats (StatUpdate, HealthUpdate, EquipmentUpdate, LimbHealth, StatusEffect)
- Inventory (ItemPickup, ItemDrop, ItemTransfer, InventoryUpdate, TradeRequest)

### Channel 2: Unreliable Sequenced (flags = 0)

**Purpose:** Position updates, movement commands. Drop late packets, keep only latest.

**Guarantees:**
- Delivery NOT guaranteed
- Out-of-order packets DROPPED (sequenced)
- No retransmission

**Why sequenced, not unsequenced?**  
- `flags=0` (sequenced): ENet drops late packets → prevents jitter from stale positions
- `ENET_PACKET_FLAG_UNSEQUENCED` (unsequenced): ALL packets delivered → stale data overwrites current state → jitter

**Messages:**
- Movement (C2S_PositionUpdate, S2C_PositionUpdate, MoveCommand)

**Interpolation Buffer:**  
Clients maintain 100ms interpolation buffer (8 snapshots @ 20Hz). If position packet drops, client extrapolates up to 250ms before snapping to next received position.

---

## 7. Serialization

### 7.1 PacketWriter

```cpp
class PacketWriter {
public:
    void WriteHeader(MessageType type, uint16_t seq = 0, uint32_t tick = 0);
    void WriteU8(uint8_t v);
    void WriteU16(uint16_t v);
    void WriteU32(uint32_t v);
    void WriteI32(int32_t v);
    void WriteF32(float v);
    void WriteVec3(float x, float y, float z);
    void WriteString(const std::string& s); // uint16_t length + chars
    void WriteRaw(const void* data, size_t len);

    const uint8_t* Data() const;
    size_t Size() const;
};
```

### 7.2 PacketReader

```cpp
class PacketReader {
public:
    PacketReader(const uint8_t* data, size_t size);

    bool ReadHeader(PacketHeader& h);
    bool ReadU8(uint8_t& v);
    bool ReadU16(uint16_t& v);
    bool ReadU32(uint32_t& v);
    bool ReadI32(int32_t& v);
    bool ReadF32(float& v);
    bool ReadVec3(float& x, float& y, float& z);
    bool ReadString(std::string& s, uint16_t maxLen = 1024);
    bool ReadRaw(void* out, size_t len);

    size_t Remaining() const;
    size_t Position() const;
};
```

### 7.3 String Serialization

```
Format: uint16_t length + char data[length]
Max length: 1024 (enforced by ReadString)
No null terminator in wire format
```

### 7.4 Quaternion Compression

**Smallest-Three Encoding:**  
Quaternions are normalized (w² + x² + y² + z² = 1), so we can drop the largest component and reconstruct it:

```
1. Find largest component (w/x/y/z)
2. Store 2-bit index (00=w, 01=x, 10=y, 11=z)
3. Store 3 remaining components in 10 bits each (1024 values)
4. Range: [-0.7071, 0.7071] → [0, 1023]
5. Total: 2 + 10 + 10 + 10 = 32 bits (75% compression vs. 128-bit float quat)

uint32_t compressedQuat = Quat::Compress(quat);
Quat decompressed = Quat::Decompress(compressedQuat);
```

**Precision:** ~0.001 per component (acceptable for gameplay, < 1° error)

---

## 8. Constants and Limits

### 8.1 Protocol Constants

```cpp
KMP_PROTOCOL_VERSION    = 1        // Current protocol version
KMP_DEFAULT_PORT        = 27800    // Default game server port
KMP_MAX_PLAYERS         = 16       // Maximum concurrent players
KMP_MAX_NAME_LENGTH     = 31       // Player name limit (32 bytes with null)
```

### 8.2 Tick Rates

```cpp
KMP_TICK_RATE           = 20       // 20 Hz state sync
KMP_TICK_INTERVAL_MS    = 50       // 50ms per tick
KMP_TICK_INTERVAL_SEC   = 0.05     // 0.05s per tick
```

### 8.3 Interpolation

```cpp
KMP_INTERP_DELAY_SEC    = 0.1      // 100ms interpolation buffer
KMP_MAX_SNAPSHOTS       = 8        // Snapshot ring buffer per entity
KMP_INTERP_DELAY_MIN    = 0.05     // 50ms minimum adaptive delay
KMP_INTERP_DELAY_MAX    = 0.2      // 200ms maximum adaptive delay
KMP_EXTRAP_MAX_SEC      = 0.25     // 250ms max extrapolation
KMP_SNAP_THRESHOLD_MIN  = 5.0      // Below 5m: smooth blend
KMP_SNAP_THRESHOLD_MAX  = 50.0     // Above 50m: instant teleport
```

### 8.4 Networking

```cpp
KMP_CHANNEL_COUNT               = 3
KMP_CHANNEL_RELIABLE_ORDERED    = 0
KMP_CHANNEL_RELIABLE_UNORDERED  = 1
KMP_CHANNEL_UNRELIABLE_SEQ      = 2

KMP_UPSTREAM_LIMIT      = 2 MB/s   // Server → client bandwidth
KMP_DOWNSTREAM_LIMIT    = 2 MB/s   // Client → server bandwidth

KMP_CONNECT_TIMEOUT_MS  = 5000     // Initial connect timeout
KMP_KEEPALIVE_INTERVAL  = 1000     // 1 second keepalive
KMP_TIMEOUT_MS          = 10000    // 10 second session timeout
```

### 8.5 Zone System

```cpp
KMP_ZONE_SIZE           = 750.0    // Meters per zone (estimated)
KMP_INTEREST_RADIUS     = 1        // ±1 zone (3×3 grid)
KMP_AUTHORITY_HYSTERESIS = 64.0    // Units before zone authority transfer
```

### 8.6 Entity Limits

```cpp
KMP_MAX_ENTITIES_PER_ZONE = 512    // Entities per zone
KMP_MAX_SYNC_ENTITIES     = 2048   // Total synced entities per client
```

### 8.7 Entity ID Ranges

```cpp
KMP_ID_PLAYER_MIN       = 1        // Player entity IDs: 1-255
KMP_ID_PLAYER_MAX       = 255
KMP_ID_NPC_MIN          = 256      // NPC entity IDs: 256-8191
KMP_ID_NPC_MAX          = 8191
KMP_ID_BUILDING_MIN     = 8192     // Building entity IDs: 8192-16383
KMP_ID_BUILDING_MAX     = 16383
KMP_ID_CONTAINER_MIN    = 16384    // Container entity IDs: 16384-24575
KMP_ID_CONTAINER_MAX    = 24575
KMP_ID_SQUAD_MIN        = 24576    // Squad entity IDs: 24576-32767
KMP_ID_SQUAD_MAX        = 32767
```

### 8.8 BodyPart Enum

```cpp
enum BodyPart {
    Head      = 0,
    Chest     = 1,
    Stomach   = 2,
    LeftArm   = 3,
    RightArm  = 4,
    LeftLeg   = 5,
    RightLeg  = 6,
    Count     = 7
};
```

### 8.9 EquipSlot Enum

```cpp
enum EquipSlot {
    Weapon    = 0,  Back      = 1,  Hair      = 2,  Hat       = 3,
    Eyes      = 4,  Body      = 5,  Legs      = 6,  Shirt     = 7,
    Boots     = 8,  Gloves    = 9,  Neck      = 10, Backpack  = 11,
    Beard     = 12, Belt      = 13, Count     = 14
};
```

---

## 9. Implementation Notes

### 9.1 Entity Generation Counters

**Problem:** Entity IDs are reused. Entity 55 = Player A, then Entity 55 = NPC. Late packet for Player A arrives → ghost control bug.

**Solution:** Every entity has a `generation` counter that increments on ID reuse. Packets include `(entityId, generation)`. Clients reject mismatched generations.

```cpp
struct NetEntityId {
    uint32_t id;
    uint32_t generation;
};
```

### 9.2 Client-Side Prediction

**Position Updates:**
1. Client sends C2S_PositionUpdate (unreliable, 20Hz)
2. Server broadcasts S2C_PositionUpdate to others (unreliable, 20Hz)
3. Local player uses immediate input (no interpolation)
4. Remote players interpolate with 100ms buffer

**Combat:**
1. Client plays attack animation immediately
2. Server resolves hit/miss/damage
3. Server sends S2C_CombatHit to all clients (reliable)
4. Clients apply server-authoritative result (may correct prediction)

### 9.3 Zone Interest Management

**3×3 Grid (±1 zone):**
- Client at zone (5, 10) receives entities from zones (4-6, 9-11)
- Zone transitions send C2S_ZoneRequest → S2C_ZoneData
- Entities outside 3×3 grid are despawned on client (S2C_EntityDespawn, reason=2)

### 9.4 Server Authority

**Authoritative:**
- Combat resolution (hit/miss/damage/KO/death)
- Entity spawning/despawning
- Inventory validation
- Faction relations
- Building placement

**Client-Driven:**
- Movement prediction
- Animation playback
- UI state
- Camera control

---

## 10. Version History

| Version | Date | Changes |
|---------|------|---------|
| 1 | 2026-06-04 | Initial protocol specification |

---

## 11. References

- **ENet Documentation:** http://enet.bespin.org/
- **Source Files:**
  - `KenshiMP.Common/include/kmp/messages.h` - Message structures
  - `KenshiMP.Common/include/kmp/protocol.h` - Message types and serialization
  - `KenshiMP.Common/include/kmp/types.h` - Core types and enums
  - `KenshiMP.Common/include/kmp/constants.h` - Protocol constants
  - `KenshiMP.Core/net/client.cpp` - Client networking implementation
  - `KenshiMP.Server/server.cpp` - Server networking implementation

---

**End of Protocol Specification**
