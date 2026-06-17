# Contributing to Kenshi Online

Thank you for your interest in contributing to Kenshi Online! This document provides guidelines for contributing to the project.

---

## Table of Contents

1. [Code of Conduct](#code-of-conduct)
2. [Getting Started](#getting-started)
3. [Development Environment](#development-environment)
4. [Code Style Guide](#code-style-guide)
5. [Git Workflow](#git-workflow)
6. [Pull Request Process](#pull-request-process)
7. [Testing Requirements](#testing-requirements)
8. [Documentation Requirements](#documentation-requirements)
9. [Reporting Bugs](#reporting-bugs)
10. [Areas Needing Help](#areas-needing-help)

---

## Code of Conduct

- Be respectful and professional in all communications
- Focus on constructive feedback
- Help create a welcoming environment for new contributors
- Follow the project's technical standards

---

## Getting Started

### Prerequisites

- **Visual Studio 2022** (Community Edition or higher)
- **Windows 10+ SDK** (included with VS 2022)
- **CMake 3.20+** (included with VS 2022)
- **Git** for version control
- **Kenshi** (Steam version recommended for testing)

### Initial Setup

1. **Fork and Clone**
   ```bash
   git clone https://github.com/YOUR_USERNAME/Kenshi-Online.git
   cd Kenshi-Online
   ```

2. **Build Dependencies**
   ```bash
   cd build
   cmake ..
   ```

3. **Build the Project**
   ```bash
   MSBuild.exe KenshiMP.sln /p:Configuration=Release /p:Platform=x64 /m
   ```

4. **Verify Build**
   - Check `build/bin/Release/` for output files:
     - `KenshiMP.Core.dll` (client)
     - `KenshiMP.Server.exe` (server)
     - `KenshiMP.Injector.exe` (launcher)

### Project Structure

```
KenshiMP/
├── KenshiMP.Common/     # Shared types, protocol, messages (static lib)
├── KenshiMP.Scanner/    # Memory pattern scanner, MinHook wrapper (static lib)
├── KenshiMP.Core/       # Client DLL with 14 hook modules
├── KenshiMP.Server/     # Dedicated server executable
├── KenshiMP.Injector/   # Win32 GUI launcher
├── KenshiMP.TestClient/ # Fake player console client
├── KenshiMP.IntegrationTest/ # Protocol test suite
├── KenshiMP.UnitTest/   # Unit tests
├── docs/                # Technical documentation
└── lib/                 # Third-party dependencies (ENet, MinHook, spdlog, json)
```

---

## Code Style Guide

### Naming Conventions

#### Files
- Headers: `snake_case.h` (e.g., `entity_registry.h`)
- Source: `snake_case.cpp` (e.g., `packet_handler.cpp`)
- Modules: `descriptive_noun` (e.g., `combat_hooks.cpp`, not `combat.cpp`)

#### C++ Identifiers
- **Namespaces**: `lowercase` (all code in `namespace kmp`)
- **Classes/Structs**: `PascalCase` (e.g., `EntityRegistry`, `GameServer`, `MsgHandshake`)
- **Functions/Methods**: `PascalCase` (e.g., `HandlePositionUpdate()`, `ValidateInbound()`)
- **Variables**: `camelCase` (e.g., `entityId`, `localState`, `isConnected`)
- **Member Variables**: `m_camelCase` prefix (e.g., `m_config`, `m_players`, `m_entityRegistry`)
- **Static/Global Variables**: `g_camelCase` prefix (e.g., `g_lastTickStep`, `g_vehHandle`)
- **Constants**: `SCREAMING_SNAKE_CASE` (e.g., `KMP_MAX_PLAYERS`, `INVALID_ENTITY`)
- **Enums**: `PascalCase` for type, `PascalCase` for values (e.g., `MessageType::Handshake`)

#### Example
```cpp
namespace kmp {

constexpr int KMP_MAX_PLAYERS = 16;

struct NetEntityId {
    uint32_t id = INVALID_ENTITY;
    uint32_t generation = 0;
    
    bool IsValid() const { return id != INVALID_ENTITY; }
};

class EntityRegistry {
public:
    void RegisterEntity(EntityID localId, PlayerID owner);
    bool IsLocalOwned(EntityID localId) const;
    
private:
    std::unordered_map<EntityID, EntityInfo> m_entities;
    PlayerID m_localPlayerId = INVALID_PLAYER;
};

} // namespace kmp
```

### Code Formatting

#### Indentation & Spacing
- **4 spaces** per indent level (no tabs)
- **Braces**: Opening brace on same line for functions/classes, same line for control flow
  ```cpp
  void MyFunction() {
      if (condition) {
          DoSomething();
      }
  }
  ```
- **Line Length**: Aim for 100 characters, hard limit 120
- **Blank Lines**: One blank line between functions, two between major sections

#### Comments
- Use `//` for single-line comments
- Use `/* */` for multi-line comments or disable code blocks
- Use `// ──` for section dividers (3 em-dashes: `───`)
  ```cpp
  // ── Network Handlers ──
  void HandleConnect() { /* ... */ }
  void HandleDisconnect() { /* ... */ }
  ```

#### Header Guards
Use `#pragma once` (already project standard):
```cpp
#pragma once
#include <cstdint>

namespace kmp {
// ...
}
```

#### Include Order
1. Corresponding header (for .cpp files)
2. Project headers (`kmp/*`, `sync/*`, `hooks/*`)
3. Third-party headers (`<spdlog/*>`, `<enet/*>`)
4. Standard library headers (`<string>`, `<vector>`)

```cpp
#include "entity_registry.h"
#include "kmp/protocol.h"
#include "sync/authority_validator.h"
#include <spdlog/spdlog.h>
#include <string>
#include <vector>
```

### Memory & Safety

#### Pointers
- **Prefer references** over raw pointers when ownership is clear
- **Use `nullptr`** not `NULL` or `0`
- **Check pointers before dereferencing** in hooks (game memory can be invalid)
  ```cpp
  if (entity && entity->health > 0.f) {
      // safe to use
  }
  ```

#### Game Memory Access
- **Always use SafeRead/SafeWrite** from `kmp/memory.h` for game structures
- **Never assume game pointers are valid** - always null-check
- **Use `__try/__except`** for crash-prone reverse engineering code
  ```cpp
  __try {
      float* healthPtr = reinterpret_cast<float*>(baseAddr + 0x1B8);
      health = *healthPtr;
  } __except(EXCEPTION_EXECUTE_HANDLER) {
      spdlog::warn("Failed to read health at 0x{:X}", baseAddr);
      health = 0.f;
  }
  ```

#### Thread Safety
- Use `std::mutex` for shared data structures
- Lock guards mandatory for mutex access:
  ```cpp
  std::lock_guard<std::mutex> lock(m_mutex);
  ```
- Mark volatile variables used in crash handlers: `volatile int g_lastTickStep`

### Logging

Use spdlog with appropriate levels:
```cpp
#include <spdlog/spdlog.h>

spdlog::trace("Detailed trace info (disabled in Release)");
spdlog::debug("Debug info for development");
spdlog::info("Important runtime events");
spdlog::warn("Recoverable errors or unexpected state");
spdlog::error("Critical errors requiring attention");
```

**Guidelines:**
- Use `info` for connection events, spawn confirmations, major state changes
- Use `warn` for validation failures, authority violations, retries
- Use `error` for crashes, hook failures, network disconnects
- **Include context**: player IDs, entity IDs, generations, tick numbers
  ```cpp
  spdlog::warn("Authority violation: Player {} tried to move entity {} (owner: {})",
               senderId, entityId, actualOwner);
  ```

---

## Git Workflow

### Branch Naming

Use descriptive branch names with category prefixes:

- `feature/` - New features (e.g., `feature/inventory-sync`)
- `fix/` - Bug fixes (e.g., `fix/combat-deadlock`)
- `refactor/` - Code refactoring (e.g., `refactor/entity-registry`)
- `docs/` - Documentation updates (e.g., `docs/hook-reference`)
- `test/` - Test additions/fixes (e.g., `test/combat-resolver`)
- `chore/` - Build/tooling changes (e.g., `chore/cmake-cleanup`)

**Examples:**
```bash
git checkout -b feature/ai-sync
git checkout -b fix/spawn-queue-crash
git checkout -b docs/contributing-guide
```

### Commit Messages

#### Format
```
<type>: <short summary> (max 72 chars)

<optional detailed description>

<optional footer: Co-Authored-By, Fixes, etc.>
```

#### Types
- `feat:` - New feature
- `fix:` - Bug fix
- `refactor:` - Code restructuring without behavior change
- `docs:` - Documentation only
- `test:` - Test additions/fixes
- `chore:` - Build system, dependencies, tooling
- `perf:` - Performance improvements
- `style:` - Formatting, whitespace (no logic change)

#### Examples
```
feat: Add inventory sync to HandleItemPickup hook

Implements Phase 9 inventory sync by hooking ItemPickup (0x74C8B0)
and broadcasting item state changes to server. Validates item ownership
before allowing pickup via AuthorityValidator.

Fixes #42
```

```
fix: Prevent spawn queue deadlock on late join

DeferredSpawnQueue now flushes pending snapshots immediately in
HandleEntitySpawn after local entity registration. Previously
snapshots were queued but never flushed, causing invisible players.

Fixes #38
```

```
docs: Add hook status reference to memory.md

Documents all 14 hook modules with addresses, patterns, and working
status. Includes Steam/GOG offset differences and MovRaxRsp prologue
requirements.
```

### Workflow Steps

1. **Create a branch** from `main`:
   ```bash
   git checkout main
   git pull origin main
   git checkout -b feature/my-feature
   ```

2. **Make focused commits** (one logical change per commit):
   ```bash
   git add path/to/changed/files  # Be specific, avoid `git add -A`
   git commit -m "feat: Add authority validation to combat hooks"
   ```

3. **Keep your branch updated**:
   ```bash
   git fetch origin
   git rebase origin/main  # Preferred over merge for clean history
   ```

4. **Push to your fork**:
   ```bash
   git push origin feature/my-feature
   ```

### What NOT to Commit

- Build artifacts (`build/`, `bin/`, `*.dll`, `*.exe`, `*.lib`)
- IDE files (`*.user`, `.vs/`, `*.suo`)
- Log files (`*.log`, `KenshiOnline_*.txt`)
- Personal config (`settings.cfg`, `server.json` with real IPs)
- Temporary files (`*.bak`, `*.tmp`, `*~`)
- Large binaries or test saves

See `.gitignore` for full list.

---

## Pull Request Process

### Before Opening a PR

1. **Test your changes locally**:
   - Build in Release mode: `MSBuild.exe KenshiMP.sln /p:Configuration=Release /p:Platform=x64 /m`
   - Run integration tests: `KenshiMP.IntegrationTest.exe`
   - Test in-game with 2+ clients if networking changes
   - Check logs for errors/warnings

2. **Update documentation**:
   - Add/update comments for new APIs
   - Update relevant `.md` files in `docs/`
   - Update `README.md` if user-facing changes

3. **Run code review checklist** (see below)

### PR Title & Description

#### Title Format
```
[Type] Short summary (e.g., [Feature] Add AI sync to entity_hooks)
```

#### Description Template
```markdown
## Summary
Brief description of what this PR does (2-3 sentences).

## Changes
- Added `HandleAIUpdate()` to `entity_hooks.cpp`
- Modified `GameServer::Tick()` to broadcast AI decisions
- Updated `MessageType` enum with `AIStateUpdate`

## Testing
- [ ] Built successfully in Release mode
- [ ] Integration tests pass
- [ ] Tested with 2 clients in-game
- [ ] No new warnings in logs

## Related Issues
Fixes #42, addresses part of #38

## Screenshots/Logs (if applicable)
[Attach relevant screenshots or log snippets]
```

### Review Checklist

Before requesting review, verify:

#### Code Quality
- [ ] Follows naming conventions (PascalCase classes, camelCase variables, m_ prefix)
- [ ] Includes logging for important events (connection, spawn, errors)
- [ ] No hardcoded magic numbers (use named constants)
- [ ] Error handling for all game memory access (SafeRead/SafeWrite or `__try`)
- [ ] Thread safety (mutexes for shared state)
- [ ] No memory leaks (RAII, smart pointers preferred)

#### Testing
- [ ] Compiles without warnings in Release x64
- [ ] Integration tests pass (`KenshiMP.IntegrationTest.exe`)
- [ ] Tested with at least 2 clients (if multiplayer change)
- [ ] No crashes during 5-minute test session
- [ ] Logs show expected behavior (no unexpected warnings/errors)

#### Documentation
- [ ] Public APIs have doc comments
- [ ] Complex algorithms have explanation comments
- [ ] Updated relevant `docs/*.md` files
- [ ] Updated `README.md` if user-facing

#### Git Hygiene
- [ ] Commits are focused (one logical change each)
- [ ] Commit messages follow format (`type: summary`)
- [ ] No merge commits (rebased onto latest main)
- [ ] No unrelated changes (formatting, whitespace in other files)
- [ ] `.gitignore` prevents committing build artifacts

### Review Process

1. **Automated Checks** (when CI is set up):
   - Build succeeds on Windows x64
   - Integration tests pass
   - No new compiler warnings

2. **Code Review**:
   - At least one maintainer approval required
   - Address all review comments or explain why not
   - Use "Request changes" / "Approve" workflow

3. **Merge**:
   - Maintainer will squash-merge or rebase-merge
   - Your commits will be preserved if clean history
   - Branch deleted automatically after merge

### PR Etiquette

- **Be responsive** to review feedback
- **Ask questions** if feedback is unclear
- **Explain your approach** if non-obvious
- **Don't take it personally** - reviews improve code quality
- **Help review others' PRs** - we're all learning

---

## Testing Requirements

### Before Every PR

1. **Build in Release Mode**:
   ```bash
   cd build
   MSBuild.exe KenshiMP.sln /p:Configuration=Release /p:Platform=x64 /m
   ```
   - Must succeed with **zero errors**
   - Warnings acceptable only if pre-existing (document in PR)

2. **Run Integration Tests**:
   ```bash
   cd build/bin/Release
   KenshiMP.IntegrationTest.exe
   ```
   - All 15 protocol tests must pass
   - If you add new message types, add corresponding tests

3. **Manual Testing**:
   - **Minimal**: Launch Kenshi with your DLL, connect to localhost server
   - **Standard**: 2 clients on same machine
   - **Thorough**: 2 clients on different machines (LAN or Internet)
   - **Duration**: At least 5 minutes of gameplay
   - **Verify**: Check `KenshiOnline_Server.log` and `Kenshi/kenshi.log` for errors

### Test Scenarios by Category

#### Networking Changes
- [ ] Connect/disconnect cycles (10+ times)
- [ ] Late join (server running, clients join one-by-one)
- [ ] Server shutdown with clients connected
- [ ] Network hiccup simulation (firewall block for 5s)

#### Entity/Spawn Changes
- [ ] Player spawn on connect
- [ ] Remote player visibility
- [ ] Entity destruction/respawn cycles
- [ ] Late join with existing entities

#### Combat Changes
- [ ] Melee attacks sync correctly
- [ ] Death/KO events broadcast
- [ ] No duplicate death events (echo suppression)
- [ ] Combat damage values reasonable

#### Authority Changes
- [ ] Cannot move other players' entities
- [ ] Server rejects invalid commands
- [ ] Generation mismatch rejection
- [ ] Authority violations logged

### Writing New Tests

If adding new protocol messages or major features:

1. **Add Integration Test** in `KenshiMP.IntegrationTest/`:
   ```cpp
   void TestNewFeature() {
       // Setup
       TestServer server;
       TestClient client;
       
       // Action
       client.SendMessage(MsgNewFeature{...});
       
       // Verify
       auto response = client.WaitForMessage(MessageType::NewFeatureResponse, 1000);
       assert(response.success);
   }
   ```

2. **Add Unit Test** in `KenshiMP.UnitTest/` for pure logic:
   ```cpp
   TEST_CASE("AuthorityValidator rejects stale generation") {
       EntityRegistry reg;
       reg.RegisterEntity(100, 1, 5); // gen=5
       
       auto decision = AuthorityValidator::ValidateInbound(reg, 100, 4, 1);
       REQUIRE(decision == SnapshotDecision::RejectStaleGeneration);
   }
   ```

---

## Documentation Requirements

### Code Documentation

#### Public APIs (Headers)
Document all public classes, structs, functions:

```cpp
/// @brief Validates authority for inbound entity snapshots.
///
/// Implements the 8-way decision tree (Phase 2) to determine whether
/// a snapshot should be applied, queued, reconciled, or rejected.
class AuthorityValidator {
public:
    /// @brief Validates a position update from a remote client.
    /// @param registry Entity registry to query ownership
    /// @param entityId Local entity ID
    /// @param generation Generation tag from network packet
    /// @param senderId Player ID who sent the snapshot
    /// @return Decision enum indicating how to handle the snapshot
    static SnapshotDecision ValidateInbound(
        const EntityRegistry& registry,
        EntityID entityId,
        uint32_t generation,
        PlayerID senderId);
};
```

#### Complex Logic (Implementation)
Explain *why*, not *what*:

```cpp
// Use smallest-three quaternion compression: drop the largest component
// (recoverable via w^2 + x^2 + y^2 + z^2 = 1) and pack remaining 3
// components into 10 bits each. This reduces 16 bytes to 4 bytes with
// ~0.001 precision loss, critical for 50ms tick network bandwidth.
uint32_t Quat::Compress() const {
    // ... implementation
}
```

#### Reverse Engineering Findings
Document offsets, patterns, and quirks:

```cpp
// ── Character Death Hook ──
// RVA: 0x007A6200 (Steam), 0x007A4F30 (GOG)
// Pattern: "48 89 5C 24 08 57 48 83 EC 20 48 8B F9"
// Signature: void __fastcall CharacterDeath(Character* this)
//
// CRITICAL: This is a MovRaxRsp prologue (mov [rsp+8], rbx). MinHook's
// trampoline corrupts RBX if we don't use a naked detour. Use the
// wrapper pattern from combat_hooks.cpp.
static Character_Death_t g_origCharacterDeath = nullptr;
```

### Project Documentation

Update these files when relevant:

#### `README.md`
- User-facing changes (new features, installation steps)
- Known issues / bug fixes
- Version bumps

#### `docs/PHASES.md`
- Authority system implementation progress
- Phase completion status

#### `docs/AUTHORITY-IMPLEMENTATION-COMPLETE.md`
- Completed authority phases
- Implementation details

#### `docs/MULTIPLAYER-FIXES-*.md`
- Bug fixes and solutions
- Performance improvements

#### `docs/Kenshi-Online-Technical-Reference.md`
- Architecture changes
- Protocol updates
- New modules/systems

### Commit Documentation

If adding new files/modules, include a header block:

```cpp
// ──────────────────────────────────────────────────────────────────────────────
// KenshiMP.Core - authority_validator.cpp
// ──────────────────────────────────────────────────────────────────────────────
// Purpose: Client-side authority validation for entity snapshots (Phase 2).
//
// Implements the 8-way decision tree to prevent:
//  - Echo loops (local entity updates from server)
//  - Ghost control (stale generation packets)
//  - Authority violations (clients controlling others' entities)
//
// Used by: packet_handler.cpp (HandlePositionUpdate, HandleEntitySpawn)
// See: docs/AUTHORITY-IMPLEMENTATION-COMPLETE.md (Phase 2)
// ──────────────────────────────────────────────────────────────────────────────
```

---

## Reporting Bugs

### Before Reporting

1. **Search existing issues**: Your bug might be known
2. **Test on clean install**: Rule out mod conflicts
3. **Check logs**: `KenshiOnline_Server.log`, `kenshi.log`, `KenshiOnline_BREADCRUMB.txt`
4. **Try latest version**: Bug might be fixed already

### Bug Report Template

Use this template when opening an issue:

```markdown
## Bug Description
Clear, concise description of the problem.

## Steps to Reproduce
1. Launch server with config X
2. Connect 2 clients
3. Player 1 attacks Player 2
4. Observe crash/incorrect behavior

## Expected Behavior
What should happen?

## Actual Behavior
What actually happens?

## Environment
- **KenshiMP Version**: 0.3.0-alpha (check README.md)
- **Kenshi Version**: Steam / GOG
- **OS**: Windows 10 21H2 / Windows 11 23H2
- **Network**: LAN / Internet / Localhost

## Logs
Attach or paste relevant log sections:
- `KenshiOnline_Server.log`
- `kenshi.log` (last 50 lines)
- `KenshiOnline_BREADCRUMB.txt` (if crash)

## Screenshots/Videos
If visual bug, attach screenshots or short video.

## Additional Context
Any other relevant info (mods, system specs, etc.)
```

### Severity Labels

When maintainers triage, issues are labeled:

- **Critical**: Crash, data loss, or security issue
- **High**: Major feature broken, blocks gameplay
- **Medium**: Feature partially working, workaround exists
- **Low**: Minor visual bug, QoL issue

### Effective Bug Reports

**Good Example:**
> **Title**: "Server crash on player death in combat"
>
> When Player A kills Player B in melee combat, the server crashes with access violation at `0x007A6200`. Happens consistently (5/5 attempts).
>
> **Steps**: 1) Start server, 2) Connect 2 clients, 3) Player A attacks Player B until death
>
> **Log**: `[ERROR] Access violation at 0x007A6200 (CharacterDeath hook)`
>
> **Env**: v0.3.0-alpha, Steam, Windows 11, LAN

**Bad Example:**
> **Title**: "It doesn't work"
>
> I tried to play multiplayer but it crashed. Fix it.

---

## Areas Needing Help

We welcome contributions in these areas:

### High Priority

1. **Combat Damage Sync** (`fix/combat-damage-sync`)
   - ApplyDamage hook (0x7A33A0) needs naked detour
   - Damage bars don't sync to remote clients
   - See `docs/02-hook-status.md` for details

2. **Client Prediction** (`feature/client-prediction`)
   - Phase 7: Implement `ReconcileLocal()` in `authority_validator.cpp`
   - Interpolate between predicted and server positions
   - Reduce perceived lag

3. **Inventory Sync** (`feature/inventory-sync`)
   - Hook ItemPickup (0x74C8B0) and ItemDrop (0x745DE0)
   - Broadcast inventory changes via new message types
   - Validate item ownership on server

### Medium Priority

4. **AI Synchronization** (`feature/ai-sync`)
   - Hook AICreate (0x622110) and AI decision functions
   - Broadcast NPC AI states
   - Server-authoritative AI for shared NPCs

5. **Documentation/Wiki** (`docs/`)
   - API reference for protocol messages
   - Hook module reference (14 modules)
   - User guide for hosting servers

6. **Automated Testing** (`test/ci-pipeline`)
   - GitHub Actions workflow for Windows builds
   - Automated integration test runs
   - Crash dump analysis

### Low Priority / Nice to Have

7. **Performance Profiling** (`perf/`)
   - Identify network bandwidth bottlenecks
   - Optimize snapshot compression
   - Reduce tick processing overhead

8. **Quality of Life**
   - In-game chat improvements
   - Server browser UI
   - Connection quality indicators

### How to Claim

Comment on an existing issue or create a new one:
> "I'd like to work on combat damage sync. Can you assign this to me?"

Maintainers will provide guidance and answer questions.

---

## Additional Resources

### Documentation
- **[README.md](README.md)** - Project overview
- **[docs/AUTHORITY-IMPLEMENTATION-COMPLETE.md](docs/AUTHORITY-IMPLEMENTATION-COMPLETE.md)** - Authority system
- **[docs/Kenshi-Online-Technical-Reference.md](docs/Kenshi-Online-Technical-Reference.md)** - Full technical spec
- **[docs/MULTIPLAYER-FIXES-2026-06-04.md](docs/MULTIPLAYER-FIXES-2026-06-04.md)** - Recent fixes

### External Resources
- **Kenshi Modding Wiki**: https://kenshi.fandom.com/wiki/Modding
- **ENet Documentation**: http://enet.bespin.org/Tutorial.html
- **MinHook Guide**: https://github.com/TsudaKageyu/minhook/wiki

### Communication
- **GitHub Issues**: [Report bugs, request features](https://github.com/The404Studios/Kenshi-Online/issues)
- **GitHub Discussions**: [Ask questions, share ideas](https://github.com/The404Studios/Kenshi-Online/discussions)
- **Email**: the404studios@gmail.com

---

## Questions?

If anything is unclear:

1. Check existing documentation in `docs/`
2. Search closed issues for similar questions
3. Ask in GitHub Discussions
4. Open an issue with the "question" label

We appreciate your contributions and look forward to building Kenshi Online together!

---

**Last Updated**: 2026-06-04 | **Version**: 0.3.0-alpha
