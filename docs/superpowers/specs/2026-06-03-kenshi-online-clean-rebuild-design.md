# Kenshi-Online — Clean Rebuild Design

**Date:** 2026-06-03
**Status:** Draft for user review
**Supersedes:** the accreted `KenshiMP/` tree (kept read-only as `salvage source`)
**Backing evidence:** `KenshiMP/docs/audit-2026-06-03-gap-report.json` (18-agent read-only audit + runtime log analysis)

---

## 1. Goal

A **16-player co-op multiplayer mod for Kenshi** that **actually completes the full sync loop**:
two or more players connect, load into the same world together, **see each other spawn and move
smoothly**, chat, share a persistent world, group into parties, and trade. Combat sync is
**secondary** (death/KO + health streaming, polished later).

Equally important and explicitly in scope: the codebase must be **reorganized into clean,
single-responsibility modules** and feel like **QOL** to build, run, and play.

### Decisions locked (this session)
- **MVP focus:** Co-op squad / shared-world (shared save, party grouping, trade, base-building together). Combat polish is post-slice.
- **Build strategy:** **Clean-architecture rebuild of the broken middle, salvaging the proven low-level layer.** NOT greenfield — the reverse-engineering (verified offsets, MovRaxRsp detour, ENet/protocol/interpolation/server) is the irreplaceable asset and is *not* the bug.
- **Game data mod:** Rebuild a fresh `kenshi-online.mod` with stable `Player 1..16` CHARACTER templates + a persistent multiplayer faction, consumed by deterministic spawn. *(Open: user to confirm in review.)*

---

## 2. Root cause the rebuild must fix (the headline)

The mod is **not** broken at the network layer. ENet connects, handshakes, and streams 4,900+
packets reliably. The loop is **deadlocked at a single gate**:

1. `OnGameLoaded()` only fires from the **CharacterCreate loading-burst detector** *or* a
   **global-pointer fallback** requiring `GameWorldSingleton`/`PlayerBase`.
2. At runtime `GameWorldSingleton` resolved **UNRESOLVED** (~30 patterns failed). Both trigger
   paths are dead → the "game loaded" gate **never opens**.
3. Because the gate never opens, `PacketRouter` **drops every `S2C_EntitySpawn`** at a
   `// TODO: queue for later spawn` that was never implemented. The deferred buffer does not exist.
4. Therefore **remote characters never appear**, regardless of which spawn strategy is used —
   they all sit downstream of the closed gate.

**Three load-bearing fixes** (each independently verifiable):
- **LoadGate** detects "game loaded" by **polling the character iterator** (already proven to work
  as a fallback), decoupled from unresolved globals, with a time-based ultimate fallback.
- **PacketRouter** gets a **real deferred buffer**: queue spawns/positions/health while not-ready,
  **replay on load**.
- **Server `HandlePlayerReady`** is rewritten so the server **compiles** and runs a single
  authoritative lobby/ready state machine.

Everything else (unreliable spawn, half-synced combat, wrong appearance) is real but strictly
*downstream* of opening this gate.

---

## 3. Target architecture (15 single-responsibility modules)

Each module does **one** job, communicates through a defined interface, and is independently
testable. "Built from" = which existing file(s) it salvages.

| Module | Responsibility | Built from |
|---|---|---|
| **kmp_protocol** (Common) | Wire format: message enums, packed structs w/ explicit var-length payload schemas (spawn carries appearance/equipment/health inline), channel assignment, version | `Common/messages.h, protocol.h, serialization.cpp` — refactor |
| **NetworkClient** | ENet lifecycle, channelized send/recv, keepalive, session timeout set before connect returns | `Core/net/client.cpp` — minor fixes |
| **PacketRouter** | Decode S2C → route to appliers; **queue-while-not-ready + replay on load**; per-entity lock on snapshot writes | `Core/net/packet_handler.cpp` — fix deferral + locking |
| **EntityRegistry** | Authoritative net-id ↔ game-object map, ownership, generation field, spawn-state + timeout cleanup | `Core/sync/entity_registry.cpp` — add generation + timeout |
| **Interpolation** | Per-entity 8-snapshot ring buffer, jitter-adaptive delay, slerp/lerp, snap correction | `Core/sync/interpolation.cpp` — keep as-is |
| **SyncPipeline** | **THE single per-tick driver:** read+send local pos, drive interp, write remote pos, send equip/health diffs, drive zone interest. Owns EntityResolver/ZoneEngine/PlayerEngine | `Core/sync/sync_orchestrator.cpp` — rename, absorb legacy core path |
| **LoadGate** | Detect game-loaded via **iterator polling** (not globals), drain deferred buffer, gate first spawn, run lobby-ready barrier | `Core/game/loading_orchestrator.cpp` — decouple from globals |
| **SpawnService** | **Deterministic** remote-char creation via FactoryCreate + mod Player templates; per-player cap w/ disconnect release; spawn timeout. NPC-hijack only optional last resort | merge `spawn_manager.cpp` + entity_hooks spawn path; delete hijack/squad-bypass |
| **AppearanceSync** | Apply equipment/appearance/faction to spawned remotes from spawn payload; faction via server-assigned id → persistent local faction (no NPC-pointer writes) | new (pulls game_inventory/game_faction/player_controller) |
| **CombatSync** | Death/KO event hooks (authoritative end-states) + periodic remote health/limb poll (200–500ms) for intermediate damage | `Core/hooks/combat_hooks.cpp` — keep events, add poll; delete dead server ResolveCombat |
| **GameMemory (SDK)** | Single low-level read/write primitive layer (CharacterAccessor/iterator/offsets); one access pattern, no duplicate WorldAccessor | `Core/game/game_types.h` + `sdk/kenshi_sdk.cpp` — consolidate |
| **HookKit (Scanner)** | Pattern scan, .pdata function bounds, MinHook + MovRaxRsp detour, global-pointer discovery w/ **8KB window** | `KenshiMP.Scanner` — harden FindGlobalNearString + reentrancy |
| **ServerAuthority** | Per-tick authoritative hub: validated pos/combat/state, entity ids, **lobby+ready FSM**, broadcast, JSON persistence | `Server/server.cpp` — fix HandlePlayerReady + ready FSM |
| **UI/Overlay** | MyGUI HUD, menu, chat, slash commands, explicit connection-state enum | `Core/ui/*` — minor polish |
| **PipelineMonitor** | 1Hz telemetry/anomaly snapshots, **outside** the sync path | `Core/sync/pipeline_orchestrator.cpp` — rename, isolate |

**Orchestrator verdict (resolves the core mess):** exactly **one** sync driver (**SyncPipeline**),
**one** load/spawn gate (**LoadGate**), **one** diagnostics tap (**PipelineMonitor**), and **no facades**.

---

## 4. Salvage vs. Delete

### Salvage (proven — carry forward)
Common protocol/serialization · NetworkClient ENet integration · EntityRegistry mapping ·
Interpolation ring buffer · SyncOrchestrator per-tick pipeline · Scanner (pattern/.pdata/HookManager/MovRaxRspFix) ·
CharacterDeath/KO hooks + echo suppression · Position read/write Methods 1 & 2 · Server authoritative loop
(dispatch/validation/broadcast/JSON persistence/auto-save/master-server) · Faction `.rdata` string patch ·
time_hooks · full UI stack (HUD/menu/chat/commands) · LoadingOrchestrator burst detection ·
mod Player-template spawn approach.

### Delete (~18 dead/duplicate units — *this is the "organize it better"*)
Legacy in-core sync path (`PollLocalPositions`/`ApplyRemotePositionsDirect`/`SendCachedPackets`) ·
`zone_interest.cpp` (dead ZoneEngine clone) · `OwnershipManager` (facade) · `SyncFacilitator` (facade) ·
`asset_facilitator.cpp` (dead pass-through) · `squad_spawn_hooks.cpp` (fragile flag-flipping) ·
`building_hooks.cpp` (never installed — defer building sync) · `save_hooks.cpp` (disabled no-op) ·
`world_hooks` BuildingPlace · dead `spawn_manager` queue paths · server `combat_resolver`/`HandleAttackIntent`
(unreachable) · unused `compression.h/cpp` codecs · `WorldAccessor` duplicate singleton ·
manual `/ready` command · `install.bat` (superseded by Injector) · zombie `VisualProxy` render fields ·
dead enums `S2C_ZoneData`/`C2S_EntityAck` · uninstalled `movement_hooks` SetPosition/MoveTo bodies.

---

## 5. New repository organization

A new clean tree under `KenshiMP/` (old files become read-only salvage source; deleted units
do not move over). Proposed layout:

```
src/
  common/        kmp_protocol           (messages, wire, channels, version)
  net/           NetworkClient, PacketRouter
  sync/          SyncPipeline, EntityRegistry, Interpolation, EntityResolver, ZoneEngine, PlayerEngine
  load/          LoadGate
  spawn/         SpawnService, AppearanceSync
  combat/        CombatSync
  game/          GameMemory (SDK: accessors, offsets, iterator)
  hook/          HookKit (from Scanner)
  ui/            HUD, Menu, Chat, Commands, Overlay
  diag/          PipelineMonitor
server/          ServerAuthority (+ player/zone/persistence)
inject/          Injector
mod/             kenshi-online.mod source (FCS) + generator
docs/            specs, audit, offsets reference
tests/           unit + 2-client integration harness
```

Build stays CMake (the existing presets work). One DLL (`KenshiMP.Core.dll`), one server EXE, one injector EXE.

---

## 6. Phased roadmap

**Phase 1 — Working vertical slice (the foundation).** Detailed in §7. Two players reliably see
each other spawn and move. *This is the next implementation plan.*

**Phase 2 — Shared world & persistence.** Shared save loading so all players inhabit the same world
state; server-authoritative world persistence; time/weather sync.

**Phase 3 — Co-op squad layer (the MVP target).** Party/group mechanics, player-to-player trade,
cooperative base-building ownership. *This is where the user's MVP goal lands.*

**Phase 4 — Combat & appearance polish.** Continuous health/limb streaming, correct remote
appearance/equipment, faction-correct (non-hostile) remotes.

Each phase gets its own spec → plan → implementation cycle.

---

## 7. Phase 1 spec — the 2-player vertical slice

Ordered steps (each ends in a concrete, observable verification):

1. **Server compiles + lobby FSM.** Rewrite `GameServer::HandlePlayerReady` to take
   `ConnectedPlayer&` and real fields; add one authoritative lobby/ready state machine.
   *Verify:* server builds clean and logs ready-state transitions.
2. **Break the load deadlock (LoadGate).** Trigger `OnGameLoaded()` from **character-iterator
   polling**, independent of `GameWorldSingleton`/`PlayerBase`; add a time-based ultimate fallback.
   *Verify:* log shows `GameLoaded` reached on a vanilla load with globals unresolved.
3. **Real deferred-spawn buffer (PacketRouter).** Replace the `// TODO` with an actual queue;
   replay all buffered spawns + queued position/health in `OnGameLoaded()`.
   *Verify:* an `S2C_EntitySpawn` arriving pre-load is no longer dropped — it materializes on load.
4. **One sync driver.** Enable SyncPipeline unconditionally; delete the legacy core position path
   and the dead orchestrators/facades. *Verify:* exactly one read→send and one write per entity per tick.
5. **Deterministic spawn (SpawnService + fresh mod).** Route to FactoryCreate using mod `Player`
   templates as the **primary** path; spawn timeout + cap-slot release on disconnect.
   *Verify:* a remote char reliably appears within ~1–2s of join, in a safe interior (no NPCs to hijack).
6. **Lobby barrier.** Auto-send `C2S_LobbyReady` after faction patch + load; server broadcasts
   `S2C_LobbyStart`; clients spawn only after the barrier. *Verify:* both players exist before first spawn.
7. **Slice acceptance.** Two clients connect → both reach GameLoaded → exchange position updates →
   each spawns the other via FactoryCreate → **positions interpolate smoothly**, chat works.

### Phase 1 acceptance criteria
- On a machine with **no nearby NPCs**, a joining player's character **deterministically appears**
  to the other player within ~2s.
- Both players see each other **move smoothly** (interpolated), and **chat** round-trips.
- Server **compiles and runs** with a clean lobby/ready FSM; no reliance on `GameWorldSingleton` to load.
- Exactly **one** sync driver and **one** load gate exist in the tree (dead orchestrators deleted).

### Out of scope for Phase 1
Building sync, master-server polish, packet compression, endianness/portability, combat damage
streaming, equipment/appearance correctness, trade, party UI. (All scheduled in Phases 2–4.)

---

## 8. Validation strategy (how we test without 16 humans)

- **2-client local harness:** salvage `TestClient` (fake player that walks/chats) + run a second real
  client; server runs locally. This drives the Phase 1 acceptance loop deterministically.
- **Unit tests** for protocol round-trip (serialize→deserialize equality), deferred-buffer replay
  ordering, EntityRegistry id/generation lifecycle, LoadGate trigger logic (mocked iterator counts).
- **Integration test** (salvage `IntegrationTest`): auto-start server, connect 2 clients, assert each
  observes the other's spawn + position stream.
- **Log assertions:** the runtime logs already trace phase transitions — Phase 1 adds explicit
  `GameLoaded`, `DeferredReplay(n)`, `SpawnDeterministic` markers to assert against.

---

## 9. Risks & open questions

- **R1 — GameWorldSingleton resolution.** Hardening the 8KB scan window may still miss it on some
  builds; LoadGate deliberately does **not** depend on it, so this degrades to "some optional features
  off," not "deadlock." *Mitigated by design.*
- **R2 — FactoryCreate determinism.** History shows direct FactoryCreate returned NULL in some attempts.
  Phase 1 must validate FactoryCreate + mod-template path in isolation **before** deleting the
  NPC-hijack fallback (keep it quarantined until SpawnService is proven).
- **R3 — Fresh `.mod` authoring.** Needs correct FCS Player CHARACTER templates + faction. Requires
  confirming the generator approach (OpenConstructionSet / hand-authored FCS).
- **Open Q1:** Confirm the `kenshi-online.mod` rebuild (vs keep existing vs mod-free).
- **Open Q2:** Old `KenshiMP/` tree — archive in place (read-only) or move salvage files into the new
  `src/` layout as we go? (Recommend: leave old tree untouched, copy salvaged files into new tree per module.)
```
