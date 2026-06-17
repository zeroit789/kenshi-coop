# Kenshi-Online Hook System Documentation

**Project:** Kenshi-Online (formerly KenshiMP)  
**Version:** Clean Rebuild (2026-06-03)  
**Total Hooks:** 17 modules (14 active, 3 passive/disabled)

---

## Table of Contents

1. [Hook Architecture](#hook-architecture)
2. [Hook Modules](#hook-modules)
3. [Installation Order](#installation-order)
4. [Critical Hooks](#critical-hooks)
5. [Disabled Hooks](#disabled-hooks)
6. [RVA Offsets](#rva-offsets)

---

## Hook Architecture

### Core Hooking Mechanism

- **Library:** MinHook (detour-based function interception)
- **Special Handling:** MovRaxRsp naked detour for `mov rax, rsp` prologue functions
- **Safety:** SEH (Structured Exception Handling) wrappers on all trampoline calls
- **Deferred Processing:** Combat/zone hooks use lock-free ring buffers to defer heavy work from detour context

### MovRaxRsp Problem

Many Kenshi functions use the `mov rax, rsp` (48 8B C4) prologue:
1. The function captures RSP into RAX on entry
2. It derives RBP from RAX: `lea rbp, [rax-0x??]`
3. All local variables are accessed via `[rbp+offset]`

When MinHook creates a trampoline, the captured RSP is WRONG (points to our C++ stack, not the game caller's stack). This causes:
- Stack corruption
- Sign-extended pointers (0xFFFF... instead of 0x0000...)
- Deterministic crashes on first use

**Solution:** Custom naked detour that:
1. Captures game RSP before the hook function runs
2. Restores it before calling the trampoline
3. Uses global slots (NOT thread-local) because game is single-threaded

**Limitations:** Not reentrant-safe. Rapid-fire calls (300+/sec) or reentrant calls corrupt the global slots → crash. Such hooks must be disabled or use inline hooks instead.

---

## Hook Modules

### 1. **entity_hooks** (Character Creation/Destruction)

**Purpose:** Intercepts character spawning to enable remote player injection

**Functions Hooked:**
- `CharacterSpawn` (RVA 0x581770)
  - **Signature:** `void*(__fastcall*)(void* factory, void* templateData)`
  - **Fires:** During savegame load (130+ calls), NPC zone spawns, character recruitment
  - **Data Captured:** Factory pointer, pre-call request struct (1024B), faction pointers, position, rotation
  - **Actions:**
    - **Loading Mode:** Captures factory + pre-call data + faction voting (first 8 chars), then enables lightweight passthrough
    - **Connected Mode:** NPC Hijack — takes over just-created NPCs for remote players (teleport + rename + disable AI)
    - Registers local player characters, sends `C2S_EntitySpawnReq`
  - **Special:** Uses MovRaxRsp wrapper. Loading passthrough mode minimizes overhead during 130+ load burst.

**Functions NOT Hooked:**
- `CharacterDestroy` — disabled, not needed for current sync model

**Installation:** First hook installed (before AI/Combat). Must succeed for remote player spawning.

**State:**
- Starts in **loading passthrough mode** (ultra-lightweight)
- Switches to **full mode** on `ResumeForNetwork()` after connect
- Reverts to passthrough on `SuspendForDisconnect()`

**Key Features:**
- **Multi-source faction voting** — scans first 8 characters during load, elects player faction via frequency + name match + isPlayerFaction flag
- **Mod template capture** — during loading passthrough, captures "Player 1" through "Player 16" pointers for post-load use
- **NPC Hijack** — primary remote spawn method. Intercepts NPC creation, registers as remote entity, teleports to remote position.
- **Per-player spawn cap** — max 4 remote entities per player to prevent spawn spam

**Diagnostics:**
- `GetTotalCreates()` — lifetime counter
- `GetInPlaceSpawnCount()` — NPC hijack successes
- `GetTimeSinceLastCreate()` — loading detection

---

### 2. **combat_hooks** (Death/KO Events)

**Purpose:** Synchronize character death and knockout state across clients

**Functions Hooked:**
- `CharacterDeath` (RVA 0x7A6200)
  - **Signature:** `void(__fastcall*)(void* character, void* killer)`
  - **Fires:** When character HP reaches 0 or head/chest severed
  - **Actions:** Pushes `{Death, entityId, killerId}` to deferred ring buffer
  - **Server Message:** `C2S_CombatDeath` (sent from OnGameTick, not hook body)

- `CharacterKO` (RVA not listed — found via pattern)
  - **Signature:** `void(__fastcall*)(void* character, void* attacker, int reason)`
  - **Fires:** When character is knocked unconscious (HP < -100 or head trauma)
  - **Actions:** Pushes `{KO, entityId, attackerId, reason, chestHealth}` to deferred ring buffer
  - **Server Message:** `C2S_CombatKO`

**Functions NOT Hooked:**
- `ApplyDamage` (RVA 0x7A33A0) — **DISABLED**. Fires 300+ times/sec during combat. MovRaxRsp global slots corrupt under rapid fire → crash on "attack unprovoked". Health sync uses death/KO hooks + periodic polling instead.

**Deferred Processing:**
- **Ring Buffer:** 256-slot lock-free ring (single producer/single consumer, same thread)
- **ProcessDeferredEvents():** Called from `Core::OnGameTick`, safe context for spdlog/PacketWriter/SendReliable
- **Drop Counter:** Tracks events dropped when ring is full (reports in next OnGameTick)

**Echo Suppression:**
- `SetServerSourcedDeath(bool)` — set by packet handler before applying S2C_CombatDeath
- `SetServerSourcedKO(bool)` — set by packet handler before applying S2C_CombatKO
- Prevents C2S→S2C→C2S infinite echo loops

**Safety:** SEH wrappers on all trampoline calls. Auto-disables on crash via `HookHealth` tracking.

---

### 3. **movement_hooks** (Position/Movement Commands)

**Functions NOT Hooked:**
- `CharacterSetPosition` (RVA found via pattern) — **DISABLED**. Starts with `mov rax, rsp` AND fires 500+ times/frame during movement. Too expensive for HookBypass method. Position sync via polling in `Core::OnGameTick` instead.

- `CharacterMoveTo` (RVA found via pattern) — **DISABLED**. Starts with `mov rax, rsp` AND has a 5th stack parameter. MovRaxRsp detour cannot forward stack params → crash on EVERY call. Movement sync works via position polling.

**Passive Features:**
- AI decision override for remote characters via `ai_hooks::IsRemoteControlled()` check
- Position polling in `Core::OnGameTick` (5Hz throttle, 0.5m change threshold)
- Sends `C2S_PositionUpdate` with position, rotation, animState, moveSpeed, flags

**Installation:** Module installs but hooks nothing. Acts as namespace for remote character position guards.

---

### 4. **world_hooks** (Zone Loading/Unloading)

**Purpose:** Notify server of zone transitions for interest management

**Functions Hooked:**
- `ZoneLoad` (RVA found via pattern)
  - **Signature:** `void(__fastcall*)(void* zoneMgr, int zoneX, int zoneY)`
  - **Fires:** When player moves into a new zone (128x128 world coordinate chunk)
  - **Actions:** Pushes `{Load, zoneX, zoneY}` to deferred ring buffer
  - **Server Message:** `C2S_ZoneRequest`

- `ZoneUnload` (RVA found via pattern)
  - **Signature:** `void(__fastcall*)(void* zoneMgr, int zoneX, int zoneY)`
  - **Fires:** When a previously loaded zone is unloaded (player moved away)
  - **Actions:** Pushes `{Unload, zoneX, zoneY}` to deferred ring buffer, cleans up entity registry + interpolation state for zone

**Deferred Processing:**
- **Ring Buffer:** 32-slot lock-free ring
- **ProcessDeferredZoneEvents():** Called from `Core::OnGameTick`

**Bug Fixes:**
- BUG 2+3 FIX: On zone unload, decrements spawn caps for remote entities before bulk removal
- Cleans up interpolation state to prevent orphaned lerp targets

**Installation:** Installs after entity_hooks.

---

### 5. **squad_hooks** (Squad Management)

**Purpose:** Synchronize squad creation and member additions

**Functions NOT Hooked:**
- `SquadCreate` (RVA 0x480B50) — **DISABLED**. Starts with `mov rax, rsp`. Zone loading creates 100+ NPC squads rapidly → cumulative corruption → crash 10s later. Squad create sync not needed (only SquadAddMember matters for local squad injection).

- `SquadAddMember` (RVA 0x928423) — **DISABLED**. Fires 30-40+ times during zone load as NPC squads are assembled. Each call through trampoline does lookups + packet writes → cumulative corruption. Raw function pointer kept for `AddCharacterToLocalSquad` direct call only.

**Validation:**
- `s_squadAddMemberValidated` — .pdata check confirms function entry point
- `s_squadAddMemberDisabled` — set if validation fails, prevents raw ptr usage

**Raw Function Usage:**
- `AddCharacterToLocalSquad()` calls raw ptr with SEH wrapper
- Exploits game's squad system to make remote characters selectable/controllable

**Installation:** Module installs but hooks nothing. Raw function pointer validated and stored.

---

### 6. **ai_hooks** (AI Controller Management)

**Purpose:** Mark remote characters for AI decision override

**Functions Hooked:**
- `AICreate` (RVA 0x622110)
  - **Signature:** `void*(__fastcall*)(void* character, void* faction)`
  - **Fires:** During character creation, after model/faction setup
  - **Actions:** 
    - ALWAYS calls original (every character needs valid AI controller)
    - Checks if character is remote, calls `MarkRemoteControlled()` if so
  - **Critical:** Returning nullptr here was the root cause of crashes when interacting with remote characters. AI controller must exist for combat/pathfinding/animation/UI selection to work.

- `AIPackages` (RVA found via pattern)
  - **Signature:** `void(__fastcall*)(void* character, void* aiPackage)`
  - **Fires:** When behavior trees/AI packages are loaded for a character
  - **Actions:** ALWAYS lets packages load. Behavior tree structure must exist even for remote characters.

**Remote Control Tracking:**
- `MarkRemoteControlled(void*)` — adds character to `s_remoteControlled` set
- `IsRemoteControlled(void*)` — checked by movement_hooks to block AI move commands
- `UnmarkRemoteControlled(void*)` — removes from set on despawn

**Philosophy:** Keep AI CONTROLLER valid (prevents crashes), override AI DECISIONS (prevent autonomous movement).

**Installation:** Installs after entity_hooks, before movement_hooks.

---

### 7. **inventory_hooks** (Item Pickup/Drop/Trade)

**Purpose:** Synchronize inventory changes across clients

**Functions Hooked:**
- `ItemPickup` (RVA 0x74C8B0)
  - **Signature:** `void(__fastcall*)(void* inventory, void* item, int quantity)`
  - **Fires:** When character picks up item from ground or container
  - **Actions:** Reads inventory owner at inventory+0x28, sends `C2S_ItemPickup`
  - **Data:** entityId, itemTemplateId, quantity

- `ItemDrop` (RVA 0x745DE0)
  - **Signature:** `void(__fastcall*)(void* inventory, void* item)`
  - **Fires:** When character drops item to ground
  - **Actions:** Sends `C2S_ItemDrop` with entityId, itemTemplateId, position

- `BuyItem` (RVA found via pattern)
  - **Signature:** `void(__fastcall*)(void* buyer, void* seller, void* item, int quantity)`
  - **Fires:** When character completes vendor purchase
  - **Actions:** Sends `C2S_TradeRequest` with buyer/seller/item/quantity

**Safety:**
- SEH wrappers using `SafeCall_*` pattern
- `HookHealth` tracking with auto-disable on trampoline crash
- Loading gate via `SetLoading(bool)` to skip during savegame load

**Offset Dependencies:**
- `inventory.owner` offset (0x28) — must be resolved for owner character lookup

**Installation:** Installs after entity_hooks and ai_hooks.

---

### 8. **faction_hooks** (Faction Relations)

**Purpose:** Synchronize faction reputation changes across clients

**Functions Hooked:**
- `FactionRelation` (RVA 0x872E00)
  - **Signature:** `void(__fastcall*)(void* factionA, void* factionB, float relation)`
  - **Fires:** When faction relations change (crime, combat, diplomacy)
  - **Actions:** Reads faction IDs from factionA/B+offset, sends `C2S_FactionRelation`

**Echo Suppression:**
- `SetServerSourced(bool)` — set by packet handler before applying S2C_FactionRelation
- Prevents client→server→broadcast→client echo loops

**Loading Gate:**
- `SetLoading(bool)` — skips during savegame load (factions restored from save)

**Offset Dependencies:**
- `faction.id` offset — must be resolved for faction ID lookup

**Installation:** Installs after entity_hooks.

---

### 9. **building_hooks** (Building Placement/Construction)

**Purpose:** Synchronize building construction across clients

**Functions Hooked:**
- `BuildingPlace` (pattern-based, signature unverified)
  - **Signature:** `void(__fastcall*)(void* world, void* building, float x, float y, float z)`
  - **Fires:** When player places building blueprint
  - **Actions:** Extracts templateId from building+0x28 GameData backptr, sends `C2S_BuildRequest`
  - **Crash Protection:** Max 10 crashes → auto-disable (wrong function matched by scanner)

- `BuildingDestroyed` (pattern-based)
  - **Fires:** When building is destroyed (combat, decay)
  - **Actions:** Sends `C2S_EntityDespawnReq` with reason=destroyed

- `BuildingDismantle` (pattern-based)
  - **Fires:** When player dismantles building
  - **Actions:** Sends `C2S_BuildDismantle`

- `BuildingConstruct` (pattern-based)
  - **Fires:** During construction progress (every worker tick)
  - **Actions:** Logged for diagnostics, no network traffic (too frequent)

- `BuildingRepair` (RVA 0x5C9E70)
  - **Fires:** When building is repaired
  - **Actions:** Logged for diagnostics, no network traffic

**Crash Recovery:**
- Each hook has independent crash counter (MAX_CRASHES = 10)
- After 60s with no crashes, counter resets to 0
- Allows graceful degradation if scanner matched wrong functions

**Installation:** Installs after entity_hooks. All hooks optional (graceful failure).

---

### 10. **save_hooks** (Save/Load Events)

**Purpose:** Detect savegame loading to gate entity hooks

**Functions NOT Hooked:**
- `SaveGame` (RVA 0x373F00) — **DISABLED** (signature unverified)
- `LoadGame` (RVA 0x373F00) — **DISABLED** (signature unverified)

**Current Behavior:**
- Pass-through mode — saves and loads proceed unmodified
- Players keep local save files, multiplayer state is server-side snapshot
- `IsLoading()` always returns false

**Future:**
- If enabled, would set `s_loading` flag during load to gate entity/combat hooks
- Would allow save-on-disconnect (local checkpoint)

**Installation:** Module installs but does nothing. Placeholder for future work.

---

### 11. **time_hooks** (Time Management)

**Purpose:** Synchronize game time and speed across clients

**Functions Hooked:**
- `TimeUpdate` (RVA 0x214B50)
  - **Signature:** `void(__fastcall*)(void* timeManager, float deltaTime)`
  - **Fires:** Every frame on GOG builds, NEVER on Steam builds
  - **Actions:**
    - Captures time manager pointer on first call
    - Clients override deltaTime with server-controlled speed
    - Triggers `Core::OnGameTick(deltaTime)`

**Steam Build Issue:**
- TimeUpdate at RVA 0x214B50 is NEVER called by Steam builds
- render_hooks `Present` drives `OnGameTick` instead
- 4ms dedup guard in `OnGameTick` prevents double-processing if TimeUpdate starts firing

**Time Control:**
- `SetServerTime(float timeOfDay, float gameSpeed)` — writes to timeManager+0x08/+0x10
- `GetTimeOfDay()` — reads timeManager+0x08
- `GetGameSpeed()` — reads timeManager+0x10
- `WriteTimeOfDay(float)` — direct memory write

**Installation:** Installs hook but doesn't set TimeHookActive flag (Present is primary driver).

---

### 12. **game_tick_hooks** (Frame Update)

**Purpose:** Trigger multiplayer tick processing from engine frame update

**Functions Hooked:**
- `GameFrameUpdate` (RVA found via pattern)
  - **Signature:** `void(__fastcall*)(void* rcx, void* rdx)`
  - **Fires:** Every frame (150fps typical)
  - **Actions:** Calls original via MovRaxRsp trampoline wrapper
  - **Special:** Uses OutputDebugStringA only (NO spdlog in detour context)

**MovRaxRsp Safety:**
- Function starts with `mov rax, rsp` (48 8B C4)
- HookManager automatically applies MovRaxRsp naked detour
- Trampoline wrapper restores correct RAX before calling original

**Spawn Diagnostics:**
- Every 3000 ticks (~20s): logs pending spawn count + inPlace spawn count
- NO spawn fallback here (moved to `Core::HandleSpawnQueue` 10s timeout)

**Deferred Probes:**
- AnimClass probing DISABLED (flooded log with failures)
- PlayerControlled probing DISABLED (CharacterIterator failures)

**Installation:** Installs after time_hooks, before render_hooks.

---

### 13. **render_hooks** (D3D11 Present + WndProc)

**Purpose:** Drive multiplayer tick from Present, intercept input via WndProc

**Functions Hooked:**
- `IDXGISwapChain::Present` (VTable index 8)
  - **Fires:** Every frame after GPU render completes
  - **Actions:**
    - Calls `Core::OnGameTick()` (primary driver on Steam builds)
    - Renders native HUD overlay (MyGUI)
    - Processes network events (ENet)

- `WndProc` (Win32 window message handler)
  - **Fires:** On all keyboard/mouse input
  - **Keybinds:**
    - F1 — Toggle native menu (connection UI)
    - Tab — Toggle player list
    - Enter — Toggle chat input
    - Escape — Close chat/menu
    - Insert — Toggle debug log panel
    - Backtick (~) — Toggle debug info
  - **Modal Input Gate:** Consumes all input when chat or menu is active (prevents double-typing + game actions)

**Spawn Queue Processing:**
- WM_KMP_SPAWN message DISABLED — ProcessSpawnQueue() consumed queue before NPC hijack could use it
- In-place replay (entity_hooks) is the ONLY safe spawn mechanism

**Startup Detection:**
- Records first Present timestamp
- Blocks native menu for first 15s (logo/splash screen, MyGUI not loaded yet)

**Installation:** Installs after game_tick_hooks. Hooks both Present and WndProc.

---

### 14. **input_hooks** (Keybind Processing)

**Purpose:** Process additional keybinds beyond WndProc

**Functions Hooked:** None

**Current Behavior:**
- All input is handled via WndProc hook in render_hooks
- This module is a placeholder for future OIS (Object Input System) hooks

**Keybinds Summary:**
- Tab — Toggle player list
- Enter — Toggle chat
- F1 — Toggle connection UI
- Escape — Close overlays
- Backtick (~) — Toggle debug overlay
- Insert — Toggle debug log panel

**Installation:** Module installs but does nothing. Future expansion point.

---

### 15. **resource_hooks** (Ogre Resource Loading)

**Purpose:** Detect loading completion via Ogre MeshManager/TextureManager VTable hooks

**Functions Hooked:** None (discovery failed)

**Architecture:**
- Ogre3D 1.x resource loading goes through:
  - `Ogre::MeshManager::load(name, group, ...)`
  - `Ogre::TextureManager::load(name, group, ...)`
  - `Ogre::MaterialManager::load(name, group, ...)`
- These are virtual methods on singleton manager objects

**Discovery Steps:**
1. Check if OgreMain.dll is loaded (dynamic linking)
2. If not, scan kenshi_x64.exe .rdata for "MeshManager" string
3. Find xrefs in .text to locate singleton accessor
4. Read VTable pointer from singleton
5. Install VTable hooks at correct indices

**Current State:**
- Discovery NOT implemented (requires runtime analysis to find VTable indices)
- LoadingOrchestrator gracefully degrades to burst-detection timing

**Installation:** Module installs but hooks nothing. Future optimization.

---

### 16. **char_tracker_hooks** (Character Animation Tracking)

**Purpose:** Track active characters via AnimationClassHuman updates

**Functions Hooked:**
- `AnimationClassHuman::Update` (inline hook at pattern-matched offset)
  - **Fires:** 300+ times/sec for active characters
  - **Actions:** Reads character pointer from animClass+0x2D8, pushes to ring buffer
  - **Deferred:** `ProcessDeferredDiscovery()` called from OnGameTick does name lookup + map insertion

**Inline Hook Implementation:**
- Custom trampoline (NOT MinHook — too performance-critical)
- Saves all registers (RAX-R15)
- Calls `SEH_OnCharUpdate()` with RBX (AnimationClassHuman*)
- Restores registers + executes original 14 bytes + jmp back

**Ring Buffer:**
- 128-slot lock-free ring (single producer/single consumer)
- NO heap allocation, NO mutex, NO spdlog in hook body
- Fast path: already tracked → atomic timestamp update (try_lock, skip on fail)

**Tracked Character Map:**
- `std::unordered_map<void*, TrackedChar>` (character pointer → name/faction/animClass)
- `FindByName(std::string)` — lookup by character name
- `FindByPtr(void*)` — lookup by character pointer
- `GetLocalPlayerAnimClass()` — primary player AnimClass for probing

**Installation:** Optional optimization. Installs after ai_hooks if pattern matches.

---

### 17. **squad_spawn_hooks** (Squad Spawn Bypass)

**Purpose:** Force NPC creation when spawn queue has pending remote players

**Functions Hooked:**
- `SquadSpawnBypass` (RVA found via research mod RE)
  - **Signature:** `void(__fastcall*)(void* context, void* activePlatoon)`
  - **Fires:** During squad processing (zone load, NPC AI squad assembly)
  - **Actions:**
    - Checks if `spawn_manager` has pending network spawns
    - If yes AND activePlatoon skip checks are set (check3==0, check1==true):
      - Saves original check values
      - Flips checks to false (forces spawn)
      - entity_hooks NPC Hijack grabs the resulting character
      - Restores original check values

**Research Mod Analysis:**
- Offsets from GOG research mod RE (activePlatoon struct):
  - +0xF0 — skip check 1 (bool)
  - +0x58 — skip check 2 (bool)
  - +0x250 — skip check 3 (void*, must be 0 for bypass)
  - +0x78 — squad pointer
  - +0xA0 — leader pointer

**Spawn Flow:**
1. Server sends `S2C_EntitySpawn` → queued to `spawn_manager`
2. Zone AI processes squads, calls SquadSpawnBypass
3. Hook detects pending spawn, flips checks, forces NPC creation
4. entity_hooks `Hook_CharacterCreate` intercepts NPC, hijacks it for remote player

**Safety:**
- SEH wrappers on all struct reads/writes
- Saves/restores original check values (don't permanently alter game state)
- NO spdlog in hook body (OutputDebugStringA only)

**Diagnostics:**
- `GetSuccessCount()` — count of successful bypasses
- `s_bypassCount` — total bypass attempts

**Installation:** Installs after entity_hooks. Required for NPC Hijack remote spawn method.

---

## Installation Order

Hooks are installed in `Core::Initialize()` via `InstallHooks()`:

```cpp
1. entity_hooks::Install()        // CRITICAL: Must succeed for remote spawn
2. ai_hooks::Install()             // Before movement (remote AI override)
3. combat_hooks::Install()         // After entity (needs registry)
4. movement_hooks::Install()       // After AI (checks IsRemoteControlled)
5. world_hooks::Install()          // After entity (zone entity cleanup)
6. squad_hooks::Install()          // After entity (squad member lookups)
7. inventory_hooks::Install()      // After entity (inventory owner lookups)
8. faction_hooks::Install()        // After entity (faction ID lookups)
9. building_hooks::Install()       // After entity (building entity registration)
10. save_hooks::Install()          // Anytime (currently no-op)
11. time_hooks::Install()          // Before game_tick (tick driver fallback)
12. game_tick_hooks::Install()     // Before render (tick diagnostics)
13. render_hooks::Install()        // Last (primary tick driver + UI)
14. input_hooks::Install()         // After render (WndProc installed)
15. resource_hooks::Install()      // Optional (Ogre discovery)
16. char_tracker_hooks::Install()  // Optional (inline hook)
17. squad_spawn_hooks::Install()   // After entity (spawn bypass for NPC hijack)
```

**Critical Path:** entity_hooks → ai_hooks → combat_hooks → render_hooks

If entity_hooks fails, remote spawning is impossible. All other hooks are optional (graceful degradation).

---

## Critical Hooks

### Must Succeed for Multiplayer

1. **entity_hooks** — Remote player spawning (NPC Hijack method)
2. **render_hooks** — Tick driver (Present) + input (WndProc)

### High Priority

3. **combat_hooks** — Cross-player death/KO sync
4. **ai_hooks** — Remote character AI override
5. **world_hooks** — Zone interest management + entity cleanup

### Medium Priority

6. **inventory_hooks** — Item sync
7. **faction_hooks** — Reputation sync
8. **squad_spawn_hooks** — NPC creation force for remote spawn

### Optional

9. **movement_hooks** — (no hooks, position polling only)
10. **squad_hooks** — (no hooks, raw ptr for injection only)
11. **building_hooks** — Building sync (pattern-based, crash-prone)
12. **char_tracker_hooks** — Character tracking optimization
13. **time_hooks** — Time sync (GOG builds only)
14. **game_tick_hooks** — Tick diagnostics
15. **save_hooks** — (no-op, pass-through)
16. **input_hooks** — (no hooks, WndProc handles all)
17. **resource_hooks** — (no hooks, discovery failed)

---

## Disabled Hooks

### Intentionally Disabled (Crash Risk)

#### ApplyDamage (combat_hooks)
- **RVA:** 0x7A33A0
- **Problem:** Fires 300+ times/sec during combat. MovRaxRsp global slots corrupt under rapid fire.
- **Crash:** Deterministic crash on "attack unprovoked" (first combat after hook install).
- **Alternative:** Health sync via death/KO hooks + periodic polling in OnGameTick.

#### CharacterSetPosition (movement_hooks)
- **Problem:** Starts with `mov rax, rsp`, fires 500+ times/frame during movement. HookBypass disable-call-reenable is too expensive.
- **Alternative:** Position polling in OnGameTick (5Hz, 0.5m threshold).

#### CharacterMoveTo (movement_hooks)
- **Problem:** Starts with `mov rax, rsp` AND has 5th stack parameter. MovRaxRsp detour cannot forward stack params.
- **Crash:** Every click-to-move action crashes immediately.
- **Alternative:** Position polling in OnGameTick.

#### SquadCreate (squad_hooks)
- **RVA:** 0x480B50
- **Problem:** Starts with `mov rax, rsp`. Zone loading creates 100+ NPC squads rapidly → cumulative corruption.
- **Crash:** Silent crash ~10s after zone load starts.
- **Alternative:** Squad sync not needed (only SquadAddMember matters for local squad injection).

#### SquadAddMember (squad_hooks)
- **RVA:** 0x928423
- **Problem:** Fires 30-40+ times during zone load. Each call does entity lookups + packet writes → cumulative corruption.
- **Crash:** Zone load stutters, crash after ~10s.
- **Alternative:** Raw function pointer kept for direct calls in `AddCharacterToLocalSquad()` only.

### Hooks That Never Fire (Steam Build)

#### TimeUpdate (time_hooks)
- **RVA:** 0x214B50
- **Problem:** Function is never called by Steam builds (GOG builds call it every frame).
- **Alternative:** render_hooks `Present` drives OnGameTick on Steam builds.
- **Safety:** 4ms dedup guard in OnGameTick prevents double-processing if TimeUpdate starts firing.

---

## RVA Offsets

**Steam Build (kenshi_x64.exe, PE base 0x140000000)**

### Verified Functions (confirmed in .pdata)

| Function | RVA | Full Address | Pattern | Notes |
|----------|-----|--------------|---------|-------|
| CharacterSpawn | 0x581770 | 0x140581770 | mov rax, rsp | Entity creation |
| CharacterDeath | 0x7A6200 | 0x1407A6200 | — | Death event |
| ApplyDamage | 0x7A33A0 | 0x1407A33A0 | mov rax, rsp | **DISABLED** |
| StartAttack | 0x7B2A20 | 0x1407B2A20 | — | Attack initiation |
| HealthUpdate | 0x86B2B0 | 0x14086B2B0 | — | Health calculation |
| AICreate | 0x622110 | 0x140622110 | — | AI controller init |
| BuildingRepair | 0x5C9E70 | 0x1405C9E70 | — | Building repair |
| SquadCreate | 0x480B50 | 0x140480B50 | mov rax, rsp | **DISABLED** |
| SquadAddMember | 0x928423 | 0x140928423 | — | **DISABLED** |
| TimeUpdate | 0x214B50 | 0x140214B50 | mov rax, rsp | **Never fires** |
| LoadGame | 0x373F00 | 0x140373F00 | — | **Not hooked** |
| ItemPickup | 0x74C8B0 | 0x14074C8B0 | mov rax, rsp | Inventory sync |
| ItemDrop | 0x745DE0 | 0x140745DE0 | mov rax, rsp | Inventory sync |
| FactionRelation | 0x872E00 | 0x140872E00 | — | Faction rep |

### Factory Functions (NOT hooked, called directly)

| Function | RVA | Full Address | Notes |
|----------|-----|--------------|-------|
| FactoryCreate | 0x583400 | 0x140583400 | High-level dispatcher, builds request struct |
| CreateRandomChar | 0x5836E0 | 0x1405836E0 | Creates random NPC (fallback) |

### Pattern-Based (unverified, may crash)

- ZoneLoad — pattern-based, unknown RVA
- ZoneUnload — pattern-based, unknown RVA
- BuildingPlace — pattern-based, **crash-prone**
- BuildingDestroyed — pattern-based, **crash-prone**
- BuildingDismantle — pattern-based, **crash-prone**
- BuildingConstruct — pattern-based, **crash-prone**
- CharacterSetPosition — pattern-based, **DISABLED**
- CharacterMoveTo — pattern-based, **DISABLED**
- GameFrameUpdate — pattern-based, confirmed via .pdata

### Corrected Offsets (from 07-steam-re-findings.md)

**7 functions had WRONG addresses in old offset file:**

| Function | CORRECT (Steam) | OLD (wrong) | Delta |
|----------|-----------------|-------------|-------|
| CharacterDeath | 0x7A6200 | 0x80E6F0 | +0x68AF0 |
| TimeUpdate | 0x214B50 | 0x563530 | -0x34E9E0 |
| LoadGame | 0x373F00 | 0x56F320 | -0x1FB420 |
| ItemPickup | 0x74C8B0 | 0x8ED010 | -0x1A0760 |
| ItemDrop | 0x745DE0 | 0x8ED1D0 | -0x1A73F0 |
| FactionRelation | 0x872E00 | 0x90BC10 | -0x98E10 |
| SquadCreate | 0x480B50 | 0x928470 | -0x4A7920 |

**All corrected offsets verified via .pdata (PE exception table).**

---

## Struct Offsets

**Dynamically Detected:**
- `character+0x48` — position (Vec3)
- `character+0x58` — rotation (Quat)
- `character+0x10` — faction pointer
- `inventory+0x28` — owner character pointer
- `animClass+0x2D8` — character pointer
- `activePlatoon+0xF0` — spawn skip check 1
- `activePlatoon+0x58` — spawn skip check 2
- `activePlatoon+0x250` — spawn skip check 3
- `timeManager+0x08` — time of day
- `timeManager+0x10` — game speed

**From offsets.json (loaded at runtime):**
- `character.name` — Kenshi string struct
- `character.health` — per-limb health array
- `character.alive` — bool flag
- `faction.id` — uint32
- `faction.isPlayerFaction` — bool flag

**Request Struct (entity_hooks pre-call capture):**
- Size: 1024 bytes
- Position offset: detected at runtime (first match)
- GameData offset: detected at runtime (first match)

---

## Hook Health Monitoring

All hooks use `HookHealth` struct for crash tracking:

```cpp
struct HookHealth {
    std::string name;
    std::atomic<int> failCount{0};
    std::atomic<bool> trampolineFailed{false};
};
```

When a trampoline crashes:
1. SEH catches the exception
2. `trampolineFailed` set to true
3. `failCount` incremented
4. Hook skips trampoline on next call
5. Error logged: "trampoline CRASHED — hook auto-disabled"

**Auto-Recovery (building_hooks only):**
- After 60s with no crashes, counter resets to 0
- Allows transient glitches without permanent disable

---

## Deferred Processing Pattern

**Problem:** MovRaxRsp detours cannot safely call:
- spdlog (heap allocation)
- PacketWriter (has destructor)
- SendReliable (ENet mutex)
- CharacterAccessor (game memory reads)

**Solution:** Lock-free ring buffer + deferred processing

```cpp
// Hook body (detour context, NO heap/spdlog/mutex)
static void __fastcall Hook_Combat(void* character, void* killer) {
    s_origFunc(character, killer);  // Call original first
    if (s_serverSourced) return;    // Echo suppression
    
    DeferredEvent evt{};
    evt.entityId = Core::Get().GetEntityRegistry().GetNetId(character);  // Fast lookup
    PushEvent(evt);  // Lock-free ring push
}

// ProcessDeferred() called from Core::OnGameTick (safe context)
void ProcessDeferredEvents() {
    DeferredEvent evt;
    while (PopEvent(evt)) {
        spdlog::info("Event: entity {}", evt.entityId);  // SAFE: OnGameTick context
        PacketWriter writer;  // SAFE: no detour
        writer.WriteHeader(...);
        Core::Get().GetClient().SendReliable(writer.Data(), writer.Size());  // SAFE: no nested detour
    }
}
```

**Modules Using Deferred Pattern:**
- combat_hooks (256-slot ring)
- world_hooks (32-slot ring)
- char_tracker_hooks (128-slot ring)

**Benefits:**
- Zero heap allocation in detour
- Zero mutex contention in detour
- Zero spdlog formatting in detour
- Drop counter tracks events lost when ring is full

---

## Conclusion

The Kenshi-Online hook system uses a layered architecture:

1. **MinHook detours** for standard functions
2. **MovRaxRsp naked detours** for `mov rax, rsp` prologue functions
3. **Inline hooks** for ultra-high-frequency functions (char_tracker)
4. **Deferred ring buffers** for safe processing from detour context
5. **SEH wrappers** on all trampoline calls
6. **Crash recovery** with auto-disable + optional reset

**Key Insight:** Many Kenshi functions use `mov rax, rsp` prologue and are NOT safely hookable via standard detours. The MovRaxRsp fix enables hooking BUT introduces reentrancy + rapid-fire corruption risks. Critical hooks (entity, combat, world) use deferred processing to minimize work in detour context.

**Reliability:** entity_hooks + render_hooks MUST succeed. All other hooks are optional. The system degrades gracefully when hooks fail: fewer features work, but the game doesn't crash.

---

*Generated 2026-06-04 by reverse engineering rebuild/src/hooks/*.cpp*
