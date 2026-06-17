# Remote Player Reliability on Steam — Design

**Date:** 2026-04-19
**Status:** Draft (rev 2 — corrected after codebase verification)
**Target:** KenshiMP (Kenshi 16-player coop mod)

## Problem

Remote players do not work correctly on the Steam build of Kenshi. The cause is two interlocking failure classes plus an architectural inconsistency between the three remote-character spawn paths.

**Steam pointer failures (P):**
- Hardcoded RVAs `PlayerBase = 0x01AC8A90` and `GameWorld = 0x02133040` are GOG 1.0.68 only. On Steam, those `.data` slots contain garbage or valid-looking but wrong pointers.
- `CharacterIterator` reads `GameWorld + 0x0888` ("lektor" container). Its `format1/format2` detection misinterprets the underlying `std::vector`-style layout (`_First` at +0x00, `_Last` at +0x08) — it treats `_Last` as a count, producing ASCII noise like `faction=0xFA3000007FF71F5A`, `char="race"`.
- `activePlatoon` offset `char + 0x658` is GOG-specific. The range-scan fallback at `char + 0x600..0x780` (`squad_hooks.cpp::ResolveActivePlatoon`) returns inconsistent results on Steam. Comment at `core.cpp:2733`: *"Squad injection DISABLED — activePlatoon resolution picks up code-section pointers on Steam builds → WRITE AV at game+0xE85340 → cascading crash."*

**Three spawn paths, disparate post-spawn setup (L):**

The codebase has three distinct paths that land a remote character into the world. They run different subsets of the post-spawn setup, and the two "fallback" paths have deliberately disabled steps that are known-good on the primary path:

| Path | Where | Post-spawn calls | Notes |
|---|---|---|---|
| **A. Mod-link** (preferred) | `core.cpp:1605-1624` (OnGameLoaded deferred), `core.cpp:3271-3340` (HandleSpawnQueue match) | `MarkRemoteControlled` → `SEH_FallbackPostSpawnSetup` → `WriteGameDataNameForModLink` → `SEH_AllyModFaction` | Links to pre-loaded "Player 1..16" mod characters. Depends on `FindModCharacterBySlot` → `CharacterIterator`. |
| **B. DIRECT SPAWN via factory** (secondary) | `sync_orchestrator.cpp:740-788` | `SEH_FixUpFaction_Core` → `OnRemoteCharacterSpawned` → `MarkRemoteControlled` → `AddCharacterToLocalSquad` → `WritePlayerControlled(true)` → `ScheduleDeferredAnimClassProbe` | Full setup. Works on GOG; crashes on Steam via activePlatoon. |
| **C. FallbackPostSpawn** (tertiary) | `core.cpp:2697-2761` (`SEH_FallbackPostSpawnSetup`) | `CharacterAccessor::WritePosition` → `MarkRemoteControlled` → `ScheduleDeferredAnimClassProbe` | **Squad injection DISABLED** (`core.cpp:2733-2735`). **`WritePlayerControlled` DISABLED** (`core.cpp:2737-2738`). Both disabled "not needed for visibility, and can crash if the character's internal state isn't fully initialized." |

The disabled steps in Path C are the root reason remote characters on Steam appear but behave wrong: AI keeps driving them (no `WritePlayerControlled(true)`), they never join the host's squad panel (no `AddCharacterToLocalSquad`). The steps weren't disabled because they're bad — they were disabled because Steam activePlatoon resolution made them crash-prone. Fix the pointers (P3) and the disabled steps become safe to re-enable.

**Other L issues:**
- `CallFactoryDirect` in `HandleSpawnQueue` uses a factory input struct captured at loading-time; embedded pointers (gameData, faction template) can go stale as natural NPC spawns progress.
- `SEH_FixUpFaction_Core` already has a validated fallback chain (`GetFallbackFaction()` → `GetEarlyPlayerFaction()`) where `GetFallbackFaction()` calls `SEH_IsFactionValid` before returning. But if all sources fail, the spawned character is kept around with potentially stale faction → next frame crash at `game + 0x927E94`. Needs a hard-fail-and-despawn path, not silent continuation.
- `ApplyRemotePositionsDirect` (`core.cpp:2346` — note: not `ApplyRemotePositions`, which is a legacy double-buffered variant) tracks `s_noObjCount` when a remote entity has no game object, but only logs — does not requeue the spawn.
- `CharacterAccessor::WritePosition` Method 3 (`game_character.cpp:486-505`) is documented as "may be overwritten by the physics engine next frame, but for remote characters that are continuously updated it's acceptable." Visible drift happens when it's the only method available; in practice this occurs on Steam when the physics chain offsets aren't probed before the first position update arrives.

## Goals

- Steam users can host/join; remote players spawn visibly within seconds of join.
- Remote characters follow network-applied positions without AI interference.
- Remote characters appear in the host's squad panel when spawn succeeds (parity with GOG).
- No faction-pointer crashes at `game + 0x927E94` under sustained play.
- `CharacterIterator`-equivalent enumeration returns consistent, valid data on both GOG and Steam.
- Retain the GOG 1.0.68 code path (no regressions).

## Non-Goals

- No Markov-chain movement prediction / interpolation (baseline reliability first).
- No scanner rewrite — `KenshiMP.Scanner` continues to find its 39/39 targets.
- No change to wire protocol / message schemas.
- No building-spawning work.

## Architecture

Three discovery/enumeration fixes (P1–P3) unblock a set of spawn-path fixes (L1–L5). Each lands in an isolated module and is independently testable.

```
┌─────────────────────────────────────────────────────────────────┐
│  DISCOVERY LAYER (unlocks the logic fixes below)                │
│                                                                 │
│  P1 GameWorld hook-capture (game_tick_hooks)                    │
│  P2 Shadow character list (game_character + entity_hooks)       │
│  P3 ResolveActivePlatoon unification (squad_hooks)              │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│  SPAWN-PATH LOGIC                                               │
│                                                                 │
│  L1 Fresh factory input struct per spawn attempt                │
│  L2 Faction fixup: hard-fail + despawn on total source failure  │
│  L3 Re-enable Path C's disabled steps (unblocked by P3)         │
│      → unifies post-spawn behavior across Paths A/B/C           │
│  L4 ApplyRemotePositionsDirect: requeue when game object missing│
│  L5 WritePosition Method 3: gate behind explicit opt-in         │
└─────────────────────────────────────────────────────────────────┘
```

## Components

### P1. GameWorld hook-capture

**Location:** `KenshiMP.Core/hooks/game_tick_hooks.cpp`, `game_tick_hooks.h`; bridged into `KenshiMP.Core/game/game_world.cpp`.

`Hook_GameFrameUpdate(void* rcx, void* rdx)` already runs every frame; `rcx` is the implicit `this` of the game-loop dispatcher, empirically GameWorld. Mirror `time_hooks.cpp`'s `s_timeManager` pattern:

1. On first call with non-null `rcx`, validate: heap-range, outside-module, vtable-in-`.text`. Additional check: vtable RTTI name matches `GameWorld` family (reuse validator from `patterns.cpp::validateGameWorld`).
2. On success, store into `s_capturedGameWorld` (one-shot atomic write).
3. Expose `GetCapturedGameWorld()` getter; `GameWorldAccessor` consumers prefer it over the hardcoded RVA.
4. If validation fails, leave `s_capturedGameWorld = nullptr`, retry on subsequent frames until game-loaded gate opens.

**PlayerBase:** no existing hook receives it as a parameter. Keep the existing `RetryGlobalDiscovery` post-load path, widening validation with the same module-range exclusion already applied to `GameWorld`. If still unresolved after game-load, derive PlayerBase from a known player character's backlink chain (via shadow list — P2).

### P2. Character shadow list (hook-populated, no lektor read)

**Location:** `KenshiMP.Core/game/game_character.cpp` (`CharacterIterator` internals), with hook-maintenance in `KenshiMP.Core/hooks/entity_hooks.cpp`.

**Key insight:** `Hook_CharacterCreate` is already installed in *LOADING PASSTHROUGH mode* at DLL init (`entity_hooks.cpp:1197`). This means it captures every character creation from the moment the save begins loading — before any game memory layout matters. We do NOT need to read the lektor container at all; we already observe every character as it's born.

Agent 2's hypothesis that the lektor has `std::vector` layout (`_First`/`_Last`/`_End`) was unverified. The existing iterator code actually reads 16-byte `{count, ptr}` / `{ptr, count}` layouts, neither of which matches std::vector. Rather than risk another wrong guess, we bypass the lektor entirely.

**Structure:**
```
static std::mutex                 s_shadowMutex;
static std::vector<uintptr_t>     s_shadowCharacters;
```

**Population:**
- `Hook_CharacterCreate` (existing, already in passthrough during loading): on success, append the new character pointer to `s_shadowCharacters` under `s_shadowMutex`.
- `Hook_CharacterDestroy`: remove the pointer (swap-with-last + pop_back for O(1)).
- No bootstrap read of GameWorld's lektor. The hook covers every character from the moment it's created; there are no "pre-existing" characters to miss because DLL load precedes any save-load.

**CharacterIterator behavior:**
- `Reset()`: take a snapshot of `s_shadowCharacters` under mutex into a per-iterator copy. Always uses shadow list; legacy lektor path removed.
- `HasNext()`, `Next()`: operate on the snapshot (no mid-iteration live-memory reads).

**Correctness:**
- Completeness: every `CharacterCreate` call adds to shadow; every `CharacterDestroy` call removes. Matches the game's own lifecycle.
- GOG parity: on GOG, shadow list contains the same characters the lektor would have exposed. `FindModCharacterBySlot` (GOG's primary user of `CharacterIterator`) continues to work.
- No lektor layout hypothesis to be wrong about.
- Sanity sweep (every 500 ticks): iterate shadow snapshot, validate each pointer's vtable; purge any that went invalid. Guards against missed destroys.

### P3. `ResolveActivePlatoon` unification

**Location:** `KenshiMP.Core/hooks/squad_hooks.cpp::ResolveActivePlatoon`.

Replace the range-scan-with-vtable-validation with a thin delegator to the proven three-stage discovery already used (successfully on Steam) in `core.cpp::TryDiscoverSquadAddMemberFromVTable`:

1. Try `character + 0x658 → activePlatoon → vtable+0x10` (GOG-first, cheap).
2. Try `GetSquadPtr(character) → squad + 0x1D8 → activePlatoon → vtable+0x10`.
3. Try `GetSquadPtr(character) → vtable+0x10` directly.

On success, validate the resulting struct by reading known-offset fields (`+0x78=squad*`, `+0xA0=leader`) and confirming each is a heap pointer with a vtable. Cache the resolved offset keyed on the character's vtable (not per-character — different character classes can have different platoon offsets, but characters sharing a vtable share the offset).

P3 is the gate for L3: once activePlatoon resolves reliably on Steam, the two disabled steps in `SEH_FallbackPostSpawnSetup` become safe.

### L1. Fresh factory input struct per spawn attempt

**Location:** `KenshiMP.Core/hooks/entity_hooks.cpp::CallFactoryDirect`; callers in `KenshiMP.Core/core.cpp::HandleSpawnQueue`.

Root cause: the factory input struct cached during `SEH_LoadingCapture` (first 2 `CharacterCreate` calls) embeds pointers that were valid at load time. Later natural NPC spawns can invalidate some of those embedded pointers.

**Fix:** on each spawn attempt, build a fresh input struct from live sources:
- `gameData`: re-resolve from the most recent known-valid character's `+0x40` field, pulled from `s_shadowCharacters` (requires P2).
- `factionTemplate`: pick from the local primary player character's faction, read live.
- Static-hand / object-manager: read from `s_capturedGameWorld` fields at runtime (requires P1).

The factory function pointer itself (trampoline into `CharacterHuman` factory at `game + 0xDD00`) is safe to cache from loading — its address doesn't move.

**Dependency:** L1 requires P1 and P2 populated. Guard with a `gameLoaded && shadowPopulated && gameWorldCaptured` check; if any fails, skip spawn attempt this tick and retry.

### L2. Faction fixup: hard-fail + despawn on total source failure

**Location:** `KenshiMP.Core/core.cpp::SEH_FixUpFaction_Core` (existing), callers at `core.cpp:3342` (HandleSpawnQueue) and Path A sites.

The existing chain is actually validated: `GetFallbackFaction()` calls `SEH_IsFactionValid` before returning and returns 0 on stale (`entity_hooks.cpp:1412-1418`); `SEH_FixUpFaction_Core` then falls back to `GetEarlyPlayerFaction()`. The real bug is what happens when **both** sources return 0 or invalid: the function returns but the spawned character has been kept in the registry with a potentially-stale faction pointer → next-frame crash at `game + 0x927E94`.

**Fix:** treat "all faction sources failed" as a hard spawn failure:
1. Extend the validation chain with one additional source: sample up to 3 live characters from `s_shadowCharacters` (requires P2), take their factions, pick the first that validates.
2. If the chain still fails: return `false` from `SEH_FixUpFaction_Core` and have the caller despawn the just-created character (teleport underground + delete from registry), send `C2S_SpawnFailed` to the server so it can retry, and log `error` with entity id.
3. Never leave a character in the registry with an unvalidated faction.

### L3. Re-enable disabled steps in `SEH_FallbackPostSpawnSetup` (unified post-spawn)

**Location:** `KenshiMP.Core/core.cpp::SEH_FallbackPostSpawnSetup` (lines 2697-2761).

The two disabled steps in Path C (squad injection, `WritePlayerControlled`) were disabled for **crash safety** — the comments explicitly say so. With P3 making `activePlatoon` resolution reliable on Steam, and P2's shadow list letting us sanity-check the character's initialization state before the write, the crash conditions no longer apply.

**Fix:**

1. **Re-enable squad injection** (currently `core.cpp:2733-2735`): call `squad_hooks::AddCharacterToLocalSquad(character)` inside a `__try` block. Prerequisite: `ResolveActivePlatoon` (P3) must succeed for this character's vtable class. If it fails for this vtable, skip squad injection with a `warn` log rather than aborting post-spawn — squad is cosmetic (panel visibility); the character still works without it.

2. **Re-enable `WritePlayerControlled`** (currently `core.cpp:2737-2738`): call `game::WritePlayerControlled(character, true)` inside a `__try` block. No per-character probe gating — the DIRECT SPAWN path (`sync_orchestrator.cpp:782`) demonstrates this call is safe immediately after factory return and is what makes remote characters stop fighting network positions. The original "internal state not fully initialized" concern is addressed by the SEH wrapper: if the character truly isn't initialized, `__except` catches the AV and we log + continue rather than crash.

3. **Unify with Path B (DIRECT SPAWN)**: both `sync_orchestrator.cpp:780-783` and `SEH_FallbackPostSpawnSetup` should call the same post-spawn helper. Extract a `ApplyStandardPostSpawn(character, netId, owner, position)` inline helper in `core.cpp` that both paths invoke. Keeps behavior identical across all spawn outcomes; single place to fix future issues.

### L4. `ApplyRemotePositionsDirect` requeue + diagnostics

**Location:** `KenshiMP.Core/core.cpp::ApplyRemotePositionsDirect` (the function called every tick at line 2346). Note: the legacy `ApplyRemotePositions` (double-buffered, line 2764) is no longer the primary path; L4 targets the Direct variant.

Currently: `s_noObjCount` counts remote entities that have registry entries but no game object. It only logs. No requeue.

**Fix:**
1. Track `std::unordered_map<EntityID, std::pair<int, TickT>> s_spawnRetries` — retries-so-far + last-retry-tick per entity.
2. When `GetGameObject(eid) == nullptr`, check this map. If first miss: append to spawn queue, set retries=1, log `warn` rate-limited once per entity.
3. On subsequent misses (after cooldown = 30 ticks), if retries < 3: requeue, increment. If retries >= 3: log `error` with entity id, mark entity `spawn-failed` in the registry (new state), stop retrying.
4. Expose `s_spawnRetries` via `/status` command for live debugging.

Makes "remote entity registered but never spawned" self-healing for transient races (position packet arriving before spawn packet) and visible when it's a hard failure.

### L5. `WritePosition` Method 3 gated behind explicit opt-in

**Location:** `KenshiMP.Core/game/game_character.cpp::WritePosition` (lines 486-505).

Today: Method 3 (cached-position write) is the last-resort fallback when Method 1 (setPosition function) and Method 2 (physics chain) both unavailable. Logged as WARN; comment says "acceptable" for continuously-updated remotes.

In practice on Steam, until the AnimClass probe succeeds, Method 1 may be unresolved and Method 2's `animClassOffset` unset — so the very first position updates land on Method 3, causing visible drift during the critical first few seconds after spawn.

**Fix:**
- Method 1: primary, always attempted if `s_setPositionFn != nullptr`.
- Method 2: fallback if Method 1 unresolved, only if `animClassOffset != -1`.
- Method 3: gated behind compile flag `KMP_ALLOW_CACHED_POS_FALLBACK` (default OFF). When gated off and Methods 1/2 both fail, `WritePosition` returns `false` and logs `warn` (rate-limited). Caller (`ApplyRemotePositionsDirect`) treats this as "position not applied yet" — the remote character stays at its last applied position until Method 1 or 2 becomes available.
- Trigger AnimClass probe eagerly on remote characters immediately after `L3` post-spawn — so Method 2 is available within one tick of spawn rather than whenever the deferred probe happens to run.

Eliminates visible drift by refusing to write-and-immediately-lose.

## Data Flow

**Before (on Steam):**
```
Client connects → S2C_EntitySpawn
  Path A first: FindModCharacterBySlot → CharacterIterator (reads GameWorld+0x0888)
                → garbage → null → Path A fails
  Path B (DIRECT SPAWN): factory with stale cached struct → character OK
    → SEH_FixUpFaction_Core (may leave stale faction → crash later)
    → MarkRemoteControlled OK
    → AddCharacterToLocalSquad → activePlatoon resolution broken → WRITE AV → crash
  Path C (FallbackPostSpawn): ONLY MarkRemoteControlled + AnimClass probe
    → character visible but AI-driven (no WritePlayerControlled), not in squad panel

  Position sync: ApplyRemotePositionsDirect → some entities have no game object
                                               → silent skip (no requeue)
                                               → others get Method 3 → drift
```

**After:**
```
At DLL init: CharacterCreate hook installed (LOADING PASSTHROUGH mode).
             → P2 shadow list starts populating as game spawns each NPC during save-load.
Game-loaded gate → P1 captures GameWorld from Hook_GameFrameUpdate rcx.

Client connects → S2C_EntitySpawn
  Path A: FindModCharacterBySlot → CharacterIterator reads shadow list → match OR miss
  Path B (if miss): factory with FRESH struct (L1)
    → SEH_FixUpFaction_Core with shadow-sampled fallback (L2); hard-fail despawns cleanly
    → ApplyStandardPostSpawn:
        → MarkRemoteControlled
        → AddCharacterToLocalSquad (P3 resolved activePlatoon reliably)
        → WritePlayerControlled(true) (L3, gated on AnimClass probe)
        → AnimClass probe (eager, so Method 2 ready fast)
  Path C path eliminated as a separate code path — merged into Path B via ApplyStandardPostSpawn

Position sync: Method 1 or 2 (L5) for all remote characters → smooth, no drift
  If no game object for an entity: requeue up to 3× with cooldown (L4)
```

## Error Handling

| Scenario | Behavior |
|---|---|
| `GameFrameUpdate` never fires | `s_capturedGameWorld` stays null. L1 falls back to legacy RVA read (will be wrong on Steam but no worse than today). Log `error` at game-loaded + 30s. P2 shadow list is unaffected (hook-populated). |
| Shadow list empty (no CharacterCreate events seen) | Iterator returns 0 characters. Log `warn`; likely indicates hook install failed. Poll again next save-load. |
| `CallFactoryDirect` returns null after 3 retries | Entity marked `spawn-failed`; `error` log; registry skips that entity until server resends. |
| Faction fixup exhausts all sources (L2) | Character despawned immediately (teleport underground + registry delete); `C2S_SpawnFailed` sent to server; entity marked `faction-invalid`. |
| `ResolveActivePlatoon` fails for a character's vtable class | Squad injection skipped for that character; remains visible, absent from squad panel. `warn` log. Non-fatal. |
| `WritePlayerControlled` AVs on a not-fully-initialized character | SEH `__except` catches, logs `warn` once per entity, continues. Character remains visible and AI-driven until next spawn setup attempt (no longer a full-process crash). |
| Shadow list diverges from game reality (hook suspended) | `HandleSpawnQueue` self-heals via L4 requeue. Periodic (every 500 ticks) sanity check purges shadow entries whose vtables went invalid. |
| Method 3 compile flag re-enabled | Behaves as today; documented as diagnostic-only fallback, not recommended for shipping. |

## Testing Strategy

**Unit (`KenshiMP.UnitTest`):**
- Shadow list under concurrent Create/Destroy hook calls (thread-safety).
- `CharacterIterator` returns consistent snapshots during mid-iteration mutation.
- `SEH_FixUpFaction_Core`: each class of stale pointer input (null, misaligned, bad vtable, module-range) → hard-fail.
- `ApplyStandardPostSpawn`: each step isolated under SEH, all still run on individual-step AV.

**Integration (`KenshiMP.IntegrationTest` + LiveTest dual mode):**
- Host + 1 remote on Steam build. Assert:
  - Remote squad members spawn visibly within 5s.
  - Remote characters appear in host's squad panel (regression target for re-enabled squad injection).
  - Zero `WritePosition Method 3` warnings in 2 minutes of movement.
  - Zero faction crashes across 10 disconnect/reconnect cycles.
- Large-squad test: 16 characters per side, 2-player session, 10 minutes. Assert memory stable, no shadow-list drift, no position-apply requeue errors.

**Regression (GOG 1.0.68):**
- All existing LiveTest scenarios pass unchanged. P1/P2/P3 must not regress GOG.
- Path C's re-enabled steps must work on GOG too (regression: pre-fix, they were disabled universally).

**Targeted crash replay:**
- Replay CRASH.log scenarios tagged `game + 0x927E94`. Assert zero occurrences after L2.
- Replay scenarios tagged `game + 0xE85340` (squad-injection AV). Assert zero after P3 + L3.

## Migration

- No schema changes; wire-compatible with existing servers.
- `docs/offsets.json`: annotate `PlayerBase` / `GameWorld` RVAs as GOG-only; point to `s_capturedGameWorld`.
- `docs/PHASES.md`: update phase-7 ("squad injection disabled on Steam") → "working via P3 + L3".
- `MEMORY.md`: `CRITICAL: PlayerBase / GameWorld on Steam` entry simplified — superseded by P1. `Spawn Pipeline` entry updated to show unified ApplyStandardPostSpawn.

## Open Questions (for planning phase, not blocking design approval)

1. Should `ResolveActivePlatoon`'s per-vtable offset cache be invalidated on save-switch? Probably yes.
2. L4's retry cap (3) and cooldown (30 ticks): should they scale with network RTT? Not for v1.
3. L5's `KMP_ALLOW_CACHED_POS_FALLBACK` flag: permanent diagnostic, or delete entirely once Method 1/2 are confirmed reliable? Lean toward delete after one release cycle.
4. Shadow list sanity-sweep frequency (500 ticks ≈ 8s): appropriate, or too aggressive? Tune during integration test.
