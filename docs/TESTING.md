# Kenshi-Online Testing Guide

**Version:** 0.3.0-alpha  
**Last Updated:** 2026-06-04

This guide covers manual testing, automated integration tests, and debugging procedures for Kenshi-Online multiplayer mod.

---

## Table of Contents

1. [Testing Infrastructure](#testing-infrastructure)
2. [Integration Test Suite](#integration-test-suite)
3. [Manual Testing with Test Client](#manual-testing-with-test-client)
4. [Test Scenarios](#test-scenarios)
5. [Reading Logs for Debugging](#reading-logs-for-debugging)
6. [Expected vs Actual Behavior](#expected-vs-actual-behavior)
7. [Troubleshooting Common Issues](#troubleshooting-common-issues)

---

## Testing Infrastructure

### Test Projects

The mod includes two test executables:

- **KenshiMP.IntegrationTest.exe** - Automated protocol test suite (15 tests)
- **KenshiMP.TestClient.exe** - Fake player bot for manual testing

### Test Server Configuration

Tests use `server.json` in the test executable directory:

```json
{
  "serverName": "KenshiMP Server",
  "port": 27800,
  "maxPlayers": 16,
  "pvpEnabled": true,
  "tickRate": 20,
  "gameSpeed": 1.0,
  "savePath": "world.kmpsave",
  "masterServer": "162.248.94.149",
  "masterPort": 27801,
  "password": ""
}
```

---

## Integration Test Suite

### Running the Test Suite

**Automated (finds server automatically):**
```bash
cd "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
KenshiMP.IntegrationTest.exe
```

**Manual server path:**
```bash
KenshiMP.IntegrationTest.exe "C:\path\to\KenshiMP.Server.exe"
```

### Test Suite Workflow

1. **Server Discovery** - Searches for `KenshiMP.Server.exe` in common locations
2. **Server Startup** - Launches server, waits 2s + probes up to 5 times
3. **Test Execution** - Runs 15 protocol tests with 1s pause between
4. **Cleanup** - Terminates server, displays results

### Test Cases (15 Tests)

#### 1. Server Connection
- **Tests:** ENet connection, handshake protocol
- **Pass if:** Client receives valid player ID and server info

#### 2. Two Players Connect
- **Tests:** Multi-client handshake, player joined broadcast
- **Pass if:** Both clients get unique IDs, Client 1 sees "Bob" join

#### 3. Entity Spawn & Broadcast
- **Tests:** Entity creation, server ID assignment, broadcast to peers
- **Pass if:** Both clients spawn entities and see each other's spawns

#### 4. Position Sync
- **Tests:** Unreliable position updates forwarded between clients
- **Pass if:** Client 2 receives 5+ position packets from Client 1

#### 5. Chat Relay
- **Tests:** Reliable chat message broadcast
- **Pass if:** Messages relayed bidirectionally with correct content

#### 6. Disconnect Cleanup
- **Tests:** Entity despawn and player left notifications
- **Pass if:** Client 1 receives `EntityDespawn` and `PlayerLeft` for disconnected Client 2

#### 7. Time Sync
- **Tests:** Periodic server time synchronization
- **Pass if:** Client receives at least one `TimeSync` packet within 5s

#### 8. Multiple Entities Per Player
- **Tests:** Squad spawning (3 entities per player)
- **Pass if:** Both clients receive all 3 entity spawns

#### 9. Inventory Sync
- **Tests:** Item pickup/drop broadcasts
- **Pass if:** Client 2 sees item pickup and drop messages

#### 10. Trade Sync
- **Tests:** Player-to-player trading
- **Pass if:** Trade request results in `TradeResult` message

#### 11. Squad Sync
- **Tests:** Squad creation broadcast
- **Pass if:** Both clients receive `SquadCreated` with valid net ID

#### 12. Faction Relation Sync
- **Tests:** Faction standing changes broadcast
- **Pass if:** Faction relation update propagates correctly

#### 13. Building Sync
- **Tests:** Building placement and dismantle
- **Pass if:** Client 2 sees `BuildPlaced` and `BuildDestroyed`

#### 14. Server Browser Query
- **Tests:** Serverless query (no handshake needed)
- **Pass if:** `S2C_ServerInfo` contains server name, player count, protocol

#### 15. Full Multiplayer Session (End-to-End)
- **Tests:** Complete gameplay loop (spawn, position, building, inventory, chat, disconnect)
- **Pass if:** All 6 sub-steps succeed

---

## Manual Testing with Test Client

### Running the Test Client

**Default (localhost:27800):**
```bash
KenshiMP.TestClient.exe
```

**Custom server:**
```bash
KenshiMP.TestClient.exe 192.168.1.100 7777
```

**Custom name:**
```bash
KenshiMP.TestClient.exe 127.0.0.1 27800 "MyBot"
```

### Test Client Behavior

1. **Connects** to server and sends handshake
2. **Waits for host position** - Listens for first entity spawn
3. **Spawns near host** - Positions 10 units away from detected player
4. **Patrols continuously** - Walks back and forth in 100-unit line
5. **Sends position updates** - Every 50ms (20 Hz)
6. **Prints received packets** - Chat, position, spawns, combat

### Interactive Commands

| Command | Description |
|---------|-------------|
| `c <message>` | Send chat message |
| `s` | Print status (ID, entity, position, stats) |
| `h` | Show help |
| `q` | Quit |

---

## Test Scenarios

### Critical Path Tests

#### Scenario 1: 2-Player Basic Co-op
**Objective:** Verify players can see and interact with each other

**Steps:**
1. Start server (`KenshiMP.Server.exe`)
2. Player 1: Launch via Injector, load save, connect
3. Player 2: Launch via Injector, load save, connect
4. Both: Walk around and verify you see each other moving
5. Player 1: Send chat message
6. Player 2: Verify chat received

**Expected:**
- ✅ Both players appear in each other's world
- ✅ Movement is smooth (20 Hz updates)
- ✅ Chat messages propagate
- ✅ Position desync < 2 units

---

#### Scenario 2: Late Join (Player Joins Mid-Session)
**Objective:** Test spawn queue fixes for players joining during active session

**Steps:**
1. Start server
2. Player 1: Connect, load save, wait 30s
3. Player 2: Connect, load save
4. Player 1: Check if Player 2 appears within 5s

**Expected:**
- ✅ Player 2 spawns successfully (DeferredSpawnQueue)
- ✅ Player 1 sees Player 2's entity within 5s
- ✅ No "waiting for game loaded" timeout

**Fixed Issues (v0.3.0):**
- Players joining during loading were invisible → **FIXED** via DeferredSpawnQueue
- Steam deadlock on loading → **FIXED** via 90s hard timeout

---

#### Scenario 3: Combat Synchronization
**Objective:** Verify death/KO events sync across clients

**Steps:**
1. Two players connected
2. Player 1: Attack Player 2 until death/KO
3. Player 2: Check if health updates received
4. Player 1: Check if death notification appears

**Expected:**
- ✅ `S2C_CombatHit` packets broadcast damage
- ✅ `S2C_CombatDeath` sent on death
- ✅ `S2C_HealthUpdate` reflects damage

**Known Issues:**
- ❌ Damage bars don't sync (ApplyDamage hook crashes)
- ❌ Hit animations may desync

---

#### Scenario 4: Building Sync
**Objective:** Verify building placement/destruction syncs

**Steps:**
1. Two players connected
2. Player 1: Place a building (e.g., Small Shack)
3. Player 2: Verify building appears
4. Player 1: Dismantle building
5. Player 2: Verify building disappears

**Expected:**
- ✅ `S2C_BuildPlaced` broadcasts to all clients
- ✅ Building appears at correct position
- ✅ `S2C_BuildDestroyed` removes building for all

---

#### Scenario 5: Disconnect and Reconnect
**Objective:** Test cleanup and rejoin

**Steps:**
1. Two players connected
2. Player 2: Force disconnect (Alt+F4)
3. Player 1: Check for `PlayerLeft` and entity despawn
4. Player 2: Reconnect
5. Player 1: Check if Player 2 reappears

**Expected:**
- ✅ Server detects disconnect within 5s (keepalive timeout)
- ✅ `S2C_EntityDespawn` sent for all disconnected player's entities
- ✅ `S2C_PlayerLeft` sent to remaining clients
- ✅ Reconnect creates new player ID and entity

---

## Reading Logs for Debugging

### Log File Locations

#### Client Logs
```
C:\Program Files (x86)\Steam\steamapps\common\Kenshi\KenshiOnline_<PID>.log
```

#### Server Log
```
C:\Program Files (x86)\Steam\steamapps\common\Kenshi\KenshiOnline_Server.log
```

#### Crash Log
```
C:\Program Files (x86)\Steam\steamapps\common\Kenshi\KenshiOnline_CRASH.log
```

### Client Log Analysis

#### Normal Startup
```
[2026-06-04 12:30:45.123] [info] KenshiMP Core v0.3.0 initializing...
[2026-06-04 12:30:45.456] [info] CharacterSpawn hook installed at 0x140581770
[2026-06-04 12:30:46.789] [info] All hooks installed successfully (14/14)
```

#### Connection Success
```
[2026-06-04 12:31:00.123] [info] Connecting to 127.0.0.1:27800...
[2026-06-04 12:31:00.234] [info] ENet connected
[2026-06-04 12:31:00.456] [info] Received S2C_HandshakeAck (ID: 1, players: 1/16)
```

#### Entity Spawn (Late Join Fix)
```
[2026-06-04 12:31:05.346] [info] Queued spawn in DeferredSpawnQueue (ID: 5000)
[2026-06-04 12:31:07.891] [info] Replayed spawn for entity 5000
```

---

## Expected vs Actual Behavior

### Known Issues (as of v0.3.0)

#### ✅ FIXED: Players Invisible on Late Join
- **Expected:** Player joining during active session appears within 5s
- **Was:** Player invisible, required reconnect
- **Fixed:** DeferredSpawnQueue buffers spawns until GameWorld ready

#### ✅ FIXED: Steam Deadlock on Loading
- **Expected:** Loading screen finishes within 90s
- **Was:** Infinite loading screen
- **Fixed:** 90s hard timeout bypasses Steam deadlock

#### ❌ OPEN: Combat Damage Bars Don't Sync
- **Expected:** Health bars update when remote player takes damage
- **Actual:** Health bars stay full, only death syncs
- **Cause:** ApplyDamage hook crashes (NULL pointer at +0x178)
- **Workaround:** Hook disabled, server sends HealthUpdate manually

---

## Troubleshooting Common Issues

### Issue: Test Client Won't Connect

**Symptoms:**
```
Connecting...
[... 5 seconds of silence ...]
```

**Fixes:**
1. Check server is running
2. Check port in server.json matches
3. Allow UDP 27800 in Windows Firewall
4. Wait 10s for server UPnP discovery

---

### Issue: Position Updates Not Syncing

**Symptoms:**
- Players invisible or teleporting

**Debugging:**
1. Check server log for "Broadcasting S2C_PositionUpdate"
2. Check client log for "Sent C2S_PositionUpdate"
3. Verify entity spawned with serverID

---

### Issue: Crash During Combat

**Known Issue:** ApplyDamage hook crashes on NULL pointer

**Workaround:** Hook disabled in v0.3.0, server sends manual HealthUpdate

---

## Appendix: Test Data Reference

### Valid Entity Templates

- `"Greenlander"` - Human male
- `"Scorchlander"` - Human female
- `"Skeleton"` - Robot
- `"Shek"` - Horned humanoid
- `"Hive Worker"` - Insectoid

### Test Positions (Near "The Hub")

| Location | X | Y | Z |
|----------|---|---|---|
| Hub Center | -51200 | 1600 | 2700 |
| North Gate | -51200 | 1600 | 2750 |
| South Gate | -51200 | 1600 | 2650 |

---

**End of Testing Guide**

For questions or bug reports, see:
- GitHub Issues: https://github.com/The404Studios/Kenshi-Online/issues
- Email: the404studios@gmail.com
