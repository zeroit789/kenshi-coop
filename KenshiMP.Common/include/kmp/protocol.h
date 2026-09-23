// ES: Definición del protocolo de red de KenshiMP (solo cabecera): lista de tipos de
//     mensaje (con el canal ENet que usa cada grupo), cabecera fija de 8 bytes de cada
//     paquete y las clases PacketWriter/PacketReader para (de)serializar datos binarios.
//     Lo usan el plugin Core, el servidor, el master server y las herramientas de test.
// EN: KenshiMP network protocol definition (header-only): list of message types
//     (with the ENet channel each group uses), the fixed 8-byte header of every packet
//     and the PacketWriter/PacketReader classes to (de)serialize binary data.
//     Used by the Core plugin, the server, the master server and the test tools.
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

namespace kmp {

// ES: ── Tipos de mensaje ── (prefijo C2S = cliente→servidor, S2C = servidor→cliente,
//     MS = master server). El valor es el primer byte de cada paquete.
// EN: ── Message Types ── (prefix C2S = client→server, S2C = server→client,
//     MS = master server). The value is the first byte of every packet.
enum class MessageType : uint8_t {
    // ES: Conexión (canal 0 - fiable ordenado): handshake, entrada/salida, keepalive y "listo".
    // EN: Connection (Channel 0 - Reliable Ordered)
    C2S_Handshake         = 0x01,
    S2C_HandshakeAck      = 0x02,
    S2C_HandshakeReject   = 0x03,
    C2S_Disconnect        = 0x04,
    S2C_PlayerJoined      = 0x05,
    S2C_PlayerLeft        = 0x06,
    C2S_Keepalive         = 0x07,
    S2C_KeepaliveAck      = 0x08,
    C2S_PlayerReady       = 0x09,  // Client → Server: "I'm in-game and ready to spawn"
    S2C_AllPlayersReady   = 0x0A,  // Server → All: "All players ready, spawn now"

    // ES: Estado del mundo (canal 0): snapshot completo, sincronización de hora y datos de zona.
    // EN: World State (Channel 0)
    S2C_WorldSnapshot     = 0x10,
    S2C_TimeSync          = 0x11,
    S2C_ZoneData          = 0x12,

    // ES: Ciclo de vida de entidades (canal 0): aparición/desaparición y peticiones del cliente.
    // EN: Entity Lifecycle (Channel 0)
    S2C_EntitySpawn       = 0x20,
    S2C_EntityDespawn     = 0x21,
    C2S_EntitySpawnReq    = 0x22,
    C2S_EntityDespawnReq  = 0x23,

    // ES: Peticiones de estado del mundo (canal 0).
    // EN: World State requests (Channel 0)
    C2S_ZoneRequest       = 0x13,

    // ES: Movimiento (canal 2 - no fiable secuenciado): posiciones y órdenes de movimiento.
    // EN: Movement (Channel 2 - Unreliable Sequenced)
    C2S_PositionUpdate    = 0x30,
    S2C_PositionUpdate    = 0x31,
    C2S_MoveCommand       = 0x32,
    S2C_MoveCommand       = 0x33,

    // ES: Combate (canal 1 - fiable sin orden): ataques, golpes, bloqueos, muertes, KO y postura.
    // EN: Combat (Channel 1 - Reliable Unordered)
    C2S_AttackIntent      = 0x40,
    S2C_CombatHit         = 0x41,
    S2C_CombatBlock       = 0x42,
    S2C_CombatDeath       = 0x43,
    S2C_CombatKO          = 0x44,
    C2S_CombatStance      = 0x45,
    C2S_CombatDeath       = 0x46,
    C2S_CombatKO          = 0x47,

    // ES: Estadísticas (canal 1): stats, salud, equipo, salud por extremidad y efectos de estado.
    // EN: Stats (Channel 1)
    S2C_StatUpdate        = 0x50,
    S2C_HealthUpdate      = 0x51,
    S2C_EquipmentUpdate   = 0x52,
    C2S_EquipmentUpdate   = 0x53,
    C2S_LimbHealth        = 0x54,
    S2C_LimbHealth        = 0x55,
    C2S_StatusEffect      = 0x56,
    S2C_StatusEffect      = 0x57,

    // ES: Inventario (canal 1): coger/soltar/transferir objetos y comercio.
    // EN: Inventory (Channel 1)
    C2S_ItemPickup        = 0x60,
    C2S_ItemDrop          = 0x61,
    C2S_ItemTransfer      = 0x62,
    S2C_InventoryUpdate   = 0x63,
    C2S_TradeRequest      = 0x64,
    S2C_TradeResult       = 0x65,

    // ES: Edificios (canal 0): construir, progreso, destrucción, puertas, desmontar y reparar.
    // EN: Buildings (Channel 0)
    C2S_BuildRequest      = 0x70,
    S2C_BuildPlaced       = 0x71,
    S2C_BuildProgress     = 0x72,
    S2C_BuildDestroyed    = 0x73,
    C2S_DoorInteract      = 0x74,
    S2C_DoorState         = 0x75,
    C2S_BuildDismantle    = 0x76,
    C2S_BuildRepair       = 0x77,

    // ES: Escuadras (canal 0): crear escuadra y gestionar miembros.
    // EN: Squad (Channel 0)
    C2S_SquadCreate       = 0xB0,
    S2C_SquadCreated      = 0xB1,
    C2S_SquadAddMember    = 0xB2,
    S2C_SquadMemberUpdate = 0xB3,

    // ES: Facciones (canal 0): relaciones entre facciones.
    // EN: Faction (Channel 0)
    C2S_FactionRelation   = 0xC0,
    S2C_FactionRelation   = 0xC1,

    // ES: Chat (canal 0): mensajes de jugador y mensajes del sistema.
    // EN: Chat (Channel 0)
    C2S_ChatMessage       = 0x80,
    S2C_ChatMessage       = 0x81,
    S2C_SystemMessage     = 0x82,

    // ES: Administración (canal 0): comandos de admin y asignación del jugador anfitrión (host).
    // EN: Admin (Channel 0)
    C2S_AdminCommand      = 0x90,
    S2C_AdminResponse     = 0x91,
    S2C_HostAssignment    = 0x92,  // Server → all clients: host identity (sent on assign + reassign)

    // ES: Consulta ligera del servidor (no requiere handshake), para el navegador de servidores.
    // EN: Server query (lightweight — no handshake required)
    C2S_ServerQuery       = 0xA0,
    S2C_ServerInfo        = 0xA1,

    // ES: Master server (registro del navegador de servidores): alta, latido, baja y listado.
    // EN: Master server (server browser registry)
    MS_Register           = 0xD0,  // Game server → master: register/update
    MS_Heartbeat          = 0xD1,  // Game server → master: keepalive
    MS_Deregister         = 0xD2,  // Game server → master: shutting down
    MS_QueryList          = 0xD3,  // Client → master: request server list
    MS_ServerList         = 0xD4,  // Master → client: full server list

    // ES: Depuración del pipeline (canal 1): snapshots y eventos del pipeline de sync reenviados entre peers.
    // EN: Pipeline debug (Channel 1 - Reliable Unordered)
    C2S_PipelineSnapshot  = 0xE0,  // Client → server: periodic pipeline state snapshot
    S2C_PipelineSnapshot  = 0xE1,  // Server → client: forwarded snapshot from peer
    C2S_PipelineEvent     = 0xE2,  // Client → server: pipeline event batch
    S2C_PipelineEvent     = 0xE3,  // Server → client: forwarded events from peer

    // ES: ── Latido de entidades (canal 0): lista periódica de entidades presentes ──
    // EN: ── Entity Heartbeat (Channel 0) ──
    S2C_EntityHeartbeat   = 0x14,  // Server periodic entity presence list
    C2S_EntityAck         = 0x15,  // Client confirms receipt (optional)

    // ES: ── Lobby: asignación de facción y arranque sincronizado de la partida ──
    // EN: ── Lobby ──
    S2C_FactionAssignment = 0xF0,  // Server assigns faction string to client
    C2S_LobbyReady        = 0xF1,  // Client confirms ready with faction loaded
    S2C_LobbyStart        = 0xF2,  // Server tells all clients to start/load
};

// ES: ── Cabecera de paquete ── 8 bytes empaquetados (sin relleno) al inicio de cada paquete:
//     tipo de mensaje, flags (bit 0 = comprimido), número de secuencia y tick del servidor.
// EN: ── Packet Header ── 8 packed bytes (no padding) at the start of every packet:
//     message type, flags (bit 0 = compressed), sequence number and server tick.
#pragma pack(push, 1)
struct PacketHeader {
    MessageType type;
    uint8_t     flags;     // Bit 0: compressed
    uint16_t    sequence;
    uint32_t    timestamp; // Server tick
};
#pragma pack(pop)

// ES: Garantiza en compilación que la cabecera ocupa exactamente 8 bytes.
// EN: Compile-time guarantee that the header is exactly 8 bytes.
static_assert(sizeof(PacketHeader) == 8, "PacketHeader must be 8 bytes");

// ES: ── Búfer de serialización ── Escribe datos binarios (little-endian, copia directa de
//     memoria) en un vector que crece según se necesita.
// EN: ── Serialization Buffer ── Writes binary data (little-endian, raw memory copy)
//     into a vector that grows as needed.
class PacketWriter {
public:
    PacketWriter() { m_data.reserve(256); }

    // ES: Escribe la cabecera de 8 bytes con el tipo, secuencia y tick indicados (flags = 0).
    // EN: Writes the 8-byte header with the given type, sequence and tick (flags = 0).
    void WriteHeader(MessageType type, uint16_t seq = 0, uint32_t tick = 0) {
        PacketHeader h{};
        h.type = type;
        h.flags = 0;
        h.sequence = seq;
        h.timestamp = tick;
        WriteRaw(&h, sizeof(h));
    }

    // ES: Escritura de tipos primitivos de tamaño fijo.
    // EN: Fixed-size primitive writers.
    void WriteU8(uint8_t v)   { WriteRaw(&v, 1); }
    void WriteU16(uint16_t v) { WriteRaw(&v, 2); }
    void WriteU32(uint32_t v) { WriteRaw(&v, 4); }
    void WriteI32(int32_t v)  { WriteRaw(&v, 4); }
    void WriteF32(float v)    { WriteRaw(&v, 4); }

    // ES: Escribe un vector 3D como tres float.
    // EN: Writes a 3D vector as three floats.
    void WriteVec3(float x, float y, float z) {
        WriteF32(x); WriteF32(y); WriteF32(z);
    }

    // ES: Escribe una cadena con prefijo de longitud uint16 (sin terminador nulo).
    //     Cadenas de más de 65535 bytes se truncarían en la longitud.
    // EN: Writes a uint16 length-prefixed string (no null terminator).
    //     Strings over 65535 bytes would have their length truncated.
    void WriteString(const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size());
        WriteU16(len);
        if (len > 0) WriteRaw(s.data(), len);
    }

    // ES: Añade len bytes crudos al final del búfer.
    // EN: Appends len raw bytes to the end of the buffer.
    void WriteRaw(const void* data, size_t len) {
        size_t offset = m_data.size();
        m_data.resize(offset + len);
        std::memcpy(m_data.data() + offset, data, len);
    }

    // ES: Acceso al contenido serializado.
    // EN: Access to the serialized contents.
    const uint8_t* Data() const { return m_data.data(); }
    size_t Size() const { return m_data.size(); }
    std::vector<uint8_t>& Buffer() { return m_data; }

private:
    std::vector<uint8_t> m_data;
};

// ES: Lector de paquetes: recorre un búfer de solo lectura con comprobación de límites.
//     Todas las lecturas devuelven false (sin avanzar) si no quedan bytes suficientes.
//     No es dueño de la memoria: el búfer debe vivir mientras se use el lector.
// EN: Packet reader: walks a read-only buffer with bounds checking.
//     Every read returns false (without advancing) if not enough bytes remain.
//     It does not own the memory: the buffer must outlive the reader.
class PacketReader {
public:
    PacketReader(const uint8_t* data, size_t size)
        : m_data(data), m_size(size), m_pos(0) {}

    // ES: Lee la cabecera de 8 bytes.
    // EN: Reads the 8-byte header.
    bool ReadHeader(PacketHeader& h) {
        return ReadRaw(&h, sizeof(h));
    }

    // ES: Lectura de tipos primitivos de tamaño fijo.
    // EN: Fixed-size primitive readers.
    bool ReadU8(uint8_t& v)   { return ReadRaw(&v, 1); }
    bool ReadU16(uint16_t& v) { return ReadRaw(&v, 2); }
    bool ReadU32(uint32_t& v) { return ReadRaw(&v, 4); }
    bool ReadI32(int32_t& v)  { return ReadRaw(&v, 4); }
    bool ReadF32(float& v)    { return ReadRaw(&v, 4); }

    // ES: Lee un vector 3D (tres float).
    // EN: Reads a 3D vector (three floats).
    bool ReadVec3(float& x, float& y, float& z) {
        return ReadF32(x) && ReadF32(y) && ReadF32(z);
    }

    // ES: Lee una cadena con prefijo de longitud uint16. Rechaza cadenas mayores que maxLen
    //     o que se salgan del búfer. Ojo: si falla tras leer la longitud, el cursor ya avanzó 2 bytes.
    // EN: Reads a uint16 length-prefixed string. Rejects strings longer than maxLen or
    //     running past the buffer. Note: if it fails after reading the length, the cursor has already moved 2 bytes.
    bool ReadString(std::string& s, uint16_t maxLen = 1024) {
        uint16_t len;
        if (!ReadU16(len)) return false;
        if (len > maxLen) return false; // Reject oversized strings
        // ES: Comprobación sin overflow: m_pos <= m_size siempre (invariante de ReadRaw),
        //     así que m_size - m_pos nunca se desborda. "m_pos + len" sí podría desbordar
        //     si len fuera enorme; esta forma es defensa en profundidad.
        // EN: Overflow-free check: m_pos <= m_size always holds (ReadRaw invariant),
        //     so m_size - m_pos never wraps. "m_pos + len" could overflow if len were
        //     huge; this form is defense in depth.
        if (len > m_size - m_pos) return false;
        s.assign(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return true;
    }

    // ES: Copia len bytes crudos a out y avanza el cursor.
    // EN: Copies len raw bytes into out and advances the cursor.
    bool ReadRaw(void* out, size_t len) {
        // ES: Resta sin overflow (m_pos <= m_size siempre). Evita que un len gigante
        //     (p.ej. sizeof de un struct mal calculado) pase el bounds check por wraparound.
        // EN: Overflow-free subtraction (m_pos <= m_size always). Prevents a huge len
        //     (e.g. a miscomputed struct sizeof) from passing the bounds check via wraparound.
        if (len > m_size - m_pos) return false;
        std::memcpy(out, m_data + m_pos, len);
        m_pos += len;
        return true;
    }

    // ES: Bytes restantes, posición actual del cursor y puntero a los datos en esa posición.
    // EN: Remaining bytes, current cursor position and pointer to the data at that position.
    size_t Remaining() const { return m_size - m_pos; }
    size_t Position() const { return m_pos; }
    const uint8_t* Current() const { return m_data + m_pos; }

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos;
};

} // namespace kmp
