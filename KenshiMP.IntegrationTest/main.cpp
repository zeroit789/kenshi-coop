// ES: Test de integración de KenshiMP. Arranca KenshiMP.Server.exe como proceso aparte, conecta
//     clientes ENet falsos (sin Kenshi) a 127.0.0.1:27800 y comprueba el protocolo de punta a punta:
//     handshake, spawn de entidades, sincronía de posiciones, chat, limpieza al desconectar, TimeSync,
//     inventario, comercio, escuadras, relaciones de facción, edificios, consulta del navegador y una
//     sesión completa. Ejecutar desde la carpeta de salida del build (o la de Kenshi, donde está
//     KenshiMP.Server.exe) o pasar la ruta del servidor como argv[1]. Devuelve 1 si algún test falla.
// EN: KenshiMP Integration Test
// Starts a server, connects two fake clients, and verifies the full protocol pipeline.
// Tests: handshake, entity spawn, position sync, chat relay, disconnect cleanup.
// Run from the build output directory (or Kenshi dir where KenshiMP.Server.exe lives).

// ES: OJO: todos los clientes vienen de loopback, así que el servidor los trata como candidatos a
//     host integrado (sin rate limit y con prioridad de host).
// EN: NOTE: every client comes from loopback, so the server treats them as integrated-host
//     candidates (no rate limit and host priority).
#include <kmp/protocol.h>
#include <kmp/messages.h>
#include <kmp/constants.h>
#include <kmp/types.h>
#include <enet/enet.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <functional>
#include <atomic>

#ifdef _WIN32
#include <Windows.h>
#endif

using namespace kmp;

// ─────────────────────────────────────────────────
// ES: Mini framework de tests
// EN:  Test Framework
// ─────────────────────────────────────────────────

// ES: Contadores globales de aciertos/fallos. TestAssert imprime [PASS]/[FAIL] y suma al contador.
// EN: Global pass/fail counters. TestAssert prints [PASS]/[FAIL] and bumps the counter.
static int g_testsPassed = 0;
static int g_testsFailed = 0;

static void TestAssert(bool condition, const char* testName) {
    if (condition) {
        printf("  [PASS] %s\n", testName);
        g_testsPassed++;
    } else {
        printf("  [FAIL] %s\n", testName);
        g_testsFailed++;
    }
}

// ─────────────────────────────────────────────────
// ES: Cliente ENet de prueba
// EN:  Simple ENet Client Wrapper
// ─────────────────────────────────────────────────

// ES: Cliente falso: un host ENet con un peer hacia el servidor. Sabe enviar los mensajes C2S que se
//     prueban y apunta en vectores/contadores todo lo S2C que recibe para que los tests lo comprueben.
//     myEntityId = primera entidad propia confirmada por el servidor (S2C_EntitySpawn con owner = yo).
// EN: Fake client: an ENet host with one peer to the server. It can send the C2S messages under test
//     and records everything S2C it receives in vectors/counters so tests can check it.
//     myEntityId = first own entity confirmed by the server (S2C_EntitySpawn with owner = me).
struct TestClient {
    std::string name;
    ENetHost*   host = nullptr;
    ENetPeer*   peer = nullptr;
    PlayerID    playerId = 0;
    bool        connected = false;
    bool        handshakeOk = false;
    EntityID    myEntityId = 0;

    // ES: Registro de lo recibido del servidor.
    // EN: Received data tracking
    std::vector<MsgPlayerJoined>  playersJoined;
    std::vector<MsgPlayerLeft>    playersLeft;
    std::vector<uint32_t>         entitiesSpawned;   // entity IDs
    std::vector<uint32_t>         entitiesDespawned; // entity IDs
    int                           posUpdatesReceived = 0;
    std::vector<std::string>      chatMessages;
    std::vector<std::string>      systemMessages;
    int                           timeSyncsReceived = 0;
    MsgHandshakeAck               lastAck{};
    bool                          wasRejected = false;

    // ES: Registro de los sistemas nuevos (inventario, comercio, escuadras, facciones, edificios).
    // EN: New system tracking
    std::vector<MsgInventoryUpdate>   inventoryUpdates;
    std::vector<MsgTradeResult>       tradeResults;
    std::vector<uint32_t>             squadsCreated;     // squad net IDs
    std::vector<MsgSquadMemberUpdate> squadMemberUpdates;
    std::vector<MsgFactionRelation>   factionRelations;
    std::vector<MsgBuildPlaced>       buildingsPlaced;
    std::vector<uint32_t>             buildingsDestroyed;
    std::vector<MsgBuildProgress>     buildProgress;

    // ES: Crea el host ENet del cliente (1 peer, 3 canales) y guarda el nombre de jugador.
    // EN: Creates the client ENet host (1 peer, 3 channels) and stores the player name.
    bool Init(const std::string& playerName) {
        name = playerName;
        host = enet_host_create(nullptr, 1, KMP_CHANNEL_COUNT,
                                KMP_DOWNSTREAM_LIMIT, KMP_UPSTREAM_LIMIT);
        return host != nullptr;
    }

    // ES: Inicia la conexión ENet (asíncrona: 'connected' se pone al procesar el evento en Poll).
    // EN: Starts the ENet connection (async: 'connected' is set when Poll handles the event).
    bool Connect(const char* addr, uint16_t port) {
        ENetAddress enetAddr;
        enet_address_set_host(&enetAddr, addr);
        enetAddr.port = port;
        peer = enet_host_connect(host, &enetAddr, KMP_CHANNEL_COUNT, 0);
        return peer != nullptr;
    }

    // ES: Envío fiable por el canal 0 con flush inmediato.
    // EN: Reliable send on channel 0 with an immediate flush.
    void SendReliable(const uint8_t* data, size_t len) {
        ENetPacket* pkt = enet_packet_create(data, len, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);
        enet_host_flush(host); // Flush immediately so packet is sent even if we poll another host next
    }

    // ES: Envío no fiable por el canal 2 (el único que el servidor acepta para posiciones).
    // EN: Unreliable send on channel 2 (the only one the server accepts for positions).
    void SendUnreliable(const uint8_t* data, size_t len) {
        ENetPacket* pkt = enet_packet_create(data, len, ENET_PACKET_FLAG_UNSEQUENCED);
        enet_peer_send(peer, KMP_CHANNEL_UNRELIABLE_SEQ, pkt);
    }

    // ES: C2S_Handshake con la versión de protocolo actual, el nombre y la versión de juego 1.0.68.
    // EN: C2S_Handshake with the current protocol version, the name and game version 1.0.68.
    void SendHandshake() {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_Handshake);
        MsgHandshake hs{};
        hs.protocolVersion = KMP_PROTOCOL_VERSION;
        strncpy(hs.playerName, name.c_str(), KMP_MAX_NAME_LENGTH);
        hs.playerName[KMP_MAX_NAME_LENGTH] = '\0';
        hs.gameVersionMajor = 1;
        hs.gameVersionMinor = 0;
        hs.gameVersionPatch = 68;
        w.WriteRaw(&hs, sizeof(hs));
        SendReliable(w.Data(), w.Size());
    }

    // ES: C2S_EntitySpawnReq de un PlayerCharacter "Greenlander" en (x, y, z), sin estado extendido.
    // EN: C2S_EntitySpawnReq for a "Greenlander" PlayerCharacter at (x, y, z), without extended state.
    void SendEntitySpawn(float x, float y, float z) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_EntitySpawnReq);
        w.WriteU32(1);  // client entity ID
        w.WriteU8(static_cast<uint8_t>(EntityType::PlayerCharacter));
        w.WriteU32(playerId);
        w.WriteU32(0);  // template ID
        w.WriteF32(x);
        w.WriteF32(y);
        w.WriteF32(z);
        w.WriteU32(Quat().Compress());  // identity rotation
        w.WriteU32(0);  // faction
        std::string tmpl = "Greenlander";
        w.WriteU16(static_cast<uint16_t>(tmpl.size()));
        w.WriteRaw(tmpl.data(), tmpl.size());
        SendReliable(w.Data(), w.Size());
    }

    // ES: C2S_PositionUpdate con una sola entidad (rotación identidad, animación 1, velocidad 85).
    // EN: C2S_PositionUpdate with a single entity (identity rotation, animation 1, speed 85).
    void SendPositionUpdate(EntityID entityId, float x, float y, float z) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_PositionUpdate);
        w.WriteU8(1);  // 1 character

        CharacterPosition cp{};
        cp.entityId = entityId;
        cp.posX = x;
        cp.posY = y;
        cp.posZ = z;
        cp.compressedQuat = Quat().Compress();
        cp.animStateId = 1;
        cp.moveSpeed = 85;
        cp.flags = 0x01;
        w.WriteRaw(&cp, sizeof(cp));

        SendUnreliable(w.Data(), w.Size());
    }

    // ES: Envíos del resto de mensajes C2S que se prueban: chat, recoger/soltar objeto, comercio,
    //     crear escuadra, relación de facción, construir y desmontar edificio.
    // EN: Senders for the other C2S messages under test: chat, item pickup/drop, trade, squad
    //     creation, faction relation, building placement and dismantling.
    void SendChat(const std::string& msg) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_ChatMessage);
        w.WriteU32(playerId);
        w.WriteString(msg);
        SendReliable(w.Data(), w.Size());
    }

    void SendItemPickup(EntityID entityId, uint32_t itemId, int32_t qty) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_ItemPickup);
        MsgItemPickup msg{};
        msg.entityId = entityId;
        msg.itemTemplateId = itemId;
        msg.quantity = qty;
        w.WriteRaw(&msg, sizeof(msg));
        SendReliable(w.Data(), w.Size());
    }

    void SendItemDrop(EntityID entityId, uint32_t itemId, float x, float y, float z) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_ItemDrop);
        MsgItemDrop msg{};
        msg.entityId = entityId;
        msg.itemTemplateId = itemId;
        msg.posX = x; msg.posY = y; msg.posZ = z;
        w.WriteRaw(&msg, sizeof(msg));
        SendReliable(w.Data(), w.Size());
    }

    void SendTradeRequest(EntityID buyerId, uint32_t itemId, int32_t qty, int32_t price) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_TradeRequest);
        MsgTradeRequest msg{};
        msg.buyerEntityId = buyerId;
        msg.sellerEntityId = 0;
        msg.itemTemplateId = itemId;
        msg.quantity = qty;
        msg.price = price;
        w.WriteRaw(&msg, sizeof(msg));
        SendReliable(w.Data(), w.Size());
    }

    void SendSquadCreate(EntityID creatorId, const std::string& squadName) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_SquadCreate);
        w.WriteU32(creatorId);
        w.WriteString(squadName);
        SendReliable(w.Data(), w.Size());
    }

    void SendFactionRelation(uint32_t factionA, uint32_t factionB, float relation) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_FactionRelation);
        MsgFactionRelation msg{};
        msg.factionIdA = factionA;
        msg.factionIdB = factionB;
        msg.relation = relation;
        msg.causerEntityId = 0;
        w.WriteRaw(&msg, sizeof(msg));
        SendReliable(w.Data(), w.Size());
    }

    void SendBuildRequest(uint32_t templateId, float x, float y, float z) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_BuildRequest);
        MsgBuildRequest msg{};
        msg.templateId = templateId;
        msg.posX = x; msg.posY = y; msg.posZ = z;
        msg.compressedQuat = Quat().Compress();
        w.WriteRaw(&msg, sizeof(msg));
        SendReliable(w.Data(), w.Size());
    }

    void SendBuildDismantle(EntityID buildingId) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_BuildDismantle);
        MsgBuildDismantle msg{};
        msg.buildingId = buildingId;
        msg.dismantlerId = myEntityId;
        w.WriteRaw(&msg, sizeof(msg));
        SendReliable(w.Data(), w.Size());
    }

    // ES: Decodifica un paquete S2C y lo apunta en el registro correspondiente. Los tipos que no se
    //     prueban se ignoran. OJO: en S2C_EntitySpawn solo se leen los campos base + nombre de plantilla
    //     (el estado extendido que añade el servidor se ignora).
    // EN: Decodes an S2C packet and records it in the matching field. Types not under test are ignored.
    //     NOTE: for S2C_EntitySpawn only the base fields + template name are read (the extended state
    //     the server appends is ignored).
    void HandlePacket(const uint8_t* data, size_t len) {
        if (len < sizeof(PacketHeader)) return;

        PacketReader r(data, len);
        PacketHeader hdr;
        r.ReadHeader(hdr);

        switch (hdr.type) {
        case MessageType::S2C_HandshakeAck: {
            MsgHandshakeAck ack;
            if (r.ReadRaw(&ack, sizeof(ack))) {
                playerId = ack.playerId;
                handshakeOk = true;
                lastAck = ack;
            }
            break;
        }
        case MessageType::S2C_HandshakeReject: {
            wasRejected = true;
            break;
        }
        case MessageType::S2C_PlayerJoined: {
            MsgPlayerJoined pj;
            if (r.ReadRaw(&pj, sizeof(pj))) {
                playersJoined.push_back(pj);
            }
            break;
        }
        case MessageType::S2C_PlayerLeft: {
            MsgPlayerLeft pl;
            if (r.ReadRaw(&pl, sizeof(pl))) {
                playersLeft.push_back(pl);
            }
            break;
        }
        case MessageType::S2C_EntitySpawn: {
            uint32_t entId, ownerId, templateId, compQuat, factionId;
            uint8_t type;
            float px, py, pz;
            r.ReadU32(entId);
            r.ReadU8(type);
            r.ReadU32(ownerId);
            r.ReadU32(templateId);
            r.ReadF32(px); r.ReadF32(py); r.ReadF32(pz);
            r.ReadU32(compQuat);
            r.ReadU32(factionId);
            std::string tmplName;
            r.ReadString(tmplName);

            entitiesSpawned.push_back(entId);

            // ES: Apunta nuestra primera entidad propia.
            // EN: Track our own entity
            if (ownerId == playerId && myEntityId == 0) {
                myEntityId = entId;
            }
            break;
        }
        case MessageType::S2C_EntityDespawn: {
            MsgEntityDespawn ds;
            if (r.ReadRaw(&ds, sizeof(ds))) {
                entitiesDespawned.push_back(ds.entityId);
            }
            break;
        }
        case MessageType::S2C_PositionUpdate: {
            posUpdatesReceived++;
            break;
        }
        case MessageType::S2C_ChatMessage: {
            uint32_t senderId;
            r.ReadU32(senderId);
            std::string msg;
            r.ReadString(msg);
            chatMessages.push_back(msg);
            break;
        }
        case MessageType::S2C_SystemMessage: {
            uint32_t senderId;
            r.ReadU32(senderId);
            std::string msg;
            r.ReadString(msg);
            systemMessages.push_back(msg);
            break;
        }
        case MessageType::S2C_TimeSync: {
            timeSyncsReceived++;
            break;
        }
        case MessageType::S2C_InventoryUpdate: {
            MsgInventoryUpdate inv;
            if (r.ReadRaw(&inv, sizeof(inv))) {
                inventoryUpdates.push_back(inv);
            }
            break;
        }
        case MessageType::S2C_TradeResult: {
            MsgTradeResult tr;
            if (r.ReadRaw(&tr, sizeof(tr))) {
                tradeResults.push_back(tr);
            }
            break;
        }
        case MessageType::S2C_SquadCreated: {
            uint32_t creator, squadId;
            r.ReadU32(creator);
            r.ReadU32(squadId);
            std::string squadName;
            r.ReadString(squadName);
            squadsCreated.push_back(squadId);
            break;
        }
        case MessageType::S2C_SquadMemberUpdate: {
            MsgSquadMemberUpdate smu;
            if (r.ReadRaw(&smu, sizeof(smu))) {
                squadMemberUpdates.push_back(smu);
            }
            break;
        }
        case MessageType::S2C_FactionRelation: {
            MsgFactionRelation fr;
            if (r.ReadRaw(&fr, sizeof(fr))) {
                factionRelations.push_back(fr);
            }
            break;
        }
        case MessageType::S2C_BuildPlaced: {
            MsgBuildPlaced bp;
            if (r.ReadRaw(&bp, sizeof(bp))) {
                buildingsPlaced.push_back(bp);
            }
            break;
        }
        case MessageType::S2C_BuildDestroyed: {
            uint32_t buildingId;
            uint8_t reason;
            r.ReadU32(buildingId);
            r.ReadU8(reason);
            buildingsDestroyed.push_back(buildingId);
            break;
        }
        case MessageType::S2C_BuildProgress: {
            MsgBuildProgress prog;
            if (r.ReadRaw(&prog, sizeof(prog))) {
                buildProgress.push_back(prog);
            }
            break;
        }
        default:
            break;
        }
    }

    // ES: Procesa eventos ENet durante timeoutMs milisegundos. Devuelve cuántos eventos procesó.
    // EN: Poll ENet events for up to timeoutMs milliseconds.
    // Returns number of events processed.
    int Poll(int timeoutMs = 100) {
        int count = 0;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);

        while (std::chrono::steady_clock::now() < deadline) {
            ENetEvent event;
            int result = enet_host_service(host, &event, 5);
            if (result > 0) {
                count++;
                switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    connected = true;
                    break;
                case ENET_EVENT_TYPE_RECEIVE:
                    HandlePacket(event.packet->data, event.packet->dataLength);
                    enet_packet_destroy(event.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    connected = false;
                    break;
                default:
                    break;
                }
            }
        }
        return count;
    }

    // ES: Procesa eventos hasta que se cumpla la condición o se agote el tiempo; devuelve si se cumplió.
    // EN: Poll until a condition is met or timeout
    bool PollUntil(std::function<bool()> condition, int timeoutMs = 3000) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            Poll(50);
            if (condition()) return true;
        }
        return false;
    }

    // ES: Desconexión limpia; si en 500 ms no se completa, se fuerza con enet_peer_reset.
    // EN: Graceful disconnect; if it does not complete within 500 ms it is forced with enet_peer_reset.
    void Disconnect() {
        if (peer) {
            enet_peer_disconnect(peer, 0);
            // ES: Procesa eventos un momento para que la desconexión llegue al servidor.
            // EN: Drain events briefly to let disconnect propagate
            ENetEvent event;
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(500);
            while (std::chrono::steady_clock::now() < deadline) {
                if (enet_host_service(host, &event, 50) > 0) {
                    if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                        HandlePacket(event.packet->data, event.packet->dataLength);
                        enet_packet_destroy(event.packet);
                    } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                        connected = false;
                        break;
                    }
                }
            }
            // ES: Si la desconexión limpia no terminó, se fuerza para que el servidor vea marcharse al peer (evita huecos zombis).
            // EN: If graceful disconnect didn't complete, force it so the server
            // sees the peer go away immediately (prevents zombie peer slots)
            if (connected && peer) {
                enet_peer_reset(peer);
                connected = false;
            }
            peer = nullptr;
        }
    }

    // ES: Destruye el host ENet del cliente.
    // EN: Destroys the client ENet host.
    void Destroy() {
        if (host) {
            enet_host_destroy(host);
            host = nullptr;
        }
    }
};

// ─────────────────────────────────────────────────
// ES: Gestión del proceso del servidor
// EN:  Server Process Management
// ─────────────────────────────────────────────────

#ifdef _WIN32
static PROCESS_INFORMATION g_serverProcess{};

// ES: Lanza KenshiMP.Server.exe en una consola nueva con su carpeta como directorio de trabajo
//     (el servidor busca server.json en el cwd). Guarda el PROCESS_INFORMATION en g_serverProcess.
// EN: Launches KenshiMP.Server.exe in a new console with its folder as working directory (the
//     server looks for server.json in its cwd). Stores the PROCESS_INFORMATION in g_serverProcess.
static bool StartServer(const char* exePath) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);

    // ES: Línea de comandos: la ruta del exe entre comillas.
    // EN: Build command line
    char cmdLine[512];
    sprintf_s(cmdLine, "\"%s\"", exePath);

    // ES: El directorio de trabajo es la carpeta del exe (el servidor necesita server.json en su cwd).
    // EN: Derive working directory from exe path (server needs server.json in its cwd)
    std::string exeStr(exePath);
    std::string workDir;
    auto lastSlash = exeStr.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        workDir = exeStr.substr(0, lastSlash);
    }
    const char* workDirPtr = workDir.empty() ? nullptr : workDir.c_str();

    printf("[Server] Working directory: %s\n", workDirPtr ? workDirPtr : "(current)");

    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE, nullptr, workDirPtr,
                        &si, &g_serverProcess)) {
        printf("ERROR: Failed to start server (error %lu)\n", GetLastError());
        printf("  Tried: %s\n", exePath);
        return false;
    }
    printf("[Server] Started (PID %lu)\n", g_serverProcess.dwProcessId);
    return true;
}

// ES: Mata el proceso del servidor (TerminateProcess: no guarda el mundo) y cierra sus handles.
// EN: Kills the server process (TerminateProcess: the world is not saved) and closes its handles.
static void StopServer() {
    if (g_serverProcess.hProcess) {
        TerminateProcess(g_serverProcess.hProcess, 0);
        WaitForSingleObject(g_serverProcess.hProcess, 2000);
        CloseHandle(g_serverProcess.hProcess);
        CloseHandle(g_serverProcess.hThread);
        g_serverProcess = {};
        printf("[Server] Stopped\n");
    }
}
#endif

// ─────────────────────────────────────────────────
// ES: Localizar el ejecutable del servidor
// EN:  Find Server Executable
// ─────────────────────────────────────────────────

// ES: Busca KenshiMP.Server.exe en: carpeta actual, padre, abuelo y la ruta por defecto de Kenshi
//     en Steam. Devuelve "" si no lo encuentra.
// EN: Looks for KenshiMP.Server.exe in: current dir, parent, grandparent and the default Steam
//     Kenshi path. Returns "" if not found.
static std::string FindServerExe() {
    // ES: Prueba varias ubicaciones relativas.
    // EN: Try several locations
    const char* candidates[] = {
        // Same directory as this test exe
        "KenshiMP.Server.exe",
        // Build output
        "../KenshiMP.Server.exe",
        // Kenshi directory (post-build copy)
        "../../KenshiMP.Server.exe",
    };

    for (auto& path : candidates) {
        DWORD attrs = GetFileAttributesA(path);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }

    // ES: Prueba la ruta absoluta de Kenshi en Steam.
    // EN: Try absolute Kenshi path
    const char* kenshiPath = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Kenshi\\KenshiMP.Server.exe";
    if (GetFileAttributesA(kenshiPath) != INVALID_FILE_ATTRIBUTES) {
        return kenshiPath;
    }

    return "";
}

// ─────────────────────────────────────────────────
// ES: Baterías de tests
// EN:  Test Suites
// ─────────────────────────────────────────────────

// ES: Test 1 - Conexión: un cliente conecta, hace handshake y recibe un PlayerID válido y maxPlayers > 0.
// EN: Test 1 - Connection: one client connects, handshakes and gets a valid PlayerID and maxPlayers > 0.
static void Test_ServerConnection() {
    printf("\n=== Test: Server Connection ===\n");

    TestClient client;
    TestAssert(client.Init("TestPlayer1"), "Client init");

    TestAssert(client.Connect("127.0.0.1", KMP_DEFAULT_PORT), "Client connect call");

    // Wait for ENet connection
    bool enetConnected = client.PollUntil([&]() { return client.connected; }, 3000);
    TestAssert(enetConnected, "ENet connection established");

    if (!enetConnected) {
        client.Disconnect();
        client.Destroy();
        return;
    }

    // Send handshake
    client.SendHandshake();

    // Wait for handshake ack
    bool gotAck = client.PollUntil([&]() { return client.handshakeOk; }, 3000);
    TestAssert(gotAck, "Handshake acknowledged");

    if (gotAck) {
        TestAssert(client.playerId > 0, "Received valid player ID");
        TestAssert(client.lastAck.maxPlayers > 0, "Server reports max players > 0");
        printf("    Player ID: %u, Players: %u/%u\n",
               client.playerId, client.lastAck.currentPlayers, client.lastAck.maxPlayers);
    }

    client.Disconnect();
    client.Destroy();
}

// ES: Test 2 - Dos jugadores: Alice y Bob hacen handshake; Alice recibe PlayerJoined con el nombre
//     "Bob" y ambos tienen PlayerID distintos.
// EN: Test 2 - Two players: Alice and Bob handshake; Alice gets PlayerJoined with name "Bob" and
//     both have different PlayerIDs.
static void Test_TwoPlayersConnect() {
    printf("\n=== Test: Two Players Connect ===\n");

    TestClient client1, client2;
    client1.Init("Alice");
    client2.Init("Bob");

    // Connect both
    client1.Connect("127.0.0.1", KMP_DEFAULT_PORT);
    client2.Connect("127.0.0.1", KMP_DEFAULT_PORT);

    // Wait for ENet connections
    client1.PollUntil([&]() { return client1.connected; }, 3000);
    client2.PollUntil([&]() { return client2.connected; }, 3000);
    TestAssert(client1.connected && client2.connected, "Both clients connected");

    if (!client1.connected || !client2.connected) {
        client1.Disconnect(); client2.Disconnect();
        client1.Destroy(); client2.Destroy();
        return;
    }

    // Client 1 handshake
    client1.SendHandshake();
    client1.PollUntil([&]() { return client1.handshakeOk; }, 3000);
    TestAssert(client1.handshakeOk, "Client 1 handshake OK");

    // Small delay so server processes client 1 fully
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Client 2 handshake
    client2.SendHandshake();
    client2.PollUntil([&]() { return client2.handshakeOk; }, 3000);
    TestAssert(client2.handshakeOk, "Client 2 handshake OK");

    // Client 1 should receive "player joined" for client 2
    bool c1SawJoin = client1.PollUntil([&]() {
        return !client1.playersJoined.empty();
    }, 3000);
    TestAssert(c1SawJoin, "Client 1 received PlayerJoined for Client 2");

    if (c1SawJoin) {
        bool nameMatch = (strncmp(client1.playersJoined.back().playerName, "Bob",
                                  KMP_MAX_NAME_LENGTH) == 0);
        TestAssert(nameMatch, "PlayerJoined name matches 'Bob'");
        printf("    Client 1 saw join: '%s' (ID: %u)\n",
               client1.playersJoined.back().playerName,
               client1.playersJoined.back().playerId);
    }

    // Verify different player IDs
    TestAssert(client1.playerId != client2.playerId,
               "Clients have different player IDs");
    printf("    Alice ID: %u, Bob ID: %u\n", client1.playerId, client2.playerId);

    client1.Disconnect();
    client2.Disconnect();
    client1.Destroy();
    client2.Destroy();
}

// ES: Test 3 - Spawn de entidades: Alice pide un spawn y recibe su propio S2C_EntitySpawn (ID del
//     servidor); Bob recibe el mismo ID. Luego al revés.
// EN: Test 3 - Entity spawn: Alice requests a spawn and gets her own S2C_EntitySpawn (server ID);
//     Bob receives the same ID. Then the other way round.
static void Test_EntitySpawnAndBroadcast() {
    printf("\n=== Test: Entity Spawn & Broadcast ===\n");

    TestClient client1, client2;
    client1.Init("Alice");
    client2.Init("Bob");

    client1.Connect("127.0.0.1", KMP_DEFAULT_PORT);
    client2.Connect("127.0.0.1", KMP_DEFAULT_PORT);

    client1.PollUntil([&]() { return client1.connected; }, 3000);
    client2.PollUntil([&]() { return client2.connected; }, 3000);

    client1.SendHandshake();
    client1.PollUntil([&]() { return client1.handshakeOk; }, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client2.SendHandshake();
    client2.PollUntil([&]() { return client2.handshakeOk; }, 3000);
    // Drain join notifications
    client1.Poll(200);
    client2.Poll(200);

    // Client 1 spawns an entity
    float spawnX = -51200.f, spawnY = 1600.f, spawnZ = 2700.f;
    client1.SendEntitySpawn(spawnX, spawnY, spawnZ);

    // Client 1 should get confirmation (S2C_EntitySpawn for own entity)
    bool c1GotEntity = client1.PollUntil([&]() {
        return client1.myEntityId != 0;
    }, 3000);
    TestAssert(c1GotEntity, "Client 1 received own entity spawn confirmation");
    if (c1GotEntity) {
        printf("    Client 1 entity ID: %u\n", client1.myEntityId);
    }

    // Client 2 should also see the entity spawn broadcast
    size_t c2SpawnsBefore = client2.entitiesSpawned.size();
    bool c2SawSpawn = client2.PollUntil([&]() {
        return client2.entitiesSpawned.size() > c2SpawnsBefore;
    }, 3000);
    TestAssert(c2SawSpawn, "Client 2 received entity spawn broadcast");

    if (c2SawSpawn) {
        // Check that the entity ID matches
        bool foundEntity = false;
        for (auto id : client2.entitiesSpawned) {
            if (id == client1.myEntityId) {
                foundEntity = true;
                break;
            }
        }
        TestAssert(foundEntity, "Client 2 sees Client 1's entity ID");
    }

    // Now Client 2 spawns
    client2.SendEntitySpawn(spawnX + 20.f, spawnY, spawnZ + 20.f);
    bool c2GotEntity = client2.PollUntil([&]() {
        return client2.myEntityId != 0;
    }, 3000);
    TestAssert(c2GotEntity, "Client 2 received own entity spawn confirmation");

    // Client 1 should see Client 2's entity
    size_t c1SpawnsBefore = client1.entitiesSpawned.size();
    bool c1SawC2Spawn = client1.PollUntil([&]() {
        return client1.entitiesSpawned.size() > c1SpawnsBefore;
    }, 3000);
    TestAssert(c1SawC2Spawn, "Client 1 received Client 2's entity spawn");

    client1.Disconnect();
    client2.Disconnect();
    client1.Destroy();
    client2.Destroy();
}

// ES: Test 4 - Posiciones: cada cliente envía 5 actualizaciones y se comprueba que el otro recibe
//     paquetes S2C_PositionUpdate. OJO: solo cuenta paquetes, no mira su contenido; como el servidor
//     envía cada tick las posiciones de todas las entidades ajenas, pasaría aunque rechazara las
//     actualizaciones (ver la nota de 'authority' en server.h).
// EN: Test 4 - Positions: each client sends 5 updates and the other must receive S2C_PositionUpdate
//     packets. NOTE: it only counts packets, not their contents; since the server sends every tick
//     the positions of all non-owned entities, it would pass even if updates were rejected (see the
//     'authority' note in server.h).
static void Test_PositionSync() {
    printf("\n=== Test: Position Sync ===\n");

    TestClient client1, client2;
    client1.Init("Alice");
    client2.Init("Bob");

    client1.Connect("127.0.0.1", KMP_DEFAULT_PORT);
    client2.Connect("127.0.0.1", KMP_DEFAULT_PORT);

    client1.PollUntil([&]() { return client1.connected; }, 3000);
    client2.PollUntil([&]() { return client2.connected; }, 3000);

    client1.SendHandshake();
    client1.PollUntil([&]() { return client1.handshakeOk; }, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client2.SendHandshake();
    client2.PollUntil([&]() { return client2.handshakeOk; }, 3000);

    // Drain join notifications
    client1.Poll(200);
    client2.Poll(200);

    // Both spawn entities
    client1.SendEntitySpawn(-51200.f, 1600.f, 2700.f);
    client1.PollUntil([&]() { return client1.myEntityId != 0; }, 3000);

    client2.SendEntitySpawn(-51180.f, 1600.f, 2720.f);
    client2.PollUntil([&]() { return client2.myEntityId != 0; }, 3000);

    // Drain spawn broadcasts
    client1.Poll(300);
    client2.Poll(300);

    TestAssert(client1.myEntityId != 0 && client2.myEntityId != 0,
               "Both clients have entities");

    // ES: Alice envía varias posiciones.
    // EN: Client 1 sends several position updates
    int c2PosBefore = client2.posUpdatesReceived;
    for (int i = 0; i < 5; i++) {
        float x = -51200.f + static_cast<float>(i) * 10.f;
        client1.SendPositionUpdate(client1.myEntityId, x, 1600.f, 2700.f);
        // Small delay to let server forward
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        client2.Poll(50);
    }

    // Give extra time for server forwarding
    client2.Poll(500);

    bool gotPosUpdates = (client2.posUpdatesReceived > c2PosBefore);
    TestAssert(gotPosUpdates, "Client 2 received position updates from Client 1");
    printf("    Position updates received by Client 2: %d (was %d)\n",
           client2.posUpdatesReceived, c2PosBefore);

    // ES: Bob envía posiciones en sentido contrario.
    // EN: Client 2 sends position updates back
    int c1PosBefore = client1.posUpdatesReceived;
    for (int i = 0; i < 5; i++) {
        float z = 2720.f + static_cast<float>(i) * 10.f;
        client2.SendPositionUpdate(client2.myEntityId, -51180.f, 1600.f, z);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        client1.Poll(50);
    }
    client1.Poll(500);

    bool gotReverse = (client1.posUpdatesReceived > c1PosBefore);
    TestAssert(gotReverse, "Client 1 received position updates from Client 2");
    printf("    Position updates received by Client 1: %d (was %d)\n",
           client1.posUpdatesReceived, c1PosBefore);

    client1.Disconnect();
    client2.Disconnect();
    client1.Destroy();
    client2.Destroy();
}

// ES: Test 5 - Chat: Alice escribe y Bob recibe el texto exacto; luego al revés.
// EN: Test 5 - Chat: Alice writes and Bob gets the exact text; then the other way round.
static void Test_ChatRelay() {
    printf("\n=== Test: Chat Relay ===\n");

    TestClient client1, client2;
    client1.Init("Alice");
    client2.Init("Bob");

    client1.Connect("127.0.0.1", KMP_DEFAULT_PORT);
    client2.Connect("127.0.0.1", KMP_DEFAULT_PORT);

    client1.PollUntil([&]() { return client1.connected; }, 3000);
    client2.PollUntil([&]() { return client2.connected; }, 3000);

    client1.SendHandshake();
    client1.PollUntil([&]() { return client1.handshakeOk; }, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client2.SendHandshake();
    client2.PollUntil([&]() { return client2.handshakeOk; }, 3000);

    // Drain notifications
    client1.Poll(200);
    client2.Poll(200);

    // Drain any remaining notifications (join messages, system messages, etc.)
    client1.Poll(300);
    client2.Poll(300);

    // Clear chat state for clean test
    client1.chatMessages.clear();
    client2.chatMessages.clear();

    // Client 1 sends a chat message
    client1.SendChat("Hello from Alice!");

    // ES: Se procesan AMBOS clientes (el servidor podría mandarlo también al emisor).
    // EN: Poll BOTH clients — server may broadcast to sender too
    bool c2GotChat = false;
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (std::chrono::steady_clock::now() < deadline) {
            client1.Poll(25);
            client2.Poll(25);
            if (!client2.chatMessages.empty()) { c2GotChat = true; break; }
        }
    }
    TestAssert(c2GotChat, "Client 2 received chat from Client 1");

    if (c2GotChat) {
        bool msgMatch = false;
        for (auto& m : client2.chatMessages) {
            if (m.find("Hello from Alice!") != std::string::npos) {
                msgMatch = true;
                break;
            }
        }
        TestAssert(msgMatch, "Chat message content matches");
    }

    // Clear and test reverse direction
    size_t c1ChatBefore = client1.chatMessages.size();
    client2.SendChat("Hi Alice, Bob here!");

    bool c1GotChat = false;
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (std::chrono::steady_clock::now() < deadline) {
            client1.Poll(25);
            client2.Poll(25);
            if (client1.chatMessages.size() > c1ChatBefore) { c1GotChat = true; break; }
        }
    }
    TestAssert(c1GotChat, "Client 1 received chat from Client 2");

    if (c1GotChat) {
        bool msgMatch = false;
        for (auto& m : client1.chatMessages) {
            if (m.find("Hi Alice, Bob here!") != std::string::npos) {
                msgMatch = true;
                break;
            }
        }
        TestAssert(msgMatch, "Reply chat message content matches");
    }

    client1.Disconnect();
    client2.Disconnect();
    client1.Destroy();
    client2.Destroy();
}

// ES: Test 6 - Desconexión: Bob crea una entidad y se desconecta; Alice debe recibir S2C_PlayerLeft y
//     S2C_EntityDespawn de la entidad de Bob. OJO: el servidor actual CONSERVA las entidades del que se
//     va (owner = 0, para que pueda reconectar) y no envía despawn, así que esa comprobación
//     probablemente falla (sin verificar en ejecución).
// EN: Test 6 - Disconnect: Bob creates an entity and disconnects; Alice must receive S2C_PlayerLeft
//     and S2C_EntityDespawn for Bob's entity. NOTE: the current server KEEPS the leaving player's
//     entities (owner = 0, so it can reconnect) and sends no despawn, so that check probably fails
//     (not verified at runtime).
static void Test_DisconnectCleanup() {
    printf("\n=== Test: Disconnect Cleanup ===\n");

    TestClient client1, client2;
    client1.Init("Alice");
    client2.Init("Bob");

    client1.Connect("127.0.0.1", KMP_DEFAULT_PORT);
    client2.Connect("127.0.0.1", KMP_DEFAULT_PORT);

    client1.PollUntil([&]() { return client1.connected; }, 3000);
    client2.PollUntil([&]() { return client2.connected; }, 3000);

    client1.SendHandshake();
    client1.PollUntil([&]() { return client1.handshakeOk; }, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client2.SendHandshake();
    client2.PollUntil([&]() { return client2.handshakeOk; }, 3000);

    // Drain
    client1.Poll(200);
    client2.Poll(200);

    // Client 2 spawns an entity
    client2.SendEntitySpawn(-51200.f, 1600.f, 2700.f);
    client2.PollUntil([&]() { return client2.myEntityId != 0; }, 3000);
    // Client 1 sees it
    client1.Poll(500);

    PlayerID bobId = client2.playerId;
    EntityID bobEntity = client2.myEntityId;
    TestAssert(bobEntity != 0, "Bob has a spawned entity");

    // Now Client 2 disconnects
    printf("    Bob disconnecting...\n");
    client2.Disconnect();
    client2.Destroy();

    // Give server time to process disconnect
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ES: Alice debería recibir: 1) S2C_EntityDespawn de la entidad de Bob; 2) S2C_PlayerLeft de Bob.
    // EN: Client 1 should receive:
    // 1. S2C_EntityDespawn for Bob's entity
    // 2. S2C_PlayerLeft for Bob
    client1.Poll(2000);

    bool gotDespawn = false;
    for (auto id : client1.entitiesDespawned) {
        if (id == bobEntity) {
            gotDespawn = true;
            break;
        }
    }
    TestAssert(gotDespawn, "Client 1 received EntityDespawn for Bob's entity");

    bool gotPlayerLeft = false;
    for (auto& pl : client1.playersLeft) {
        if (pl.playerId == bobId) {
            gotPlayerLeft = true;
            break;
        }
    }
    TestAssert(gotPlayerLeft, "Client 1 received PlayerLeft for Bob");

    client1.Disconnect();
    client1.Destroy();
}

// ES: Test 7 - TimeSync: tras el handshake llega al menos un S2C_TimeSync en 5 s (el servidor lo manda
//     cada 5 s, así que el margen es justo).
// EN: Test 7 - TimeSync: after the handshake at least one S2C_TimeSync arrives within 5 s (the server
//     sends it every 5 s, so the margin is tight).
static void Test_TimeSync() {
    printf("\n=== Test: Time Sync ===\n");

    TestClient client;
    client.Init("TimeTestPlayer");

    client.Connect("127.0.0.1", KMP_DEFAULT_PORT);
    client.PollUntil([&]() { return client.connected; }, 3000);

    client.SendHandshake();
    client.PollUntil([&]() { return client.handshakeOk; }, 3000);
    TestAssert(client.handshakeOk, "Client connected and handshook");

    // Wait for time sync packets (server sends these periodically)
    bool gotTimeSync = client.PollUntil([&]() {
        return client.timeSyncsReceived > 0;
    }, 5000);
    TestAssert(gotTimeSync, "Received at least one TimeSync packet");
    printf("    TimeSyncs received: %d\n", client.timeSyncsReceived);

    client.Disconnect();
    client.Destroy();
}

// ES: Test 8 - Varias entidades: Alice crea 3 (simula una escuadra) y ambos clientes reciben los 3 spawns.
// EN: Test 8 - Several entities: Alice creates 3 (simulating a squad) and both clients get the 3 spawns.
static void Test_MultipleEntitiesPerPlayer() {
    printf("\n=== Test: Multiple Entities Per Player ===\n");

    TestClient client1, client2;
    client1.Init("Alice");
    client2.Init("Bob");

    client1.Connect("127.0.0.1", KMP_DEFAULT_PORT);
    client2.Connect("127.0.0.1", KMP_DEFAULT_PORT);

    client1.PollUntil([&]() { return client1.connected; }, 3000);
    client2.PollUntil([&]() { return client2.connected; }, 3000);

    client1.SendHandshake();
    client1.PollUntil([&]() { return client1.handshakeOk; }, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client2.SendHandshake();
    client2.PollUntil([&]() { return client2.handshakeOk; }, 3000);

    // Drain
    client1.Poll(200);
    client2.Poll(200);

    // Client 1 spawns 3 entities (simulating a squad)
    size_t c2SpawnsBefore = client2.entitiesSpawned.size();

    for (int i = 0; i < 3; i++) {
        PacketWriter w;
        w.WriteHeader(MessageType::C2S_EntitySpawnReq);
        w.WriteU32(static_cast<uint32_t>(i + 1));  // client entity ID
        w.WriteU8(static_cast<uint8_t>(EntityType::PlayerCharacter));
        w.WriteU32(client1.playerId);
        w.WriteU32(0);
        w.WriteF32(-51200.f + i * 10.f);
        w.WriteF32(1600.f);
        w.WriteF32(2700.f + i * 10.f);
        w.WriteU32(Quat().Compress());
        w.WriteU32(0);
        std::string name = "Squad_" + std::to_string(i);
        w.WriteU16(static_cast<uint16_t>(name.size()));
        w.WriteRaw(name.data(), name.size());
        client1.SendReliable(w.Data(), w.Size());

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Wait for all to arrive
    client1.Poll(1000);
    client2.Poll(1000);

    size_t c1Entities = client1.entitiesSpawned.size();
    TestAssert(c1Entities >= 3, "Client 1 received confirmations for 3 entities");
    printf("    Client 1 total entities: %zu\n", c1Entities);

    size_t c2NewSpawns = client2.entitiesSpawned.size() - c2SpawnsBefore;
    TestAssert(c2NewSpawns >= 3, "Client 2 received 3 entity spawns from Client 1");
    printf("    Client 2 new entities from Client 1: %zu\n", c2NewSpawns);

    client1.Disconnect();
    client2.Disconnect();
    client1.Destroy();
    client2.Destroy();
}

// ─────────────────────────────────────────────────
// ES: Tests de los sistemas nuevos
// EN:  New System Tests
// ─────────────────────────────────────────────────

// ES: Limpieza: desconecta y destruye ambos clientes y da tiempo al servidor.
// EN: Cleanup helper: properly disconnect and destroy both clients
static void CleanupTwoClients(TestClient& c1, TestClient& c2) {
    c1.Disconnect(); c2.Disconnect();
    c1.Destroy(); c2.Destroy();
    // Give server time to fully process both disconnects
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// ES: Preparación: conecta dos clientes, handshake y una entidad para cada uno; devuelve si todo fue bien.
// EN: Helper: connect two clients, handshake, spawn entities, return ready state
static bool SetupTwoClients(TestClient& c1, TestClient& c2,
                            const char* name1 = "Alice", const char* name2 = "Bob") {
    c1.Init(name1);
    c2.Init(name2);
    c1.Connect("127.0.0.1", KMP_DEFAULT_PORT);
    c2.Connect("127.0.0.1", KMP_DEFAULT_PORT);
    c1.PollUntil([&]() { return c1.connected; }, 3000);
    c2.PollUntil([&]() { return c2.connected; }, 3000);
    if (!c1.connected || !c2.connected) {
        printf("    SetupTwoClients: connection failed (c1=%d, c2=%d)\n",
               c1.connected, c2.connected);
        return false;
    }

    c1.SendHandshake();
    c1.PollUntil([&]() { return c1.handshakeOk || c1.wasRejected; }, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    c2.SendHandshake();
    c2.PollUntil([&]() { return c2.handshakeOk || c2.wasRejected; }, 3000);
    if (!c1.handshakeOk || !c2.handshakeOk) {
        printf("    SetupTwoClients: handshake failed (c1=%d/%d, c2=%d/%d)\n",
               c1.handshakeOk, c1.wasRejected, c2.handshakeOk, c2.wasRejected);
        return false;
    }
    c1.Poll(200); c2.Poll(200);

    // Spawn entities for both
    c1.SendEntitySpawn(-51200.f, 1600.f, 2700.f);
    c1.PollUntil([&]() { return c1.myEntityId != 0; }, 3000);
    c2.SendEntitySpawn(-51180.f, 1600.f, 2720.f);
    c2.PollUntil([&]() { return c2.myEntityId != 0; }, 3000);
    c1.Poll(300); c2.Poll(300);

    if (c1.myEntityId == 0 || c2.myEntityId == 0) {
        printf("    SetupTwoClients: entity spawn failed (c1=%u, c2=%u)\n",
               c1.myEntityId, c2.myEntityId);
        return false;
    }

    return true;
}

// ES: Test 9 - Inventario: Alice recoge (acción 0, objeto 1001 x3) y suelta un objeto (acción 1);
//     Bob recibe ambos S2C_InventoryUpdate con los datos correctos.
// EN: Test 9 - Inventory: Alice picks up (action 0, item 1001 x3) and drops an item (action 1);
//     Bob receives both S2C_InventoryUpdate with the right data.
static void Test_InventorySync() {
    printf("\n=== Test: Inventory Sync ===\n");

    TestClient c1, c2;
    bool ready = SetupTwoClients(c1, c2);
    TestAssert(ready, "Both clients connected with entities");
    if (!ready) { CleanupTwoClients(c1, c2); return; }

    // Client 1 picks up an item
    size_t c2InvBefore = c2.inventoryUpdates.size();
    c1.SendItemPickup(c1.myEntityId, 1001, 3); // item template 1001, qty 3

    // Client 2 should receive inventory update
    bool c2GotInvUpdate = c2.PollUntil([&]() {
        return c2.inventoryUpdates.size() > c2InvBefore;
    }, 3000);
    // Also poll c1
    c1.Poll(200);

    TestAssert(c2GotInvUpdate, "Client 2 received inventory update from Client 1");
    if (c2GotInvUpdate) {
        auto& inv = c2.inventoryUpdates.back();
        TestAssert(inv.action == 0, "Inventory action is 'add' (0)");
        TestAssert(inv.itemTemplateId == 1001, "Item template ID matches");
        TestAssert(inv.quantity == 3, "Item quantity matches");
        printf("    Received: entity=%u item=%u qty=%d action=%u\n",
               inv.entityId, inv.itemTemplateId, inv.quantity, inv.action);
    }

    // Client 1 drops an item
    size_t c2InvBefore2 = c2.inventoryUpdates.size();
    c1.SendItemDrop(c1.myEntityId, 1001, -51200.f, 1600.f, 2700.f);

    bool c2GotDrop = c2.PollUntil([&]() {
        return c2.inventoryUpdates.size() > c2InvBefore2;
    }, 3000);
    c1.Poll(200);

    TestAssert(c2GotDrop, "Client 2 received item drop update");
    if (c2GotDrop) {
        auto& inv = c2.inventoryUpdates.back();
        TestAssert(inv.action == 1, "Inventory action is 'remove' (1)");
    }

    CleanupTwoClients(c1, c2);
}

// ES: Test 10 - Comercio: Alice compra (objeto 2001, 1 unidad, precio 500) a una tienda NPC y recibe
//     S2C_TradeResult con success = 1.
// EN: Test 10 - Trade: Alice buys (item 2001, 1 unit, price 500) from an NPC shop and gets
//     S2C_TradeResult with success = 1.
static void Test_TradeSync() {
    printf("\n=== Test: Trade Sync ===\n");

    TestClient c1, c2;
    bool ready = SetupTwoClients(c1, c2);
    TestAssert(ready, "Both clients connected with entities");
    if (!ready) { CleanupTwoClients(c1, c2); return; }

    // Client 1 sends a trade request
    c1.SendTradeRequest(c1.myEntityId, 2001, 1, 500);

    // Both should receive trade result (server broadcasts)
    bool c1GotResult = c1.PollUntil([&]() {
        return !c1.tradeResults.empty();
    }, 3000);
    c2.Poll(500);

    TestAssert(c1GotResult, "Client 1 received trade result");
    if (c1GotResult) {
        auto& tr = c1.tradeResults.back();
        TestAssert(tr.success == 1, "Trade was accepted");
        TestAssert(tr.itemTemplateId == 2001, "Trade item ID matches");
        printf("    Trade result: buyer=%u item=%u qty=%d success=%u\n",
               tr.buyerEntityId, tr.itemTemplateId, tr.quantity, tr.success);
    }

    CleanupTwoClients(c1, c2);
}

// ES: Test 11 - Escuadras: Alice crea "Alpha Squad"; ambos reciben S2C_SquadCreated con un ID válido.
// EN: Test 11 - Squads: Alice creates "Alpha Squad"; both get S2C_SquadCreated with a valid ID.
static void Test_SquadSync() {
    printf("\n=== Test: Squad Sync ===\n");

    TestClient c1, c2;
    bool ready = SetupTwoClients(c1, c2);
    TestAssert(ready, "Both clients connected with entities");
    if (!ready) { CleanupTwoClients(c1, c2); return; }

    // Client 1 creates a squad
    size_t c2SquadsBefore = c2.squadsCreated.size();
    c1.SendSquadCreate(c1.myEntityId, "Alpha Squad");

    // Both clients should receive squad created broadcast
    bool c1GotSquad = c1.PollUntil([&]() {
        return !c1.squadsCreated.empty();
    }, 3000);

    bool c2GotSquad = c2.PollUntil([&]() {
        return c2.squadsCreated.size() > c2SquadsBefore;
    }, 3000);

    TestAssert(c1GotSquad, "Client 1 received squad creation confirmation");
    TestAssert(c2GotSquad, "Client 2 received squad creation broadcast");

    if (c1GotSquad) {
        uint32_t squadId = c1.squadsCreated.back();
        TestAssert(squadId > 0, "Squad has valid net ID");
        printf("    Squad net ID: %u\n", squadId);
    }

    CleanupTwoClients(c1, c2);
}

// ES: Test 12 - Relaciones de facción: Alice pone la relación 100 <-> 200 a -50; ambos la reciben con
//     los valores exactos.
// EN: Test 12 - Faction relations: Alice sets relation 100 <-> 200 to -50; both receive it with the
//     exact values.
static void Test_FactionRelationSync() {
    printf("\n=== Test: Faction Relation Sync ===\n");

    TestClient c1, c2;
    bool ready = SetupTwoClients(c1, c2);
    TestAssert(ready, "Both clients connected with entities");
    if (!ready) { CleanupTwoClients(c1, c2); return; }

    // Client 1 changes a faction relation
    size_t c2FactionBefore = c2.factionRelations.size();
    c1.SendFactionRelation(100, 200, -50.0f); // Faction 100 vs 200, hostile

    // Both should receive the relation change (server broadcasts to ALL)
    bool c1GotRelation = c1.PollUntil([&]() {
        return !c1.factionRelations.empty();
    }, 3000);
    bool c2GotRelation = c2.PollUntil([&]() {
        return c2.factionRelations.size() > c2FactionBefore;
    }, 3000);

    TestAssert(c1GotRelation, "Client 1 received faction relation confirmation");
    TestAssert(c2GotRelation, "Client 2 received faction relation broadcast");

    if (c2GotRelation) {
        auto& fr = c2.factionRelations.back();
        TestAssert(fr.factionIdA == 100, "Faction A ID matches");
        TestAssert(fr.factionIdB == 200, "Faction B ID matches");
        TestAssert(fr.relation == -50.0f, "Relation value matches");
        printf("    Faction %u <-> %u = %.1f\n", fr.factionIdA, fr.factionIdB, fr.relation);
    }

    CleanupTwoClients(c1, c2);
}

// ES: Test 13 - Edificios: Alice coloca un edificio (plantilla 5001) y lo desmonta; Bob recibe
//     S2C_BuildPlaced y S2C_BuildDestroyed. OJO: el test espera que Alice también reciba BuildPlaced,
//     pero el servidor lo envía con BroadcastExcept (a todos menos al constructor), así que esa
//     comprobación probablemente falla y, sin buildingId, la parte de desmontar no se ejecuta
//     (sin verificar en ejecución).
// EN: Test 13 - Buildings: Alice places a building (template 5001) and dismantles it; Bob receives
//     S2C_BuildPlaced and S2C_BuildDestroyed. NOTE: the test expects Alice to get BuildPlaced too, but
//     the server sends it with BroadcastExcept (everyone but the builder), so that check probably
//     fails and, with no buildingId, the dismantle part is skipped (not verified at runtime).
static void Test_BuildingSync() {
    printf("\n=== Test: Building Placement & Dismantle ===\n");

    TestClient c1, c2;
    bool ready = SetupTwoClients(c1, c2);
    TestAssert(ready, "Both clients connected with entities");
    if (!ready) { CleanupTwoClients(c1, c2); return; }

    // Client 1 places a building
    size_t c2BuildsBefore = c2.buildingsPlaced.size();
    c1.SendBuildRequest(5001, -51200.f, 1600.f, 2710.f);

    // Both should receive building placed
    bool c1GotBuild = c1.PollUntil([&]() {
        return !c1.buildingsPlaced.empty();
    }, 3000);
    bool c2GotBuild = c2.PollUntil([&]() {
        return c2.buildingsPlaced.size() > c2BuildsBefore;
    }, 3000);

    TestAssert(c1GotBuild, "Client 1 received building placement confirmation");
    TestAssert(c2GotBuild, "Client 2 received building placement broadcast");

    EntityID buildingId = 0;
    if (c1GotBuild) {
        auto& bp = c1.buildingsPlaced.back();
        buildingId = bp.entityId;
        TestAssert(bp.templateId == 5001, "Building template ID matches");
        TestAssert(bp.builderId == c1.playerId, "Builder ID matches Client 1");
        printf("    Building ID: %u, template: %u, builder: %u\n",
               bp.entityId, bp.templateId, bp.builderId);
    }

    if (buildingId == 0) {
        CleanupTwoClients(c1, c2);
        return;
    }

    // Client 1 dismantles the building
    size_t c2DestroyBefore = c2.buildingsDestroyed.size();
    c1.SendBuildDismantle(buildingId);

    bool c2GotDestroy = c2.PollUntil([&]() {
        return c2.buildingsDestroyed.size() > c2DestroyBefore;
    }, 3000);
    c1.Poll(500);

    TestAssert(c2GotDestroy, "Client 2 received building destruction");
    if (c2GotDestroy) {
        bool found = false;
        for (auto id : c2.buildingsDestroyed) {
            if (id == buildingId) { found = true; break; }
        }
        TestAssert(found, "Destroyed building ID matches placed building");
    }

    CleanupTwoClients(c1, c2);
}

// ES: Test 14 - Navegador de servidores: sin handshake, C2S_ServerQuery -> S2C_ServerInfo con la versión
//     de protocolo correcta y maxPlayers > 0.
// EN: Test 14 - Server browser: without a handshake, C2S_ServerQuery -> S2C_ServerInfo with the right
//     protocol version and maxPlayers > 0.
static void Test_ServerBrowser() {
    printf("\n=== Test: Server Browser Query ===\n");

    TestClient client;
    TestAssert(client.Init("BrowserQuery"), "Client init for server query");

    client.Connect("127.0.0.1", KMP_DEFAULT_PORT);
    bool enetConnected = client.PollUntil([&]() { return client.connected; }, 3000);
    TestAssert(enetConnected, "ENet connection established for query");

    if (!enetConnected) {
        client.Disconnect();
        client.Destroy();
        return;
    }

    // ES: Envía la consulta (no hace falta handshake).
    // EN: Send server query (no handshake needed)
    PacketWriter w;
    w.WriteHeader(MessageType::C2S_ServerQuery);
    MsgServerQuery query{};
    query.protocolVersion = KMP_PROTOCOL_VERSION;
    w.WriteRaw(&query, sizeof(query));
    client.SendReliable(w.Data(), w.Size());

    // ES: Debe llegar S2C_ServerInfo; TestClient no lo maneja, así que se lee a mano aquí.
    // EN: We should receive S2C_ServerInfo - track it manually since TestClient
    // doesn't have a handler for it. We'll just check we get a packet back.
    bool gotResponse = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < deadline) {
        ENetEvent event;
        if (enet_host_service(client.host, &event, 50) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                if (event.packet->dataLength >= sizeof(PacketHeader)) {
                    PacketReader r(event.packet->data, event.packet->dataLength);
                    PacketHeader hdr;
                    r.ReadHeader(hdr);
                    if (hdr.type == MessageType::S2C_ServerInfo) {
                        MsgServerInfo info;
                        if (r.ReadRaw(&info, sizeof(info))) {
                            gotResponse = true;
                            printf("    Server: '%s' (%u/%u players) port=%u pvp=%u\n",
                                   info.serverName, info.currentPlayers,
                                   info.maxPlayers, info.port, info.pvpEnabled);
                            TestAssert(info.protocolVersion == KMP_PROTOCOL_VERSION,
                                       "Server protocol version matches");
                            TestAssert(info.maxPlayers > 0, "Server reports max players > 0");
                        }
                    }
                }
                enet_packet_destroy(event.packet);
                if (gotResponse) break;
            }
        }
    }
    TestAssert(gotResponse, "Received S2C_ServerInfo response");

    client.Disconnect();
    client.Destroy();
}

// ES: Test 15 - Sesión completa de punta a punta con "Host" y "Joiner": posiciones, edificio,
//     inventario, chat y desconexión limpia (el otro recibe PlayerLeft).
// EN: Test 15 - Full end-to-end session with "Host" and "Joiner": positions, building, inventory,
//     chat and clean disconnect (the other gets PlayerLeft).
static void Test_FullMultiplayerSession() {
    printf("\n=== Test: Full Multiplayer Session (End-to-End) ===\n");

    // ES: Simula una sesión completa: 1) conectan y crean personaje; 2) intercambian posiciones;
    //     3) el jugador 1 coloca un edificio; 4) recoge un objeto; 5) chatean; 6) el jugador 2 se va.
    // EN: This test simulates a complete multiplayer session:
    // 1. Two players connect and spawn
    // 2. They exchange position updates (can see each other)
    // 3. Player 1 places a building (visible to player 2)
    // 4. Player 1 picks up an item (synced to player 2)
    // 5. They chat with each other
    // 6. Player 2 disconnects cleanly

    TestClient c1, c2;
    bool ready = SetupTwoClients(c1, c2, "Host", "Joiner");
    TestAssert(ready, "Full session: both players connected and spawned");
    if (!ready) { CleanupTwoClients(c1, c2); return; }

    printf("    Host entity: %u, Joiner entity: %u\n", c1.myEntityId, c2.myEntityId);

    // ES: Paso 1: posiciones (¿se ven?).
    // EN: Step 1: Position updates (can they see each other?)
    int c2PosBefore = c2.posUpdatesReceived;
    for (int i = 0; i < 3; i++) {
        c1.SendPositionUpdate(c1.myEntityId,
            -51200.f + i * 5.f, 1600.f, 2700.f);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        c2.Poll(50);
    }
    c2.Poll(500);
    bool seesEachOther = (c2.posUpdatesReceived > c2PosBefore);
    TestAssert(seesEachOther, "Full session: players can see each other's movement");

    // ES: Paso 2: colocar edificio.
    // EN: Step 2: Building placement
    c1.SendBuildRequest(9001, -51200.f, 1600.f, 2705.f);
    bool c2SawBuild = c2.PollUntil([&]() {
        return !c2.buildingsPlaced.empty();
    }, 3000);
    c1.Poll(200);
    TestAssert(c2SawBuild, "Full session: building placement synced");

    // ES: Paso 3: inventario.
    // EN: Step 3: Inventory sync
    c1.SendItemPickup(c1.myEntityId, 3001, 5);
    bool c2SawInv = c2.PollUntil([&]() {
        return !c2.inventoryUpdates.empty();
    }, 3000);
    c1.Poll(200);
    TestAssert(c2SawInv, "Full session: inventory sync works");

    // ES: Paso 4: chat.
    // EN: Step 4: Chat
    c1.chatMessages.clear();
    c2.chatMessages.clear();
    c1.SendChat("Can you see me?");
    bool chatWorks = false;
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (std::chrono::steady_clock::now() < deadline) {
            c1.Poll(25); c2.Poll(25);
            if (!c2.chatMessages.empty()) { chatWorks = true; break; }
        }
    }
    TestAssert(chatWorks, "Full session: chat relay works");

    // ES: Paso 5: desconexión limpia.
    // EN: Step 5: Clean disconnect
    PlayerID joinerId = c2.playerId;
    c2.Disconnect();
    c2.Destroy();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    c1.Poll(2000);

    bool gotLeave = false;
    for (auto& pl : c1.playersLeft) {
        if (pl.playerId == joinerId) { gotLeave = true; break; }
    }
    TestAssert(gotLeave, "Full session: disconnect cleanup notified");

    c1.Disconnect();
    c1.Destroy();
}

// ─────────────────────────────────────────────────
// ES: Programa principal
// EN:  Main
// ─────────────────────────────────────────────────

// ES: argv[1] opcional = ruta de KenshiMP.Server.exe. Arranca el servidor, espera a que acepte un
//     handshake (sonda con hasta 5 intentos), ejecuta los 15 tests con 1 s de pausa entre ellos,
//     muestra el resumen, mata el servidor y espera a Enter (bloquea si se ejecuta en CI).
// EN: Optional argv[1] = path to KenshiMP.Server.exe. Starts the server, waits until it accepts a
//     handshake (probe with up to 5 attempts), runs the 15 tests with a 1 s pause between them,
//     prints the summary, kills the server and waits for Enter (blocks if run in CI).
int main(int argc, char** argv) {
    printf("======================================\n");
    printf("  KenshiMP Integration Test Suite\n");
    printf("======================================\n\n");

    // ES: Localiza el ejecutable del servidor.
    // EN: Find server executable
    std::string serverExe;
    if (argc >= 2) {
        serverExe = argv[1];
    } else {
        serverExe = FindServerExe();
    }

    if (serverExe.empty()) {
        printf("ERROR: Could not find KenshiMP.Server.exe\n");
        printf("Usage: KenshiMP.IntegrationTest.exe [path/to/KenshiMP.Server.exe]\n");
        printf("Searched: current dir, parent dir, Kenshi dir\n");
        return 1;
    }
    printf("[*] Using server: %s\n", serverExe.c_str());

    // ES: Inicializa ENet.
    // EN: Init ENet
    if (enet_initialize() != 0) {
        printf("ERROR: Failed to initialize ENet\n");
        return 1;
    }

    // ES: Arranca el servidor.
    // EN: Start server
#ifdef _WIN32
    if (!StartServer(serverExe.c_str())) {
        enet_deinitialize();
        return 1;
    }
#endif

    // ES: El servidor se bloquea con el descubrimiento UPnP (hasta 3 intentos de 1 s) y la carga del mundo
    //     antes de entrar en su bucle; el arranque puede tardar 5-10 s o más.
    // EN: Server blocks on UPnP discovery (up to 3 retries × 1s each) + world load
    // before entering its main loop. Total startup can be 5-10+ seconds.
    printf("[*] Waiting for server to start (UPnP discovery may take a few seconds)...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // ES: Calentamiento: reintenta conectar (y hacer handshake) hasta que el servidor responda.
    // EN: Warm-up: keep trying to connect until server is ready
    {
        printf("[*] Probing server...\n");
        bool ready = false;
        for (int attempt = 0; attempt < 5 && !ready; attempt++) {
            TestClient probe;
            probe.Init("Probe");
            probe.Connect("127.0.0.1", KMP_DEFAULT_PORT);
            ready = probe.PollUntil([&]() { return probe.connected; }, 3000);
            if (ready) {
                probe.SendHandshake();
                probe.PollUntil([&]() { return probe.handshakeOk; }, 3000);
                ready = probe.handshakeOk;
            }
            probe.Disconnect();
            probe.Destroy();
            if (!ready) {
                printf("[*] Attempt %d: not ready yet, retrying...\n", attempt + 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }
        if (!ready) {
            printf("ERROR: Server did not accept connections after ~17s\n");
            StopServer();
            enet_deinitialize();
            return 1;
        }
        printf("[*] Server is ready!\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    // ES: ── Ejecutar los tests ──
    // EN: ── Run Tests ──

    Test_ServerConnection();
    // ES: Pausa corta entre tests para que el servidor limpie.
    // EN: Small pause between tests to let server clean up
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_TwoPlayersConnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_EntitySpawnAndBroadcast();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_PositionSync();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_ChatRelay();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_DisconnectCleanup();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_TimeSync();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_MultipleEntitiesPerPlayer();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_InventorySync();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_TradeSync();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_SquadSync();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_FactionRelationSync();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_BuildingSync();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_ServerBrowser();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    Test_FullMultiplayerSession();

    // ES: ── Resultados ──
    // EN: ── Results ──
    printf("\n======================================\n");
    printf("  Results: %d passed, %d failed\n",
           g_testsPassed, g_testsFailed);
    printf("======================================\n");

    // ES: Para el servidor.
    // EN: Stop server
#ifdef _WIN32
    StopServer();
#endif

    enet_deinitialize();

    if (g_testsFailed > 0) {
        printf("\nSome tests FAILED. Check output above.\n");
    } else {
        printf("\nAll tests PASSED!\n");
    }

    printf("\nPress Enter to exit...\n");
    getchar();

    return g_testsFailed > 0 ? 1 : 0;
}
