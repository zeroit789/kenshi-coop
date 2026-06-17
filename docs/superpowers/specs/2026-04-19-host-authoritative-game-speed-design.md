# Host-Authoritative Game Speed — Design

**Date:** 2026-04-19
**Status:** Draft
**Target:** KenshiMP (Kenshi 16-player coop mod)

## Problem

`gameSpeed` is currently loaded once from `server.json` into `ServerConfig.gameSpeed` and broadcast every 5s via `S2C_TimeSync`. It cannot be changed mid-session. The protocol has no explicit "host" concept — clients infer host status from the fragile `currentPlayers <= 1` check at `packet_handler.cpp:325`. The server tracks `m_hostPlayerId` but never tells clients who the host is, so the client-side `IsHost()` state can desync from server truth (especially after host disconnect/reassign).

## Goals

- The host of a session can change `gameSpeed` at runtime.
- Only the host can change it; server enforces this.
- Every client always knows who the host is, updated on reassignment.
- No breaking changes to existing message schemas (`S2C_TimeSync`, `MsgHandshakeAck`).

## Non-Goals

- No vote system, no ops list, no multi-admin roles.
- No removal of `server.json` `gameSpeed` — becomes startup default.
- No token/secret auth — loopback detection is sufficient for the integrated-host model of a coop mod.

## Architecture

Three additions layered on the existing `m_hostPlayerId` state machine:

1. **`S2C_HostAssignment`** — new server-to-client message that authoritatively names the current host. Broadcast on first assignment and every reassignment.
2. **Loopback host detection** — if a peer connects from `INADDR_LOOPBACK` and no host is assigned yet, it wins host status regardless of first-connect race. Guarantees integrated-host behaves correctly.
3. **`setSpeed` admin command** — `commandType = 5` added to the existing `C2S_AdminCommand` flow. Reuses the existing `m_hostPlayerId` auth gate.

## Components

### 1. `S2C_HostAssignment` message

**Location:** `KenshiMP.Common/include/kmp/messages.h` + `protocol.h`

- Struct: `MsgHostAssignment { PlayerID newHostPlayerId; }`
- MessageType: `S2C_HostAssignment = 0x92` (next free slot)
- Channel: `KMP_CHANNEL_RELIABLE_ORDERED`, `ENET_PACKET_FLAG_RELIABLE`

**Broadcast triggers (server-side):**
- End of `HandleHandshake` when `m_hostPlayerId` is newly assigned.
- End of `HandleDisconnect` when host reassigned to a surviving player.

**Client handler (`packet_handler.cpp`):**
Sets `Core::SetLocalHostPlayerId(msg.newHostPlayerId)`. `IsHost()` becomes `m_hostPlayerId == GetLocalPlayerId()`. Existing `m_isHost` flag remains but is now a cache, populated only from this message.

### 2. Loopback host detection

**Location:** `KenshiMP.Server/server.cpp::HandleConnect`, `HandleHandshake`

Read `peer->address.host`; mark peer as loopback when it equals `htonl(INADDR_LOOPBACK)` (`127.0.0.1`). Store this flag on the pending-player record.

At handshake-time host assignment:
- If `m_hostPlayerId == 0` and any currently-pending player is loopback, that player wins.
- Otherwise, first-connect assignment (unchanged).

Dedicated server without integrated host: no loopback peer ever connects, so first-connect wins — no regression.

### 3. `setSpeed` admin command (commandType = 5)

**Location:** `KenshiMP.Server/server.cpp::HandleAdminCommand`

New case: validate range `[0.1, 10.0]`, write `m_config.gameSpeed`, immediately call `BroadcastTimeSync()` (instead of waiting for the 5s cadence). Send response text back via the existing admin response pipeline.

Auth: already enforced by the existing `player.id != m_hostPlayerId` early-return at the top of `HandleAdminCommand`.

### 4. Client `/gamespeed` command

**Location:** `KenshiMP.Core/sys/builtin_commands.cpp`

`/gamespeed <value>`:
- Validate `[0.1, 10.0]` client-side (fail fast, better error message).
- If `!Core::IsHost()`: print "Only the host can change game speed" and return.
- Otherwise, send `C2S_AdminCommand { commandType = 5, floatParam = value }`.

## Data Flow

**Before:**
```
server.json → m_config.gameSpeed (static) → tick() advances time
                                          → S2C_TimeSync every 5s → time_hooks::SetServerTime
                                                                    → TimeManager+0x10 (gameSpeed)
                                                                    → GameWorld+0x700 (gameSpeed)
```

**After:** All the above, plus:
```
Connect:  HandleHandshake assigns m_hostPlayerId
          → S2C_HostAssignment broadcast → clients set m_hostPlayerId + m_isHost cache

Runtime:  Host types /gamespeed N
          → client sends C2S_AdminCommand(type=5, N)
          → server auth-checks sender == m_hostPlayerId
          → m_config.gameSpeed = N
          → BroadcastTimeSync() immediately
          → all clients apply new speed within one RTT

Host leaves: HandleDisconnect reassigns m_hostPlayerId → S2C_HostAssignment broadcast
```

## Error Handling

| Scenario | Behavior |
|---|---|
| Non-host client sends `commandType=5` | Server rejects via existing auth gate; returns error text; speed unchanged. |
| Invalid speed (out of range) | Server rejects; returns "Invalid speed (0.1–10.0)"; speed unchanged. |
| No host connected (dedicated server, empty) | `m_hostPlayerId = 0`; no player can set speed. Server console retains existing direct-mutation path. |
| Host disconnects mid-change | Broadcast already dispatched if server accepted it. Next reassigned host inherits new speed. |
| `S2C_HostAssignment` arrives before handshake ack is processed | Client caches `m_hostPlayerId` immediately; `IsHost()` queries are still correct once local player id is known. |
| Loopback peer disconnects and reconnects | Next assignment on that peer re-identifies it as loopback; host status reassigned correctly. |

## Testing Strategy

**Automated (LiveTest dual mode):**
1. Integrated host (loopback) + one remote client.
2. Assert host receives `S2C_HostAssignment` with its own player id.
3. Assert remote receives `S2C_HostAssignment` naming the integrated host.
4. Host sends `C2S_AdminCommand(5, 2.0)` → assert broadcast within 100ms → assert remote's last-received `S2C_TimeSync.gameSpeed == 2`.
5. Remote sends `C2S_AdminCommand(5, 3.0)` → assert server rejection → assert speed unchanged.
6. Integrated host disconnects → assert remote receives `S2C_HostAssignment` naming itself.
7. New host sends `C2S_AdminCommand(5, 1.5)` → assert accepted.

**Manual:** Same scenarios played from NativeMenu in live game.

## Migration

- `server.json` `gameSpeed` field: retained, semantics change from authoritative → startup default. Existing server configs work unchanged.
- `S2C_TimeSync` schema: unchanged.
- `MsgHandshakeAck` schema: unchanged.
- Old clients that don't implement `S2C_HostAssignment`: ignore unknown message type (existing behavior of `packet_handler`), so they lose host-display but don't crash. Host control still works because the server-side auth gate is unchanged.
