# Host-Authoritative Game Speed Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the integrated-host player change `gameSpeed` at runtime via `/gamespeed`, with server-enforced host-only authorization, using a new `S2C_HostAssignment` broadcast so every client knows the current host.

**Architecture:** Four small additions layered on the existing `m_hostPlayerId` state machine: new `S2C_HostAssignment` message (0x92) broadcast on host assignment + reassignment; loopback flag on `ConnectedPlayer` so the integrated host wins race conditions against LAN peers; new `commandType = 5` (setSpeed) reusing existing `C2S_AdminCommand` authorization; client-side `/gamespeed` slash command that sends the admin command. No schema changes to existing messages.

**Tech Stack:** C++17, CMake 3.20+ with VS 2022 generator, ENet (networking), spdlog (logging), custom unit-test harness in `KenshiMP.UnitTest/main.cpp`.

**Build command:** `cmake -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release`

**Unit test command:** `./build/bin/Release/KenshiMP.UnitTest.exe`

**Note:** This project is NOT a git repository. Checkpoint steps use a build + targeted run instead of commits.

---

## Spec Reference

`docs/superpowers/specs/2026-04-19-host-authoritative-game-speed-design.md`

## File Structure

All files already exist. No new files created.

| File | Change |
|---|---|
| `KenshiMP.Common/include/kmp/protocol.h` | Add `S2C_HostAssignment = 0x92` enum value |
| `KenshiMP.Common/include/kmp/messages.h` | Add `struct MsgHostAssignment` |
| `KenshiMP.Server/server.h` | Add `bool isLoopback` to `ConnectedPlayer`; declare `BroadcastHostAssignment()` |
| `KenshiMP.Server/server.cpp` | Detect loopback in `HandleConnect`; pass flag to player at handshake; prefer loopback when assigning host; implement + call `BroadcastHostAssignment()`; add `commandType=5` setSpeed to `HandleAdminCommand` |
| `KenshiMP.Core/core.h` | Add `PlayerID m_hostPlayerId{0}`; add `SetLocalHostPlayerId()` / `GetHostPlayerId()` |
| `KenshiMP.Core/net/packet_handler.cpp` | Add `S2C_HostAssignment` handler |
| `KenshiMP.Core/sys/builtin_commands.cpp` | Add `/gamespeed` slash command |
| `KenshiMP.UnitTest/main.cpp` | Add protocol + message-layout tests; add loopback-predicate test |

---

## Task 1: Add `S2C_HostAssignment` message type and struct

**Files:**
- Modify: `KenshiMP.Common/include/kmp/protocol.h` (add enum value after line 96)
- Modify: `KenshiMP.Common/include/kmp/messages.h` (add struct after `MsgAdminResponse` at line 296)
- Modify: `KenshiMP.UnitTest/main.cpp` (add verification test; call it from `main()`)

- [ ] **Step 1: Write the failing test**

Open `KenshiMP.UnitTest/main.cpp`. Just above `int main(` add this test function:

```cpp
// ═══════════════════════════════════════════════════════════════════════════
//  Host assignment protocol tests
// ═══════════════════════════════════════════════════════════════════════════
static void TestHostAssignmentProtocol() {
    printf("\nHost assignment protocol tests:\n");

    // The enum value is load-bearing: clients of different builds must agree on 0x92.
    TestAssert(static_cast<uint8_t>(kmp::MessageType::S2C_HostAssignment) == 0x92,
               "S2C_HostAssignment enum value is 0x92");

    // Struct layout: one PlayerID field, no padding surprises.
    TestAssert(sizeof(kmp::MsgHostAssignment) == sizeof(kmp::PlayerID),
               "MsgHostAssignment is exactly sizeof(PlayerID)");

    // Round-trip: write a MsgHostAssignment, read it back, compare.
    kmp::MsgHostAssignment original{};
    original.newHostPlayerId = 42;
    uint8_t buf[sizeof(original)];
    memcpy(buf, &original, sizeof(original));
    kmp::MsgHostAssignment copy{};
    memcpy(&copy, buf, sizeof(copy));
    TestAssert(copy.newHostPlayerId == 42, "MsgHostAssignment round-trips through byte buffer");
}
```

Call it from `main()` alongside the other `Test*()` invocations.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build --config Release --target KenshiMP.UnitTest 2>&1 | tail -30
```

Expected: **compile error** — `S2C_HostAssignment` and `MsgHostAssignment` do not exist yet.

- [ ] **Step 3: Add the enum value**

In `KenshiMP.Common/include/kmp/protocol.h`, find the Admin block at line 94-96:

```cpp
    // Admin (Channel 0)
    C2S_AdminCommand      = 0x90,
    S2C_AdminResponse     = 0x91,
```

Add one line immediately after `S2C_AdminResponse`:

```cpp
    // Admin (Channel 0)
    C2S_AdminCommand      = 0x90,
    S2C_AdminResponse     = 0x91,
    S2C_HostAssignment    = 0x92,  // Server → all clients: host identity (sent on assign + reassign)
```

- [ ] **Step 4: Add the struct**

In `KenshiMP.Common/include/kmp/messages.h`, find `MsgAdminResponse` at line 293-296:

```cpp
struct MsgAdminResponse {
    uint8_t  success;         // 0=denied, 1=ok
    char     responseText[128];
};
```

Add immediately after its closing brace:

```cpp
// Sent from server to all clients whenever the host identity changes.
// Initial send: at first-connect host assignment. Reassign: when current host
// disconnects and another player takes over. Clients use this to gate admin UI
// and match against their own GetLocalPlayerId() to derive IsHost() state.
struct MsgHostAssignment {
    PlayerID newHostPlayerId;
};
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build build --config Release --target KenshiMP.UnitTest 2>&1 | tail -10 && ./build/bin/Release/KenshiMP.UnitTest.exe
```

Expected output contains:
```
Host assignment protocol tests:
  [PASS] S2C_HostAssignment enum value is 0x92
  [PASS] MsgHostAssignment is exactly sizeof(PlayerID)
  [PASS] MsgHostAssignment round-trips through byte buffer
```

- [ ] **Step 6: Full-project build check**

```bash
cmake --build build --config Release 2>&1 | tail -5
```

Expected: `Build succeeded` (no warnings about the new enum/struct from other translation units).

---

## Task 2: Server `BroadcastHostAssignment()` helper

**Files:**
- Modify: `KenshiMP.Server/server.h` (declare new method near `BroadcastTimeSync` at line 124)
- Modify: `KenshiMP.Server/server.cpp` (implement after `BroadcastTimeSync` at line 998)

- [ ] **Step 1: Declare the method**

In `KenshiMP.Server/server.h`, find the `void BroadcastTimeSync();` declaration at line 124. Add one line immediately below it:

```cpp
    void BroadcastTimeSync();
    void BroadcastHostAssignment();  // S2C_HostAssignment to all clients
```

- [ ] **Step 2: Implement the method**

In `KenshiMP.Server/server.cpp`, find `void GameServer::BroadcastTimeSync()` at line 998. Immediately after its closing brace (around line 1008), add:

```cpp
void GameServer::BroadcastHostAssignment() {
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_HostAssignment);
    MsgHostAssignment msg{};
    msg.newHostPlayerId = m_hostPlayerId;
    writer.WriteRaw(&msg, sizeof(msg));
    Broadcast(writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
    spdlog::info("GameServer: Broadcast S2C_HostAssignment (newHost={})", m_hostPlayerId);
}
```

- [ ] **Step 3: Build check**

```bash
cmake --build build --config Release 2>&1 | tail -5
```

Expected: `Build succeeded` — server.exe links cleanly.

---

## Task 3: Broadcast host on first-connect assignment

**Files:**
- Modify: `KenshiMP.Server/server.cpp:605-609` (inside `HandleHandshake`)

- [ ] **Step 1: Update the assignment block**

In `KenshiMP.Server/server.cpp`, find:

```cpp
    // First connected player is the host
    if (m_hostPlayerId == 0) {
        m_hostPlayerId = id;
        spdlog::info("GameServer: Player '{}' is the HOST (ID: {})", player.name, id);
    }
```

Replace with:

```cpp
    // First connected player is the host (loopback priority handled in Task 6).
    bool hostChanged = false;
    if (m_hostPlayerId == 0) {
        m_hostPlayerId = id;
        hostChanged = true;
        spdlog::info("GameServer: Player '{}' is the HOST (ID: {})", player.name, id);
    }
```

Then find the next line after the handshake ack is sent (search for the end of the `// Send handshake ack` block — typically where the packet is flushed to peer, around line 640-650). Immediately **after** the handshake ack is sent to the new peer, add:

```cpp
    if (hostChanged) {
        BroadcastHostAssignment();
    }
```

(Broadcasting *after* the ack ensures the new host receives their own assignment in the correct order: Ack first, then HostAssignment.)

- [ ] **Step 2: Build check**

```bash
cmake --build build --config Release 2>&1 | tail -5
```

Expected: `Build succeeded`.

---

## Task 4: Broadcast host on reassignment (disconnect)

**Files:**
- Modify: `KenshiMP.Server/server.cpp:281-289` (inside `HandleDisconnect`)

- [ ] **Step 1: Update the reassignment block**

In `KenshiMP.Server/server.cpp`, find:

```cpp
        if (leavingId == m_hostPlayerId) {
            m_hostPlayerId = 0;
```

followed by a block that picks the next player and assigns host. The current code looks like:

```cpp
        if (leavingId == m_hostPlayerId) {
            m_hostPlayerId = 0;
            if (!m_players.empty()) {
                auto it = m_players.begin();
                m_hostPlayerId = it->first;
                spdlog::info("GameServer: Host reassigned to '{}' (ID: {})",
                             it->second.name, it->first);
                BroadcastSystemMessage(it->second.name + " is now the host");
            }
        }
```

Add `BroadcastHostAssignment();` immediately after the `BroadcastSystemMessage` line:

```cpp
        if (leavingId == m_hostPlayerId) {
            m_hostPlayerId = 0;
            if (!m_players.empty()) {
                auto it = m_players.begin();
                m_hostPlayerId = it->first;
                spdlog::info("GameServer: Host reassigned to '{}' (ID: {})",
                             it->second.name, it->first);
                BroadcastSystemMessage(it->second.name + " is now the host");
                BroadcastHostAssignment();  // notify all clients of the new host id
            }
        }
```

If `m_players` is empty (last player left), do NOT broadcast — there's no one to receive it. `m_hostPlayerId = 0` is the steady state until the next connect.

- [ ] **Step 2: Build check**

```bash
cmake --build build --config Release 2>&1 | tail -5
```

Expected: `Build succeeded`.

---

## Task 5: Loopback detection helper + unit test

**Files:**
- Modify: `KenshiMP.Server/server.cpp` (add anonymous-namespace helper near top of file)
- Modify: `KenshiMP.UnitTest/main.cpp` (add predicate test)

- [ ] **Step 1: Write the failing test**

In `KenshiMP.UnitTest/main.cpp`, just above `int main(`, add:

```cpp
// ═══════════════════════════════════════════════════════════════════════════
//  Loopback detection test
// ═══════════════════════════════════════════════════════════════════════════
// We don't link to ENet here. Re-implement the predicate inline with the exact
// same logic the server uses, then test the bit pattern.
static bool TestOnly_IsLoopbackHost(uint32_t hostNetOrder) {
    // ENet stores address.host in network byte order. 127.0.0.1 in host byte
    // order is 0x7F000001; in network byte order on little-endian systems, the
    // bytes reverse to 0x0100007F.
    return hostNetOrder == 0x0100007Fu;
}

static void TestLoopbackDetection() {
    printf("\nLoopback detection tests:\n");

    // 127.0.0.1 in network byte order
    TestAssert(TestOnly_IsLoopbackHost(0x0100007Fu), "127.0.0.1 detected as loopback");

    // 192.168.1.1 (LAN) — should NOT be loopback
    TestAssert(!TestOnly_IsLoopbackHost(0x0101A8C0u), "192.168.1.1 detected as non-loopback");

    // 0.0.0.0 (INADDR_ANY) — should NOT be loopback
    TestAssert(!TestOnly_IsLoopbackHost(0u), "0.0.0.0 detected as non-loopback");

    // 127.0.0.2 — different loopback-range IP, should still be non-match for our exact check
    // (spec uses exact 127.0.0.1 match; other loopback IPs are rare in practice)
    TestAssert(!TestOnly_IsLoopbackHost(0x0200007Fu), "127.0.0.2 not matched (exact 127.0.0.1 only)");
}
```

Call it from `main()`.

- [ ] **Step 2: Run the test to verify it passes**

```bash
cmake --build build --config Release --target KenshiMP.UnitTest 2>&1 | tail -5 && ./build/bin/Release/KenshiMP.UnitTest.exe
```

Expected: all four `Loopback detection` assertions `[PASS]`. (The test is self-contained — it doesn't depend on server code.)

- [ ] **Step 3: Add the real server-side helper**

In `KenshiMP.Server/server.cpp`, near the top of the file (after the includes, before the first function), add an anonymous-namespace helper:

```cpp
namespace {
// True when this ENet address is 127.0.0.1 (network byte order = 0x0100007F).
// Used to detect the integrated-host client: the injector launches server.exe
// and connects from loopback, so this peer is authoritative even if a LAN
// player briefly won the first-connect race.
bool IsLoopbackAddress(const ENetAddress& addr) {
    return addr.host == 0x0100007Fu;
}
} // namespace
```

- [ ] **Step 4: Build check**

```bash
cmake --build build --config Release 2>&1 | tail -5
```

Expected: `Build succeeded`.

---

## Task 6: Store loopback flag on `ConnectedPlayer`; prefer loopback when assigning host

**Files:**
- Modify: `KenshiMP.Server/server.h:17-27` (add field to `ConnectedPlayer`)
- Modify: `KenshiMP.Server/server.cpp` (set field in `HandleConnect`/handshake path; update host-assignment block)

- [ ] **Step 1: Add the field**

In `KenshiMP.Server/server.h`, find `struct ConnectedPlayer`:

```cpp
struct ConnectedPlayer {
    PlayerID    id;
    std::string name;
    ENetPeer*   peer;
    Vec3        position;
    ZoneCoord   zone;
    uint32_t    ping;
    float       lastUpdate;
    std::vector<EntityID> ownedEntities;
    bool        lobbyReady = false;
};
```

Add `bool isLoopback = false;` just above the closing brace:

```cpp
struct ConnectedPlayer {
    PlayerID    id;
    std::string name;
    ENetPeer*   peer;
    Vec3        position;
    ZoneCoord   zone;
    uint32_t    ping;
    float       lastUpdate;
    std::vector<EntityID> ownedEntities;
    bool        lobbyReady = false;
    bool        isLoopback = false;  // True if this peer connected from 127.0.0.1 (integrated host)
};
```

- [ ] **Step 2: Populate the field in `HandleHandshake`**

In `KenshiMP.Server/server.cpp`, find `HandleHandshake`. Before the `m_players[id] = player;` assignment (around line 586), add:

```cpp
    player.isLoopback = IsLoopbackAddress(peer->address);
    if (player.isLoopback) {
        spdlog::info("GameServer: Player '{}' (ID: {}) connected from loopback — integrated host candidate",
                     player.name, id);
    }
```

- [ ] **Step 3: Update host-assignment block to prefer loopback**

Replace the block you edited in Task 3:

```cpp
    // First connected player is the host (loopback priority handled in Task 6).
    bool hostChanged = false;
    if (m_hostPlayerId == 0) {
        m_hostPlayerId = id;
        hostChanged = true;
        spdlog::info("GameServer: Player '{}' is the HOST (ID: {})", player.name, id);
    }
```

With:

```cpp
    // Host assignment: loopback peer always wins over non-loopback.
    // Scenarios:
    //   (a) No host yet → this player becomes host regardless of loopback.
    //   (b) Current host is non-loopback AND this player is loopback → override.
    //   (c) Current host is loopback OR this player is non-loopback → keep current.
    bool hostChanged = false;
    if (m_hostPlayerId == 0) {
        m_hostPlayerId = id;
        hostChanged = true;
        spdlog::info("GameServer: Player '{}' is the HOST (ID: {}, loopback={})",
                     player.name, id, player.isLoopback);
    } else if (player.isLoopback) {
        auto curHostIt = m_players.find(m_hostPlayerId);
        if (curHostIt != m_players.end() && !curHostIt->second.isLoopback) {
            spdlog::info("GameServer: Loopback peer '{}' (ID: {}) overrides non-loopback host '{}'",
                         player.name, id, curHostIt->second.name);
            m_hostPlayerId = id;
            hostChanged = true;
        }
    }
```

Note: the broadcast call added in Task 3 (`if (hostChanged) BroadcastHostAssignment();`) after the handshake ack is unchanged — it fires correctly in both override and first-assign paths.

- [ ] **Step 4: Build check**

```bash
cmake --build build --config Release 2>&1 | tail -5
```

Expected: `Build succeeded`.

---

## Task 7: Server `setSpeed` admin command (commandType=5)

**Files:**
- Modify: `KenshiMP.Server/server.cpp:2090` (add `case 5:` in `HandleAdminCommand` switch)

- [ ] **Step 1: Add the case**

In `KenshiMP.Server/server.cpp`, find the `switch (msg.commandType)` block at line 2090. After `case 4: { // Announce` block ends (around line 2154), add a new case before the closing `}` of the switch:

```cpp
    case 5: { // Set game speed
        float newSpeed = msg.floatParam;
        if (newSpeed >= 0.1f && newSpeed <= 10.0f) {
            m_config.gameSpeed = newSpeed;
            BroadcastTimeSync();  // Immediately push new speed to all clients
            char buf[64];
            snprintf(buf, sizeof(buf), "Game speed set to %.2fx", newSpeed);
            responseText = buf;
            spdlog::info("GameServer: Host changed gameSpeed to {:.2f}x", newSpeed);
        } else {
            responseText = "Invalid speed (must be 0.1-10.0)";
        }
        break;
    }
```

Note: the authorization check at the top of `HandleAdminCommand` (the `player.id != m_hostPlayerId` early-return at line 2074) already rejects non-host senders. No extra guard needed inside this case.

- [ ] **Step 2: Update the comment on `MsgAdminCommand.commandType`**

In `KenshiMP.Common/include/kmp/messages.h` line 287:

```cpp
    uint8_t  commandType;     // 0=kick, 1=ban, 2=setTime, 3=setWeather, 4=announce
```

Replace with:

```cpp
    uint8_t  commandType;     // 0=kick, 1=ban, 2=setTime, 3=setWeather, 4=announce, 5=setSpeed
```

- [ ] **Step 3: Build check**

```bash
cmake --build build --config Release 2>&1 | tail -5
```

Expected: `Build succeeded`.

---

## Task 8: Client handler for `S2C_HostAssignment`

**Files:**
- Modify: `KenshiMP.Core/core.h` (add member + setters)
- Modify: `KenshiMP.Core/net/packet_handler.cpp` (add case in message-dispatch switch)

- [ ] **Step 1: Add member and accessors to `Core`**

In `KenshiMP.Core/core.h`, find the existing `m_isHost` field near line 295 and the `SetIsHost` / `IsHost` accessors near lines 85 and 182. Add alongside them:

In the public accessors block (near line 85):

```cpp
    bool IsHost() const { return m_isHost; }
    PlayerID GetHostPlayerId() const { return m_hostPlayerId; }
```

In the public mutators block (near line 182):

```cpp
    void SetIsHost(bool host) { m_isHost = host; }
    void SetLocalHostPlayerId(PlayerID id) {
        m_hostPlayerId = id;
        m_isHost = (id != 0 && id == m_localPlayerId.load());
    }
```

In the private members block (near line 295, right next to `m_isHost`):

```cpp
    bool              m_isHost = false;
    PlayerID          m_hostPlayerId = 0;  // Server-authoritative host player id
```

- [ ] **Step 2: Add the packet-handler case**

In `KenshiMP.Core/net/packet_handler.cpp`, find the top-level message switch. There's a block of `case MessageType::S2C_*` cases starting around line 138. After `case MessageType::S2C_AdminResponse:` at line 161 (and its handler call), add a new case in the same switch:

```cpp
        case MessageType::S2C_AdminResponse:
            HandleAdminResponse(reader);
            return;
        case MessageType::S2C_HostAssignment: {
            MsgHostAssignment msg{};
            if (!reader.ReadRaw(&msg, sizeof(msg))) {
                spdlog::warn("PacketHandler: Malformed S2C_HostAssignment");
                return;
            }
            core.SetLocalHostPlayerId(msg.newHostPlayerId);
            if (core.IsHost()) {
                spdlog::info("PacketHandler: You are now the host");
                core.GetNativeHud().AddSystemMessage("You are now the host.");
            } else {
                spdlog::info("PacketHandler: Host is now player {}", msg.newHostPlayerId);
            }
            return;
        }
```

(Use exact case placement: alongside other `S2C_*` handlers in the same switch level — *not* inside the nested game-world-loaded guard block, because host-assignment is valid even before the local game is loaded.)

- [ ] **Step 3: Remove the fragile first-connect heuristic**

In `KenshiMP.Core/net/packet_handler.cpp` around line 323-328, find:

```cpp
        // Determine if we're the host (player ID 1 = first connected = host)
        if (msg.currentPlayers <= 1) {
            core.SetIsHost(true);
            spdlog::info("PacketHandler: We are the HOST");
        }
```

Replace with:

```cpp
        // Host identity arrives via S2C_HostAssignment — do NOT guess from currentPlayers.
        // The server sends S2C_HostAssignment immediately after this ack when we're the host.
```

This prevents stale `m_isHost=true` from lingering when a non-host client reconnects into a session where they were the first-connecter in a past run.

- [ ] **Step 4: Build check**

```bash
cmake --build build --config Release 2>&1 | tail -5
```

Expected: `Build succeeded`. Core.dll links cleanly.

---

## Task 9: Client `/gamespeed` slash command

**Files:**
- Modify: `KenshiMP.Core/sys/builtin_commands.cpp` (add new command entry following existing `/announce` pattern at line 395-413)

- [ ] **Step 1: Locate the command dispatcher**

In `KenshiMP.Core/sys/builtin_commands.cpp`, find the existing `/announce` command implementation (around line 395-413). It follows a pattern of: parse args → build `MsgAdminCommand` → send. Use that as the template.

- [ ] **Step 2: Add the `/gamespeed` command**

Add a new command block (next to `/announce` or near the bottom of the command registration section — wherever fits the existing file structure). Template:

```cpp
    // /gamespeed <value>  — host-only; change server game speed (0.1 to 10.0)
    RegisterCommand("/gamespeed", [](Core& core, const std::vector<std::string>& args) -> std::string {
        if (args.size() < 2) {
            return "Usage: /gamespeed <value>   (e.g. /gamespeed 2.0)";
        }
        if (!core.IsHost()) {
            return "Only the host can change game speed.";
        }
        float newSpeed = 0.f;
        try {
            newSpeed = std::stof(args[1]);
        } catch (...) {
            return "Invalid number: " + args[1];
        }
        if (newSpeed < 0.1f || newSpeed > 10.0f) {
            return "Game speed must be between 0.1 and 10.0.";
        }

        MsgAdminCommand msg{};
        msg.commandType = 5; // setSpeed
        msg.floatParam  = newSpeed;

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_AdminCommand);
        writer.WriteRaw(&msg, sizeof(msg));
        core.GetClient().SendReliable(writer.Data(), writer.Size());

        char buf[64];
        snprintf(buf, sizeof(buf), "Game speed request sent (%.2fx).", newSpeed);
        return buf;
    });
```

(If the file uses a different registration pattern — e.g. if commands are added to a table rather than via `RegisterCommand` — follow the surrounding style. The existing `/kick` command at `builtin_commands.cpp:385-391` and `/announce` at line 407-413 are the authoritative patterns to mirror.)

- [ ] **Step 3: Build check**

```bash
cmake --build build --config Release 2>&1 | tail -5
```

Expected: `Build succeeded`. Core.dll builds cleanly.

---

## Task 10: Manual end-to-end smoke test

**Files:** None modified. This task is validation.

- [ ] **Step 1: Start a dedicated server**

Open a terminal. From the project root:

```bash
./build/bin/Release/KenshiMP.Server.exe
```

Expected log lines within a few seconds:
```
GameServer: Listening on port 27800
Auto-save: every 60 seconds
```

- [ ] **Step 2: Connect as loopback host**

Launch Kenshi with the KenshiMP mod loaded. Open the native multiplayer menu, click **Host** (or use `/connect 127.0.0.1` from the in-game console).

On the server terminal, expect:
```
GameServer: Incoming connection from 127.0.0.1:<port>
GameServer: Player '<name>' (ID: 1) connected from loopback — integrated host candidate
GameServer: Player '<name>' is the HOST (ID: 1, loopback=true)
GameServer: Broadcast S2C_HostAssignment (newHost=1)
```

In the game HUD / system messages, expect:
```
You are now the host.
```

- [ ] **Step 3: Try `/gamespeed 2` from the host**

Open the in-game console. Type:

```
/gamespeed 2
```

Expected HUD message: `Game speed request sent (2.00x).`

On the server terminal, expect:
```
GameServer: Host changed gameSpeed to 2.00x
```

In-game: NPCs visibly move faster; day/night cycle accelerates.

- [ ] **Step 4: Connect a second client (non-host)**

Launch Kenshi from a second machine on the LAN (or second Steam copy). Use `/connect <host-lan-ip>`.

On the server, expect the new player NOT to become host (they're non-loopback):
```
GameServer: Player '<name2>' (ID: 2, ...)
# NO "is the HOST" line for player 2
# NO S2C_HostAssignment broadcast unless host changed
```

On the second client's HUD, expect:
```
Host is now player 1
```

(Note: this text only prints to console log; the HUD just suppresses the "You are now the host" message.)

- [ ] **Step 5: Try `/gamespeed 3` from the non-host**

On the second client, type `/gamespeed 3`.

Expected HUD response: `Only the host can change game speed.`

No packet is sent. Server terminal shows nothing. Game speed remains 2.0×.

- [ ] **Step 6: Host disconnects; verify reassignment**

Quit the first (host) client. On the server, expect:
```
GameServer: Host reassigned to '<name2>' (ID: 2)
GameServer: Broadcast S2C_HostAssignment (newHost=2)
```

On the remaining client's HUD: `You are now the host.`

- [ ] **Step 7: New host can change speed**

On the remaining client (now host), type `/gamespeed 1.0`. Expected: accepted, game speed returns to normal.

- [ ] **Step 8: Stop the server**

On the server terminal, type `stop` (or Ctrl+C). Clean exit, no errors.

---

## Self-Review Checklist

- [x] **Spec coverage — Component 1 (S2C_HostAssignment):** Tasks 1, 2, 3, 4, 8 (message + broadcast helper + assignment broadcast + reassign broadcast + client handler).
- [x] **Spec coverage — Component 2 (Loopback detection):** Tasks 5, 6 (predicate + player-field + override logic).
- [x] **Spec coverage — Component 3 (setSpeed commandType=5):** Task 7.
- [x] **Spec coverage — Component 4 (client /gamespeed):** Task 9.
- [x] **Spec coverage — Migration path (keep server.json as startup default):** No code change required — existing behavior preserves server.json as startup value; runtime override via Task 7 does not modify server.json on disk.
- [x] **Error handling — non-host setSpeed rejected:** Task 7 relies on existing `player.id != m_hostPlayerId` gate at server.cpp:2074; Task 9 rejects client-side with IsHost() check.
- [x] **Error handling — invalid range:** Task 9 (client) and Task 7 (server) both validate [0.1, 10.0].
- [x] **Error handling — S2C_HostAssignment malformed:** Task 8 step 2 includes `ReadRaw` failure check.
- [x] **Testing — Unit tests for enum value, struct layout, loopback predicate:** Tasks 1, 5.
- [x] **Testing — Integration/manual smoke test covering all scenarios from spec:** Task 10.
- [x] **No placeholders**: every step contains real code; expected command outputs are specified.
- [x] **Type consistency:** `PlayerID` used throughout; `MsgHostAssignment.newHostPlayerId` matches accessor `GetHostPlayerId()` and setter `SetLocalHostPlayerId()`.
- [x] **No commit steps** — the project is not a git repo; checkpoints are build + run checks instead. Stated in plan header.
