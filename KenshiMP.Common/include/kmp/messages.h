// ES: Estructuras binarias de los mensajes de red (payload que va tras la PacketHeader).
//     Todas están empaquetadas a 1 byte (#pragma pack) para que su layout en memoria
//     coincida byte a byte con lo que viaja por la red entre cliente, servidor y master.
//     Algunos mensajes llevan datos de longitud variable a continuación del struct.
// EN: Binary network message structures (payload following the PacketHeader).
//     All are packed to 1 byte (#pragma pack) so their in-memory layout matches
//     byte for byte what travels over the wire between client, server and master.
//     Some messages carry variable-length data after the struct.
#pragma once
#include "types.h"
#include "constants.h"
#include <cstdint>
#include <string>

namespace kmp {

// ES: ── Mensajes de conexión ──
// EN: ── Connection Messages ──

#pragma pack(push, 1)

// ES: Base del handshake — tamaño fijo, compatible con clientes existentes.
//     El campo 'password' se envía OPCIONALMENTE a continuación de este struct
//     (ver lectura tolerante a tamaño en GameServer::HandleHandshake): los clientes
//     que no lo envían siguen funcionando; el server solo lo exige si tiene contraseña.
// EN: Handshake base — fixed size, compatible with existing clients.
//     The 'password' field is OPTIONALLY sent after this struct
//     (see size-tolerant read in GameServer::HandleHandshake): clients that don't
//     send it keep working; the server only requires it if it has a password set.
struct MsgHandshake {
    uint32_t protocolVersion;
    char     playerName[KMP_MAX_NAME_LENGTH + 1];
    uint8_t  gameVersionMajor;
    uint8_t  gameVersionMinor;
    uint8_t  gameVersionPatch;
    uint8_t  reserved;
};

// ES: Longitud máxima de la contraseña de servidor (campo opcional del handshake).
// EN: Maximum server password length (optional handshake field).
constexpr int KMP_MAX_PASSWORD_LENGTH = 63;

// ES: Respuesta de handshake aceptado: ID asignado al jugador, tick del servidor,
//     hora del día, clima y ocupación del servidor.
// EN: Accepted-handshake reply: player ID assigned, server tick, time of day,
//     weather and server occupancy.
struct MsgHandshakeAck {
    PlayerID playerId;
    uint32_t serverTick;
    float    timeOfDay;
    int32_t  weatherState;
    uint8_t  maxPlayers;
    uint8_t  currentPlayers;
    uint16_t reserved;
};

// ES: Handshake rechazado: código y texto del motivo.
// EN: Rejected handshake: reason code and text.
struct MsgHandshakeReject {
    uint8_t  reasonCode; // 0=full, 1=version mismatch, 2=banned, 3=other
    char     reasonText[128];
};

// ES: Aviso a todos de que ha entrado un jugador.
// EN: Notifies everyone that a player joined.
struct MsgPlayerJoined {
    PlayerID playerId;
    char     playerName[KMP_MAX_NAME_LENGTH + 1];
};

// ES: Aviso a todos de que un jugador se ha ido y por qué.
// EN: Notifies everyone that a player left and why.
struct MsgPlayerLeft {
    PlayerID playerId;
    uint8_t  reason; // 0=disconnect, 1=timeout, 2=kicked
};

// ES: ── Mensajes de movimiento ──
// EN: ── Movement Messages ──

// ES: Estado de posición de un personaje dentro de un paquete de posiciones:
//     posición, rotación comprimida, animación, velocidad y flags de movimiento.
// EN: Position state of one character inside a position packet:
//     position, compressed rotation, animation, speed and movement flags.
struct CharacterPosition {
    EntityID entityId;
    uint32_t generation;     // Phase 6: prevents ghost control when entity IDs are reused
    float    posX, posY, posZ;
    uint32_t compressedQuat; // Smallest-three encoded
    uint8_t  animStateId;
    uint8_t  moveSpeed;      // 0-255 mapped to 0.0-15.0 m/s
    uint16_t flags;          // Bit 0: running, Bit 1: sneaking, Bit 2: in combat
};

// ES: Posiciones enviadas por el cliente: contador seguido de N CharacterPosition.
// EN: Client-sent positions: count followed by N CharacterPosition.
struct MsgC2SPositionUpdate {
    uint8_t characterCount;
    // ES: Seguido de characterCount × CharacterPosition
    // EN: Followed by characterCount × CharacterPosition
};

// ES: Posiciones reenviadas por el servidor, indicando el jugador de origen.
// EN: Server-relayed positions, tagged with the source player.
struct MsgS2CPositionUpdate {
    PlayerID sourcePlayer;
    uint8_t  characterCount;
    // ES: Seguido de characterCount × CharacterPosition
    // EN: Followed by characterCount × CharacterPosition
};

// ES: Orden de movimiento hacia un punto de destino.
// EN: Move order toward a target point.
struct MsgMoveCommand {
    EntityID entityId;
    float    targetX, targetY, targetZ;
    uint8_t  moveType; // 0=walk, 1=run, 2=sneak
};

// ES: ── Mensajes de combate ──
// EN: ── Combat Messages ──

// ES: Intención de ataque del cliente (el servidor decide el resultado).
// EN: Client attack intent (the server decides the outcome).
struct MsgAttackIntent {
    EntityID attackerId;
    EntityID targetId;
    uint8_t  attackType; // 0=melee, 1=ranged
};

// ES: Resultado de un golpe: daño por tipo (corte/contundente/perforante), salud
//     resultante, si se bloqueó y si dejó KO.
// EN: Hit result: damage by type (cut/blunt/pierce), resulting health,
//     whether it was blocked and whether it caused a KO.
struct MsgCombatHit {
    EntityID attackerId;
    EntityID targetId;
    uint8_t  bodyPart;      // BodyPart enum
    float    cutDamage;
    float    bluntDamage;
    float    pierceDamage;
    float    resultHealth;
    uint8_t  wasBlocked;    // 0=hit, 1=partial block, 2=full block
    uint8_t  wasKO;
};

// ES: Bloqueo de un ataque y su efectividad.
// EN: Attack block and its effectiveness.
struct MsgCombatBlock {
    EntityID entityId;
    EntityID attackerId;
    uint8_t  bodyPart;
    float    blockEffectiveness; // 0.0-1.0
};

// ES: Personaje noqueado (KO).
// EN: Character knocked out (KO).
struct MsgCombatKO {
    EntityID entityId;
    EntityID attackerId;
    uint8_t  bodyPart;
    float    resultHealth;
};

// ES: Muerte de un personaje y quién lo mató.
// EN: Character death and who killed it.
struct MsgCombatDeath {
    EntityID entityId;
    EntityID killerId; // 0 if environmental
};

// ES: ── Mensajes de entidades ──
// EN: ── Entity Messages ──

// ES: Aparición de una entidad: tipo, dueño, plantilla del juego, posición, rotación y facción.
//     Ojo: el servidor (server.cpp) serializa S2C_EntitySpawn campo a campo SIN 'generation',
//     así que este struct no coincide con el formato real en la red.
// EN: Entity spawn: type, owner, game template, position, rotation and faction.
//     Note: the server (server.cpp) serializes S2C_EntitySpawn field by field WITHOUT
//     'generation', so this struct doesn't match the actual wire format.
struct MsgEntitySpawn {
    EntityID    entityId;
    uint32_t    generation;   // Phase 6: entity generation counter
    EntityType  type;
    PlayerID    ownerId;      // 0 = server-owned (NPC)
    uint32_t    templateId;   // Game data template reference
    float       posX, posY, posZ;
    uint32_t    compressedQuat;
    uint32_t    factionId;
    // ES: Siguen datos de longitud variable: nombre, equipo, stats
    // EN: Variable-length data follows: name string, equipment, stats
};

// ES: Desaparición de una entidad (con generación para no borrar una entidad reutilizada).
// EN: Entity despawn (with generation to avoid removing a reused entity).
struct MsgEntityDespawn {
    EntityID entityId;
    uint32_t generation; // Phase 6: verify we're despawning the right generation
    uint8_t  reason;     // 0=normal, 1=killed, 2=out of range
};

// ES: ── Mensajes de estadísticas ──
// EN: ── Stats Messages ──

// ES: Salud por parte del cuerpo y nivel de sangre.
// EN: Health per body part and blood level.
struct MsgHealthUpdate {
    EntityID entityId;
    float    health[static_cast<int>(BodyPart::Count)]; // Per body part
    float    bloodLevel;
};

// ES: Cambio de equipo en una ranura.
// EN: Equipment change in one slot.
struct MsgEquipmentUpdate {
    EntityID entityId;
    uint8_t  slot;       // EquipSlot enum
    uint32_t itemTemplateId; // 0 = empty
};

// ES: Valor de una estadística (parte entera = nivel, decimales = % de XP).
// EN: Stat value (integer part = level, decimals = XP %).
struct MsgStatUpdate {
    EntityID entityId;
    uint8_t  statIndex;
    float    statValue; // Whole = level, decimal = XP%
};

// ES: ── Mensajes de salud por extremidad ──
// EN: ── Limb Health Messages ──

// ES: Salud de las 7 partes del cuerpo (mismo orden que BodyPart).
// EN: Health of the 7 body parts (same order as BodyPart).
struct MsgLimbHealth {
    EntityID entityId;
    float    health[7]; // Per body part: Head, Chest, Stomach, LArm, RArm, LLeg, RLeg
};

// ES: ── Mensajes de efectos de estado ──
// EN: ── Status Effect Messages ──

// ES: Tipos de efecto de estado sincronizados.
// EN: Synced status effect types.
enum StatusEffectType : uint8_t {
    StatusEffect_None         = 0,
    StatusEffect_Bleeding     = 1,
    StatusEffect_Unconscious  = 2,
    StatusEffect_Crippled     = 3,
    StatusEffect_Bandaged     = 4,
};

// ES: Activa/desactiva un efecto de estado en una entidad.
// EN: Turns a status effect on/off for an entity.
struct MsgStatusEffect {
    EntityID entityId;
    uint8_t  effectType; // StatusEffectType
    uint8_t  active;     // 0=inactive, 1=active
};

// ES: ── Mensajes de edificios ──
// EN: ── Building Messages ──

// ES: Petición del cliente para colocar un edificio.
// EN: Client request to place a building.
struct MsgBuildRequest {
    uint32_t templateId;
    float    posX, posY, posZ;
    uint32_t compressedQuat;
};

// ES: Edificio colocado (confirmado por el servidor) con su ID de red y constructor.
// EN: Placed building (confirmed by the server) with its network ID and builder.
struct MsgBuildPlaced {
    EntityID entityId;
    uint32_t templateId;
    float    posX, posY, posZ;
    uint32_t compressedQuat;
    PlayerID builderId;
};

// ES: Progreso de construcción.
// EN: Construction progress.
struct MsgBuildProgress {
    EntityID entityId;
    float    progress; // 0.0 to 1.0
};

// ES: Estado de una puerta.
// EN: Door state.
struct MsgDoorState {
    EntityID entityId;
    uint8_t  state; // 0=closed, 1=open, 2=locked, 3=broken
};

// ES: ── Mensajes de inventario / comercio ──
// EN: ── Inventory / Trade Messages ──

// ES: Un personaje coge un objeto.
// EN: A character picks up an item.
struct MsgItemPickup {
    EntityID entityId;      // Character who picked up
    uint32_t itemTemplateId;
    int32_t  quantity;
};

// ES: Un personaje suelta un objeto en una posición.
// EN: A character drops an item at a position.
struct MsgItemDrop {
    EntityID entityId;      // Character who dropped
    uint32_t itemTemplateId;
    float    posX, posY, posZ; // Where the item was dropped
};

// ES: Petición de compra (precio total en "cats", la moneda de Kenshi).
// EN: Purchase request (total price in "cats", Kenshi's currency).
struct MsgTradeRequest {
    EntityID buyerEntityId;
    EntityID sellerEntityId; // 0 for NPC shop
    uint32_t itemTemplateId;
    int32_t  quantity;
    int32_t  price;          // Total cost in cats
};

// ES: Resultado de una compra.
// EN: Purchase result.
struct MsgTradeResult {
    EntityID buyerEntityId;
    uint32_t itemTemplateId;
    int32_t  quantity;
    uint8_t  success;        // 0=denied, 1=accepted
};

// ES: Cambio en el inventario de una entidad.
// EN: Change in an entity's inventory.
struct MsgInventoryUpdate {
    EntityID entityId;
    uint8_t  action;         // 0=add, 1=remove, 2=modify count
    uint32_t itemTemplateId;
    int32_t  quantity;
};

// ES: ── Mensajes de escuadras ──
// EN: ── Squad Messages ──

// ES: Creación de escuadra (le sigue el nombre como cadena).
// EN: Squad creation (followed by the name as a string).
struct MsgSquadCreate {
    EntityID creatorEntityId;
    uint32_t squadNetId;     // Server-assigned squad ID
    // ES: Seguido de una cadena: nombre de la escuadra
    // EN: Followed by string: squad name
};

// ES: Alta/baja de un miembro en una escuadra.
// EN: Member added to/removed from a squad.
struct MsgSquadMemberUpdate {
    uint32_t squadNetId;
    EntityID memberEntityId;
    uint8_t  action;         // 0=added, 1=removed
};

// ES: ── Mensajes de facciones ──
// EN: ── Faction Messages ──

// ES: Cambio de relación entre dos facciones.
// EN: Relation change between two factions.
struct MsgFactionRelation {
    uint32_t factionIdA;
    uint32_t factionIdB;
    float    relation;       // -100.0 to +100.0
    EntityID causerEntityId; // Who caused the change (0=system)
};

// ES: ── Postura de combate ──
//     Ojo: los valores de postura aquí (pasiva/defensiva/agresiva/mantener) no coinciden
//     con los de CombatInfo::stance en types.h (pasiva/cuerpo a cuerpo/distancia/bloqueo).
// EN: ── Combat Stance ──
//     Note: stance values here (passive/defensive/aggressive/hold) don't match
//     CombatInfo::stance in types.h (passive/melee/ranged/block).
struct MsgCombatStance {
    EntityID entityId;
    uint8_t  stance;    // 0=passive, 1=defensive, 2=aggressive, 3=hold
};

// ES: ── Transferencia de objetos entre personajes/contenedores ──
// EN: ── Item Transfer ──

struct MsgItemTransfer {
    EntityID sourceEntityId;  // Character/container transferring FROM
    EntityID destEntityId;    // Character/container transferring TO
    uint32_t itemTemplateId;
    int32_t  quantity;
};

// ES: ── Interacción con puertas (abrir/cerrar/bloquear/desbloquear) ──
// EN: ── Door Interaction ──

struct MsgDoorInteract {
    EntityID entityId;        // Building/gate entity
    EntityID actorEntityId;   // Character performing the action
    uint8_t  action;          // 0=open, 1=close, 2=lock, 3=unlock
};

// ES: ── Comando de administración (expulsar, banear, hora, clima, anuncio, velocidad) ──
// EN: ── Admin Command ──

struct MsgAdminCommand {
    uint8_t  commandType;     // 0=kick, 1=ban, 2=setTime, 3=setWeather, 4=announce, 5=setSpeed
    PlayerID targetPlayerId;  // For kick/ban
    float    floatParam;      // For setTime/setWeather
    char     textParam[128];  // For announce, kick reason
};

// ES: Respuesta del servidor a un comando de admin.
// EN: Server reply to an admin command.
struct MsgAdminResponse {
    uint8_t  success;         // 0=denied, 1=ok
    char     responseText[128];
};

// ES: Enviado por el servidor a todos los clientes cada vez que cambia el host.
//     Envío inicial: al asignar host en la primera conexión. Reasignación: cuando el host
//     actual se desconecta y otro jugador toma el relevo. Los clientes lo usan para
//     habilitar la UI de admin y comparar con su GetLocalPlayerId() para saber si son host.
// EN: Sent from server to all clients whenever the host identity changes.
//     Initial send: at first-connect host assignment. Reassign: when current host
//     disconnects and another player takes over. Clients use this to gate admin UI
//     and match against their own GetLocalPlayerId() to derive IsHost() state.
struct MsgHostAssignment {
    PlayerID newHostPlayerId;
};

// ES: ── Mensajes de sincronización de edificios ──
// EN: ── Building Sync Messages ──

// ES: Desmontaje de un edificio.
// EN: Building dismantled.
struct MsgBuildDismantle {
    EntityID buildingId;
    EntityID dismantlerId;   // Who dismantled it
};

// ES: Reparación de un edificio.
// EN: Building repaired.
struct MsgBuildRepair {
    EntityID buildingId;
    float    amount;         // Repair amount
};

// ES: ── Latido de entidades ──
//     El servidor lo envía periódicamente (cada 5 s) con la lista de IDs de entidad que
//     deberían existir en cada cliente. El cliente lo compara con su estado local y
//     limpia entidades huérfanas o pide las que faltan.
// EN: ── Entity Heartbeat ──
//     Server sends periodically (every 5s) with the list of all entity IDs
//     that should exist on each client. Client compares against local state
//     and cleans up orphaned entities or requests missing ones.

struct MsgEntityHeartbeat {
    uint32_t serverTick;
    uint16_t entityCount;
    // ES: Seguido de entityCount x EntityID (uint32_t cada uno)
    // EN: Followed by entityCount x EntityID (uint32_t each)
};

// ES: ── Sincronización de hora: tick, hora del día, clima y velocidad de juego ──
// EN: ── Time Sync ──

struct MsgTimeSync {
    uint32_t serverTick;
    float    timeOfDay;   // 0.0 to 1.0
    int32_t  weatherState;
    float    gameSpeed;   // 0.1 to 10.0
};

// ES: ── Chat: remitente seguido del texto como cadena ──
// EN: ── Chat ──

struct MsgChatMessage {
    PlayerID senderId; // 0 = system
    // ES: Seguido de una cadena: texto del mensaje
    // EN: Followed by string: message text
};

// ES: ── Consulta de servidor ──
//     Consulta ligera — el cliente envía C2S_ServerQuery y el servidor responde
//     con S2C_ServerInfo sin necesidad de handshake.
// EN: ── Server Query ──
//     Lightweight query — client sends C2S_ServerQuery, server responds
//     with S2C_ServerInfo without requiring a handshake.

struct MsgServerQuery {
    uint32_t protocolVersion;
};

// ES: Información pública del servidor para el navegador de servidores.
// EN: Public server info for the server browser.
struct MsgServerInfo {
    uint32_t protocolVersion;
    uint8_t  currentPlayers;
    uint8_t  maxPlayers;
    uint16_t port;
    float    timeOfDay;
    uint8_t  pvpEnabled;
    uint8_t  reserved[3];
    char     serverName[64];
};

// ES: ── Mensajes del master server ──
//     Registro del navegador de servidores: los servidores de juego se registran en el
//     master y los clientes le piden la lista de partidas disponibles.
// EN: ── Master Server Messages ──
//     Used for server browser registry. Game servers register with the master,
//     clients query for the list of available games.

// ES: Alta/actualización de un servidor de juego en el master.
// EN: Game server registration/update on the master.
struct MsgMasterRegister {
    uint32_t protocolVersion;
    uint16_t gamePort;           // Port the game server listens on
    uint8_t  currentPlayers;
    uint8_t  maxPlayers;
    float    timeOfDay;
    uint8_t  pvpEnabled;
    uint8_t  reserved[3];
    char     serverName[64];
    char     externalIP[46];     // IPv4/IPv6 string (filled by master if empty)
};

// ES: Latido periódico del servidor de juego al master.
// EN: Periodic game server heartbeat to the master.
struct MsgMasterHeartbeat {
    uint16_t gamePort;
    uint8_t  currentPlayers;
    uint8_t  maxPlayers;
    float    timeOfDay;
};

// ES: Una entrada de la lista de servidores que el master devuelve al cliente.
// EN: One entry of the server list the master returns to the client.
struct MsgMasterServerEntry {
    char     serverName[64];
    char     address[46];
    uint16_t port;
    uint8_t  currentPlayers;
    uint8_t  maxPlayers;
    uint8_t  pvpEnabled;
    uint8_t  reserved;
};

#pragma pack(pop)

} // namespace kmp
