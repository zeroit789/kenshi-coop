# Kenshi Multiplayer: Reverse Engineering Guide

**Version:** 1.0 (2026-06-04)  
**Game Target:** Kenshi v1.0.68 (Steam)  
**Binary:** kenshi_x64.exe (~35MB, 64-bit PE, MSVC compiler)

This document provides a comprehensive guide to reverse engineering Kenshi's game engine for multiplayer mod development. It covers the complete workflow from pattern discovery to hook installation.

---

## Table of Contents

1. [Overview](#overview)
2. [Binary Structure](#binary-structure)
3. [Tools & Prerequisites](#tools--prerequisites)
4. [Pattern Discovery Workflow](#pattern-discovery-workflow)
5. [Validation Using .pdata](#validation-using-pdata)
6. [Function Resolution Architecture](#function-resolution-architecture)
7. [Known Function Offsets](#known-function-offsets)
8. [Steam vs GOG Differences](#steam-vs-gog-differences)
9. [Hook Installation Process](#hook-installation-process)
10. [MovRaxRsp Prologue Fix](#movraxrsp-prologue-fix)
11. [Common Pitfalls](#common-pitfalls)
12. [IDA Pro Workflow](#ida-pro-workflow)
13. [Future Function Discovery](#future-function-discovery)

---

## Overview

Kenshi's game engine is a closed-source C++ application using Ogre3D for rendering, PhysX for physics, and MyGUI for UI. To implement multiplayer functionality, we must:

1. **Discover function addresses** in the compiled binary (kenshi_x64.exe)
2. **Validate** addresses using Windows exception handling metadata (.pdata)
3. **Hook** critical game functions to intercept events (character spawn, damage, movement)
4. **Handle MSVC quirks** like the `mov rax, rsp` prologue pattern
5. **Maintain compatibility** across game versions (Steam/GOG/patches)

Our approach uses **multiple resolution strategies** in priority order:
1. Runtime string cross-reference scanning (most reliable)
2. Static byte pattern matching (version-specific)
3. VTable slot discovery (for virtual methods)
4. Call graph analysis (for helper functions)
5. Hardcoded RVAs with validation (last resort)

---

## Binary Structure

### PE Sections (Steam v1.0.68)

```
Module Base:  0x140000000 (ImageBase)
.text:        VA=0x1000,      Size=0x1671412 (23.4 MB code)
.rdata:       VA=0x1673000,   Size=0x54A4CB  (5.3 MB strings/vtables)
.data:        VA=0x1BBE000,   Size=0x58E000  (5.6 MB globals)
.pdata:       VA=0x214C000,   Size=77,108 RUNTIME_FUNCTION entries
```

### Key Characteristics

- **77,108 functions** with exception handling metadata in .pdata
- **70+ debug strings** with `[ClassName::methodName]` format in .rdata
- **MSVC compiler artifacts**:
  - Stack frame cookies (`0xFEFFFFFF` magic)
  - SEH exception handlers (C++ `try/catch`)
  - `mov rax, rsp` prologue in 64% of large functions
- **No symbols** (.pdb not distributed)
- **No RTTI** type names (stripped in release build)
- **Virtual tables** present but class names removed

---

## Tools & Prerequisites

### Required Tools

1. **IDA Pro 7.5+** (or Ghidra) — disassembler with .pdata parsing
2. **Cheat Engine 7.4+** — runtime memory scanner for offset validation
3. **x64dbg** — debugger for dynamic analysis
4. **PE-bear** — PE structure viewer
5. **Visual Studio 2022** — for building pattern scanner

### Optional Tools

- **HxD** — hex editor for pattern verification
- **Process Hacker** — module/memory inspector
- **WinDbg** — advanced debugging (stack traces, exception analysis)

### Development Environment

```batch
# Clone KenshiMP source
git clone <repo_url> KenshiMP
cd KenshiMP

# Build pattern scanner + hooks
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release

# Scanner library is in KenshiMP.Scanner/
# Hook installer is in KenshiMP.Core/
```

---

## Pattern Discovery Workflow

### Step 1: String Cross-Reference Analysis

**Most reliable method** — works across Steam/GOG with minor adjustments.

#### Theory

MSVC embeds diagnostic strings in .rdata for logging/errors:
```cpp
// Game code
void Character::Death() {
    Logger->log("{1} has died from blood loss.");
    // ...
}
```

The string `"{1} has died from blood loss."` appears in .rdata at a fixed address. The code that references it looks like:

```asm
lea rcx, [rip+0x12345]  ; Load string address (RIP-relative)
call Logger_log         ; Call logger
```

We can:
1. Find the string in .rdata
2. Find code that LEAs it (RIP-relative addressing)
3. Walk backwards to find the function prologue
4. Validate using .pdata

#### Implementation (C++)

```cpp
// KenshiMP.Scanner/src/patterns.cpp

class RuntimeStringScanner {
public:
    // Find a function containing a string reference
    uintptr_t FindFunctionByString(const char* searchStr, int searchLen) const {
        // Step 1: Find string in .rdata
        uintptr_t strAddr = FindStringInMemory(searchStr, searchLen);
        if (!strAddr) return 0;

        // Step 2: Find LEA instruction that references it
        uintptr_t xref = FindStringXref(strAddr);
        if (!xref) return 0;

        // Step 3: Find function start using .pdata
        uintptr_t funcStart = FindFunctionStart(xref);
        return funcStart;
    }

private:
    uintptr_t FindStringInMemory(const char* searchStr, int len) const {
        // Linear scan of .rdata section
        const uint8_t* start = reinterpret_cast<const uint8_t*>(m_rdataBase);
        const uint8_t* end = start + m_rdataSize - len;
        
        for (const uint8_t* p = start; p < end; p++) {
            if (std::memcmp(p, searchStr, len) == 0) {
                return reinterpret_cast<uintptr_t>(p);
            }
        }
        return 0;
    }

    uintptr_t FindStringXref(uintptr_t stringAddr) const {
        // Scan .text for LEA rcx, [rip+disp32] pointing to stringAddr
        const uint8_t* text = reinterpret_cast<const uint8_t*>(m_textBase);
        
        for (size_t i = 0; i + 7 < m_textSize; i++) {
            // Pattern: 48 8D xx or 4C 8D xx (REX.W LEA)
            if ((text[i] == 0x48 || text[i] == 0x4C) && text[i+1] == 0x8D) {
                uint8_t modrm = text[i+2];
                if ((modrm & 0xC7) == 0x05) {  // mod=0, rm=5 → [rip+disp32]
                    int32_t disp;
                    std::memcpy(&disp, &text[i+3], 4);
                    uintptr_t instrAddr = m_textBase + i;
                    uintptr_t target = instrAddr + 7 + disp;
                    if (target == stringAddr) {
                        return instrAddr;
                    }
                }
            }
        }
        return 0;
    }

    uintptr_t FindFunctionStart(uintptr_t codeAddr) const {
        // Use Windows .pdata lookup (authoritative)
        DWORD64 imageBase = 0;
        auto* rtFunc = RtlLookupFunctionEntry(
            static_cast<DWORD64>(codeAddr), &imageBase, nullptr);
        
        if (rtFunc) {
            return static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
        }
        
        // Fallback: walk backwards looking for prologue (max 16KB)
        for (uintptr_t addr = codeAddr - 1; addr > codeAddr - 16384; addr--) {
            if (IsPrologue(addr)) {
                return addr;
            }
        }
        return 0;
    }
};
```

#### Known String Anchors

| Function | String Anchor | Length |
|----------|---------------|--------|
| CharacterSpawn | `"[RootObjectFactory::process] Character"` | 38 |
| CharacterDeath | `"{1} has died from blood loss."` | 29 |
| ApplyDamage | `"Attack damage effect"` | 20 |
| StartAttack | `"Cutting damage"` | 14 |
| HealthUpdate | `"block chance"` | 12 |
| TimeUpdate | `"timeScale"` | 9 |
| LoadGame | `"[SaveManager::loadGame] No towns loaded."` | 40 |
| FactionRelation | `"faction relation"` | 16 |
| AICreate | `"[AI::create] No faction for"` | 27 |

Full list: `KenshiMP.Scanner/include/kmp/patterns.h` → `STRING_ANCHORS[]`

### Step 2: Static Byte Pattern Matching

**Fallback method** — fast but brittle across versions.

#### Pattern Format

IDA-style notation with wildcards:
```
48 8B C4                  # mov rax, rsp
55                        # push rbp
56                        # push rsi
57                        # push rdi
41 54                     # push r12
48 8D A8 ?? ?? ?? ??      # lea rbp, [rax-0x????]  (wildcard displacement)
48 81 EC ?? ?? ?? ??      # sub rsp, 0x????        (wildcard stack size)
```

Wildcards (`??`) cover:
- Stack frame sizes (change with compiler optimizations)
- LEA/MOV displacement operands (RIP-relative offsets)
- Immediate values (constants, offsets)

#### Pattern Definition

```cpp
// KenshiMP.Scanner/include/kmp/patterns.h

namespace patterns {

// CharacterSpawn - RootObjectFactory::process
// RVA: 0x00581770 (Steam v1.0.68)
constexpr const char* CHARACTER_SPAWN = 
    "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8D A8 ?? ?? ?? ?? "  // lea rbp, [rax-FRAMESIZE]
    "48 81 EC ?? ?? ?? ?? "  // sub rsp, STACKSIZE
    "48 C7 45 C0";

// CharacterDeath - Character::Death
// RVA: 0x007A6200 (Steam v1.0.68)
constexpr const char* CHARACTER_DEATH =
    "48 8B C4 55 57 41 54 41 55 41 56 "
    "48 8D A8 28 FE FF FF "  // lea rbp, [rax-0x1D8]
    "48 81 EC B0 02 00 00 "  // sub rsp, 0x2B0
    "48 C7 44 24 28 FE FF";

} // namespace patterns
```

#### Pattern Scanner Implementation

```cpp
// KenshiMP.Scanner/src/scanner_engine.cpp

class ScannerEngine {
public:
    uintptr_t ScanPattern(const char* pattern) {
        std::vector<uint8_t> bytes;
        std::vector<bool> mask;
        ParsePattern(pattern, bytes, mask);
        
        // SSE2 accelerated scan
        return ScanBytes(m_textBase, m_textSize, bytes.data(), mask.data(), bytes.size());
    }

private:
    void ParsePattern(const char* pattern, 
                     std::vector<uint8_t>& bytes,
                     std::vector<bool>& mask) {
        const char* p = pattern;
        while (*p) {
            if (*p == ' ') { p++; continue; }
            if (*p == '?') {
                bytes.push_back(0);
                mask.push_back(false);  // Wildcard
                p += (p[1] == '?') ? 2 : 1;
            } else {
                uint8_t byte = (HexValue(p[0]) << 4) | HexValue(p[1]);
                bytes.push_back(byte);
                mask.push_back(true);  // Must match
                p += 2;
            }
        }
    }
    
    // SSE2 SIMD scan (10x faster than byte-by-byte)
    uintptr_t ScanBytes(uintptr_t base, size_t size, 
                        const uint8_t* pattern, const bool* mask, size_t len);
};
```

### Step 3: VTable Slot Discovery

**For virtual methods** — resolves functions not in .pdata or without patterns.

Example: `ActivePlatoon::addMember` (squad management)

#### Theory

C++ virtual methods are called via vtable:
```cpp
class ActivePlatoon {
    virtual void update();
    virtual void addMember(Character* member);  // Slot 2
    virtual void removeMember(Character* member);
};

// Compiled as:
mov rax, [rcx]         ; Load vtable from object's first 8 bytes
call [rax+0x10]        ; Call slot 2 (addMember) at vtable+0x10
```

At runtime, we can:
1. Find an `ActivePlatoon` object in memory (via character squad pointer)
2. Read its vtable address from offset +0x00
3. Read slot 2 function pointer at vtable+0x10
4. Validate it's a valid function using .pdata

#### Implementation

```cpp
// KenshiMP.Scanner/src/vtable_scanner.cpp

class VTableScanner {
public:
    uintptr_t FindVTableSlot(const char* className, int slot) {
        // 1. Find vtable in .rdata via runtime object discovery
        uintptr_t vtableAddr = FindVTableByClassName(className);
        if (!vtableAddr) return 0;
        
        // 2. Read function pointer at slot
        uintptr_t funcAddr = 0;
        __try {
            funcAddr = *reinterpret_cast<uintptr_t*>(vtableAddr + slot * 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        
        // 3. Validate it's in .text and has .pdata entry
        if (funcAddr < m_textBase || funcAddr >= m_textBase + m_textSize) {
            return 0;
        }
        
        DWORD64 imageBase = 0;
        auto* rtFunc = RtlLookupFunctionEntry(funcAddr, &imageBase, nullptr);
        if (!rtFunc) return 0;
        
        return funcAddr;
    }
};
```

---

## Validation Using .pdata

### What is .pdata?

The **.pdata section** (Exception Data Directory) contains a table of `RUNTIME_FUNCTION` structures for every function with a stack frame. Windows uses this for:
- **Exception unwinding** (C++ `throw/catch`, SEH)
- **Stack traces** (debuggers, crash dumps)
- **Function boundary detection** (disassemblers)

### RUNTIME_FUNCTION Structure

```cpp
struct RUNTIME_FUNCTION {
    DWORD BeginAddress;     // RVA of function start
    DWORD EndAddress;       // RVA of function end (exclusive)
    DWORD UnwindInfoAddress;  // RVA of UNWIND_INFO structure
};
```

Example from kenshi_x64.exe:
```
BeginAddress=0x00581770, EndAddress=0x00582D5A, UnwindInfoAddress=0x0192C3E4
→ Function at RVA 0x581770, size 0x15EA (5610 bytes)
```

### Validation Workflow

```cpp
// KenshiMP.Scanner/src/pdata_enumerator.cpp

class PDataEnumerator {
public:
    bool IsValidFunctionStart(uintptr_t addr) const {
        DWORD64 imageBase = 0;
        auto* rtFunc = RtlLookupFunctionEntry(
            static_cast<DWORD64>(addr), &imageBase, nullptr);
        
        if (!rtFunc) return false;
        
        // Ensure addr is exactly at function start (not mid-function)
        uintptr_t funcStart = static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
        return (funcStart == addr);
    }
    
    FunctionInfo GetFunctionInfo(uintptr_t addr) const {
        DWORD64 imageBase = 0;
        auto* rtFunc = RtlLookupFunctionEntry(addr, &imageBase, nullptr);
        
        FunctionInfo info;
        info.startRVA = rtFunc->BeginAddress;
        info.endRVA = rtFunc->EndAddress;
        info.size = rtFunc->EndAddress - rtFunc->BeginAddress;
        info.startVA = static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
        info.endVA = static_cast<uintptr_t>(imageBase) + rtFunc->EndAddress;
        return info;
    }
};
```

### Common Validation Errors

#### Error 1: Mid-Function Match
**Symptom:** Pattern matches bytes inside a function, not at the start.

```
Pattern scan found: 0x007A33AA (inside ApplyDamage at +0x0A)
.pdata lookup:      0x007A33A0 (actual function start)
```

**Fix:** Auto-correct to .pdata function start:
```cpp
uintptr_t addr = ScanPattern(pattern);
if (addr) {
    DWORD64 imageBase = 0;
    auto* rtFunc = RtlLookupFunctionEntry(addr, &imageBase, nullptr);
    if (rtFunc) {
        uintptr_t funcStart = imageBase + rtFunc->BeginAddress;
        if (funcStart != addr && (addr - funcStart) <= 0x10) {
            // Pattern matched near start, auto-correct
            addr = funcStart;
        } else if (addr - funcStart > 0x10) {
            // Pattern matched deep inside function, reject
            addr = 0;
        }
    }
}
```

#### Error 2: Alignment Check False Positive
**Symptom:** Function is valid per .pdata but not 16-byte aligned.

```
SquadAddMember: RVA=0x00928423 (valid in .pdata)
Alignment check: 0x00928423 & 0xF = 0x3 → REJECTED
```

**Fix:** Trust .pdata over alignment heuristic:
```cpp
// patterns.cpp old code (WRONG):
if ((addr & 0xF) != 0) {
    return 0;  // Reject non-aligned
}

// Fixed code:
if ((addr & 0xF) != 0) {
    // Warn but don't reject if .pdata confirms it
    if (!IsValidFunctionStart(addr)) {
        return 0;
    }
    spdlog::warn("Function at 0x{:X} is not 16-byte aligned but .pdata confirms it", addr);
}
```

#### Error 3: Backward Walk Too Short
**Symptom:** String xref is >2048 bytes from function start, FindFunctionStart fails.

```
StartAttack function size: 9253 bytes
String "Cutting damage" xref: +0xC82 from function start
Backward walk limit: 2048 bytes → MISS
```

**Fix:** Use .pdata lookup first (primary), backward walk second (fallback):
```cpp
uintptr_t FindFunctionStart(uintptr_t codeAddr) const {
    // Method 1: .pdata (authoritative, works for any function size)
    DWORD64 imageBase = 0;
    auto* rtFunc = RtlLookupFunctionEntry(codeAddr, &imageBase, nullptr);
    if (rtFunc) {
        return static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
    }
    
    // Method 2: Backward walk (fallback, increased to 16KB)
    for (uintptr_t addr = codeAddr - 1; addr > codeAddr - 16384; addr--) {
        if (IsPrologue(addr)) {
            return addr;
        }
    }
    return 0;
}
```

---

## Function Resolution Architecture

### Orchestrator System

The **PatternOrchestrator** tries multiple resolution strategies in priority order:

```cpp
// KenshiMP.Scanner/src/orchestrator.cpp

enum class ResolutionMethod {
    None,
    PatternScan,       // Static byte pattern
    StringXref,        // String cross-reference (PRIMARY)
    VTableSlot,        // Virtual method vtable lookup
    CallGraphTrace,    // Follow CALL instructions from known function
    HardcodedOffset,   // Fallback RVA with validation
    PDataLookup,       // Direct .pdata enumeration
    Manual             // User-provided address
};

class PatternOrchestrator {
public:
    void Resolve(PatternEntry& entry) {
        // Priority 1: String anchor (most reliable)
        if (entry.stringAnchor && entry.stringAnchorLen > 0) {
            uintptr_t addr = m_strings.FindFunctionByString(
                entry.stringAnchor, entry.stringAnchorLen);
            if (ValidateAndStore(entry, addr, ResolutionMethod::StringXref)) {
                return;
            }
        }
        
        // Priority 2: Static pattern (version-specific)
        if (entry.pattern) {
            uintptr_t addr = m_scanner.ScanPattern(entry.pattern);
            if (ValidateAndStore(entry, addr, ResolutionMethod::PatternScan)) {
                return;
            }
        }
        
        // Priority 3: VTable slot (virtual methods)
        if (!entry.vtableClass.empty() && entry.vtableSlot >= 0) {
            uintptr_t addr = m_vtables.FindVTableSlot(
                entry.vtableClass.c_str(), entry.vtableSlot);
            if (ValidateAndStore(entry, addr, ResolutionMethod::VTableSlot)) {
                return;
            }
        }
        
        // Priority 4: Hardcoded RVA with .pdata validation
        if (entry.hardcodedRVA != 0) {
            uintptr_t addr = m_scanner.GetBase() + entry.hardcodedRVA;
            if (ValidateAndStore(entry, addr, ResolutionMethod::HardcodedOffset)) {
                return;
            }
        }
        
        // FAILED - log for manual investigation
        spdlog::error("Failed to resolve '{}': {}", entry.id, entry.description);
    }

private:
    bool ValidateAndStore(PatternEntry& entry, uintptr_t addr, ResolutionMethod method) {
        if (!addr) return false;
        
        // .pdata validation
        if (!m_pdata.IsValidFunctionStart(addr)) {
            spdlog::warn("Address 0x{:X} for '{}' failed .pdata validation", addr, entry.id);
            return false;
        }
        
        // Store result
        if (entry.targetPtr) {
            *entry.targetPtr = reinterpret_cast<void*>(addr);
        }
        entry.resolvedAddress = addr;
        entry.method = method;
        entry.resolved = true;
        
        spdlog::info("Resolved '{}' → 0x{:X} via {}", 
                     entry.id, addr, ResolutionMethodName(method));
        return true;
    }
    
    ScannerEngine      m_scanner;
    StringAnalyzer     m_strings;
    VTableScanner      m_vtables;
    PDataEnumerator    m_pdata;
    CallGraphAnalyzer  m_callGraph;
};
```

### Resolution Statistics (Steam v1.0.68)

| Method | Success Rate | Functions |
|--------|--------------|-----------|
| StringXref | 95% (38/40) | CharacterSpawn, ApplyDamage, CharacterDeath, etc. |
| PatternScan | 85% (34/40) | Fallback when string missing |
| VTableSlot | 100% (1/1) | SquadAddMember |
| Hardcoded | 100% (7/7) | CharacterDeath, TimeUpdate, LoadGame (corrected RVAs) |

**Total resolved:** 40/40 critical functions (100%)

---

## Known Function Offsets

### Verified Function Map (Steam v1.0.68)

| Function | RVA | Size | Prologue | String Anchor | Status |
|----------|-----|------|----------|---------------|--------|
| **CharacterSpawn** | 0x00581770 | 6410 | MovRaxRsp | `[RootObjectFactory::process] Character` | ✅ |
| **ApplyDamage** | 0x007A33A0 | 6925 | MovRaxRsp | `Attack damage effect` | ✅ (was marked broken) |
| **StartAttack** | 0x007B2A20 | 9253 | MovRaxRsp | `Cutting damage` | ✅ |
| **HealthUpdate** | 0x0086B2B0 | 7381 | MovRaxRsp | `block chance` | ✅ |
| **CharacterDeath** | **0x007A6200** | 1841 | MovRaxRsp | `{1} has died from blood loss.` | ✅ (corrected) |
| **CharacterKO** | 0x00345C10 | 129 | Standard | `knockout` | ✅ |
| **AICreate** | 0x00622110 | 313 | Standard | `[AI::create] No faction for` | ✅ |
| **AIPackages** | 0x00271620 | 196 | Standard | `AI packages` | ✅ |
| **TimeUpdate** | **0x00214B50** | 50 | Standard | `timeScale` | ✅ (corrected) |
| **LoadGame** | **0x00373F00** | 4598 | Standard | `[SaveManager::loadGame]...` | ✅ (corrected) |
| **ItemPickup** | **0x0074C8B0** | 1257 | MovRaxRsp | `addItem` | ✅ (corrected) |
| **ItemDrop** | **0x00745DE0** | 60 | Standard | `removeItem` | ✅ (corrected) |
| **FactionRelation** | **0x00872E00** | 6951 | MovRaxRsp | `faction relation` | ✅ (corrected) |
| **SquadCreate** | **0x00480B50** | 1490 | MovRaxRsp | `Reset squad positions` | ✅ (corrected) |
| **SquadAddMember** | 0x00928423 | 196 | Standard | VTable slot 2 | ✅ (VTable) |
| **BuildingRepair** | 0x005C9E70 | 129 | Standard | `construction progress` | ✅ |

### 7 Functions with WRONG Old RVAs

The following functions had **megabyte-scale address errors** from GOG/old patterns:

| Function | Correct (Steam) | Old/GOG (Wrong) | Delta |
|----------|-----------------|-----------------|-------|
| CharacterDeath | 0x007A6200 | 0x0080E6F0 | +0x684F0 |
| TimeUpdate | 0x00214B50 | 0x00563530 | +0x34E9E0 |
| LoadGame | 0x00373F00 | 0x0056F320 | +0x1FB420 |
| ItemPickup | 0x0074C8B0 | 0x008ED010 | +0x1A0760 |
| ItemDrop | 0x00745DE0 | 0x008ED1D0 | +0x1A73F0 |
| FactionRelation | 0x00872E00 | 0x0090BC10 | +0x98E10 |
| SquadCreate | 0x00480B50 | 0x00928470 | +0x4A7920 |

**Root cause:** Old patterns were GOG-specific byte sequences that don't exist in Steam binary. String anchors work for both.

---

## Steam vs GOG Differences

### Key Differences

| Aspect | Steam v1.0.68 | GOG v1.0.68 |
|--------|---------------|-------------|
| **Binary size** | ~35.2 MB | ~35.1 MB |
| **ImageBase** | 0x140000000 | 0x140000000 (same) |
| **.text RVA** | 0x1000 | 0x1000 (same) |
| **Function RVAs** | Different (+/- megabytes) | Different |
| **Struct offsets** | Identical | Identical |
| **String locations** | Different (shuffled .rdata) | Different |
| **Patterns** | 60% match GOG | 60% match Steam |
| **String anchors** | 95% work on both | 95% work on both |

### Compatibility Strategy

1. **Use string anchors** for critical functions (CharacterSpawn, ApplyDamage, etc.)
2. **Maintain two pattern sets** in code (Steam/GOG variants)
3. **Version detection** via PE file version resource:
   ```cpp
   std::string DetectGameVersion() {
       // Read VS_VERSION_INFO from PE resources
       // Returns "1.0.68.0" (Steam) or "1.0.68.0" (GOG) + build timestamp
   }
   ```
4. **Fallback to hardcoded RVAs** only after string+pattern fail
5. **Runtime .pdata validation** catches wrong-version addresses

### GOG Support Status

- ✅ CharacterSpawn, ApplyDamage, CharacterDeath (string anchors work)
- ✅ TimeUpdate, LoadGame, FactionRelation (string anchors work)
- ⚠️  BuildingRepair, SquadAddMember (need GOG-specific patterns or RVAs)
- ❌ CharacterMoveTo (disabled on both — see MovRaxRsp section)

**Recommendation:** Test on GOG after each major RE update to maintain dual support.

---

## Hook Installation Process

### Overview

Hooks intercept game functions to inject multiplayer logic. Process:

1. **Resolve function addresses** (see above)
2. **Choose hook method** (MinHook trampoline or MovRaxRsp naked detour)
3. **Install hooks** via HookManager
4. **Handle reentrancy** (hook calling original, original calling hook)

### HookManager Interface

```cpp
// KenshiMP.Scanner/include/kmp/hook_manager.h

class HookManager {
public:
    // Standard trampoline hook (for most functions)
    bool InstallHook(void* target, void* detour, void** original) {
        MH_STATUS status = MH_CreateHook(target, detour, original);
        if (status != MH_OK) return false;
        
        status = MH_EnableHook(target);
        return (status == MH_OK);
    }
    
    // MovRaxRsp naked detour hook (for MSVC large-frame functions)
    bool InstallMovRaxRspHook(void* target, void* detour, void** original,
                              void** rawTrampoline = nullptr,
                              void** directCallStub = nullptr) {
        return MovRaxRspFix::InstallHook(target, detour, original, 
                                         rawTrampoline, directCallStub);
    }
    
    // Reentrancy bypass (call original without triggering hook)
    void SetBypass(void* original, bool bypass);
};
```

### Example: CharacterSpawn Hook

```cpp
// KenshiMP.Core/hooks/entity_hooks.cpp

// Function signature (reverse-engineered)
using CharacterCreateFn = void*(__fastcall*)(void* factory, void* templateData);

static CharacterCreateFn s_origCreate = nullptr;
static CharacterCreateFn s_rawCreateTrampoline = nullptr;
static CharacterCreateFn s_directCallStub = nullptr;

// Hook detour
void* __fastcall Hook_CharacterCreate(void* factory, void* templateData) {
    // Call original (via MovRaxRsp trampoline wrapper)
    void* character = s_origCreate(factory, templateData);
    
    // Post-spawn logic: check if this is a network spawn request
    if (SpawnManager::Get().IsPendingNetworkSpawn()) {
        // Hijack this NPC for remote player
        SpawnManager::Get().HijackForRemotePlayer(character);
    }
    
    return character;
}

// Installation (called from Core::Initialize)
void entity_hooks::Init(const GameFunctions& funcs) {
    if (!funcs.CharacterSpawn) {
        spdlog::error("CharacterSpawn not resolved, cannot install hook");
        return;
    }
    
    // CharacterSpawn has `mov rax, rsp` prologue → use MovRaxRsp fix
    bool success = HookManager::Get().InstallMovRaxRspHook(
        funcs.CharacterSpawn,              // Target
        (void*)Hook_CharacterCreate,       // Detour
        (void**)&s_origCreate,             // Trampoline wrapper
        (void**)&s_rawCreateTrampoline,    // Raw trampoline (for reentrancy)
        (void**)&s_directCallStub          // Direct call stub
    );
    
    if (success) {
        spdlog::info("CharacterSpawn hook installed at 0x{:X}", 
                     reinterpret_cast<uintptr_t>(funcs.CharacterSpawn));
    } else {
        spdlog::error("CharacterSpawn hook installation failed");
    }
}
```

---

## MovRaxRsp Prologue Fix

### The Problem

MSVC x64 generates a specific prologue for functions with large stack frames:

```asm
mov rax, rsp          ; 48 8B C4 — save RSP before pushes
push rbp              ; 55
push rsi              ; 56
push rdi              ; 57
push r12              ; 41 54
push r13              ; 41 55
lea rbp, [rax-0x150]  ; Derive frame pointer from RAX (pre-push RSP!)
sub rsp, 0x200        ; Allocate local variables
```

**Key insight:** The compiler **aliases** push-saved slots with `[rbp+offset]` frame slots. For example:
```
[rbp+0x150] == push rbp slot  (same memory!)
[rbp+0x158] == push rsi slot
[rbp+0x160] == push rdi slot
```

This works because `rbp = rax - 0x150`, and `rax` was captured **before** the pushes. The pushes decrement RSP, but RBP is derived from the **original** RSP.

**Standard MinHook trampoline:**
```asm
# MinHook JMPs to trampoline, which starts with:
mov rax, rsp          ; Copy of original prologue
push rbp
# ... etc
```

When you CALL this trampoline from C++ hook code:
```cpp
void* result = s_origCreate(factory, templateData);
```

The CALL instruction pushes a return address onto the stack. Now:
```
RSP = hook_caller_rsp - 8     (CALL pushed return address)
RAX = RSP = hook_caller_rsp - 8   (mov rax, rsp copies wrong value!)
```

The function executes with `rbp = rax - 0x150`, but RAX is now **8 bytes off** from what the compiler expected. Every `[rbp+offset]` access reads/writes the wrong memory. Result: **heap corruption, crashes, sign-extension bugs**.

### The Solution: Naked Detour + Trampoline Wrapper

We generate two custom ASM stubs:

#### 1. Naked Detour (MinHook JMPs here)

```asm
; Save game's RSP and return address
mov [captured_rsp], rsp
mov r11, [rsp]
mov [saved_game_ret], r11

; Create stack gap (4KB+8) to isolate C++ hook frame
sub rsp, 0x1008

; Call C++ hook (not JMP!)
mov r11, <cpp_hook_addr>
call r11

; Restore stack and return
add rsp, 0x1008
mov r11, [saved_game_ret]
mov [rsp], r11
ret
```

Why CALL (not JMP)? The trampoline wrapper will patch `[captured_rsp]` to point to its internal return point. If we JMP to the hook, the hook's RET would pop that patched address and jump back to the wrapper a second time (infinite loop). CALL ensures the hook returns to us, and we restore the original return address before our final RET.

#### 2. Trampoline Wrapper (C++ hook calls this as "original")

```asm
; Save C++ hook's RSP
mov [stub_rsp], rsp

; Swap to game's stack (where push slots must land)
mov rsp, [captured_rsp]

; Patch [rsp] to return to us (not game caller)
lea r11, [rip+return_point]
mov [rsp], r11

; Set RAX = RSP (what function expects!)
mov rax, rsp

; Enter function body (skip original's mov rax,rsp)
jmp trampoline+3

return_point:
; Swap back to C++ hook's stack
mov rsp, [stub_rsp]
ret
```

**Result:** The original function executes with:
- `RSP = captured_rsp` (game caller's RSP after CALL)
- `RAX = captured_rsp` (correct value for frame calculations)
- **ZERO extra bytes** on stack → push slots align perfectly with `[rbp+offset]`

### Implementation

```cpp
// KenshiMP.Scanner/src/mov_rax_rsp_fix.cpp

namespace MovRaxRspFix {

bool InstallHook(void* target, void* detour, void** outTrampolineWrapper,
                 void** outRawTrampoline, void** outDirectCallStub) {
    // 1. Create MinHook trampoline (standard)
    void* rawTrampoline = nullptr;
    MH_STATUS status = MH_CreateHook(target, detour, &rawTrampoline);
    if (status != MH_OK) return false;
    
    // 2. Allocate page for stubs + shared data
    void* page = VirtualAlloc(nullptr, 0x200, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    if (!page) return false;
    
    // 3. Build naked detour stub
    uint8_t* nakedStub = (uint8_t*)page + 0x40;
    BuildNakedDetourStub(nakedStub, page, detour);
    
    // 4. Build trampoline wrapper stub
    uint8_t* wrapperStub = (uint8_t*)page + 0xC0;
    BuildTrampolineWrapper(wrapperStub, page, rawTrampoline);
    
    // 5. Replace MinHook detour with our naked stub
    MH_EnableHook(target);  // Standard enable
    // ... then patch MinHook's JMP to point to nakedStub instead of detour
    
    // 6. Return wrapper as "original" for C++ hook to call
    *outTrampolineWrapper = (void*)wrapperStub;
    if (outRawTrampoline) *outRawTrampoline = rawTrampoline;
    if (outDirectCallStub) *outDirectCallStub = BuildDirectCallStub(page, rawTrampoline);
    
    return true;
}

void BuildNakedDetourStub(uint8_t* buf, void* page, void* detour) {
    int off = 0;
    
    // mov [page+0x00], rsp (captured_rsp)
    EmitByte(buf, off, 0x48); EmitByte(buf, off, 0x89);
    EmitByte(buf, off, 0x24); EmitByte(buf, off, 0x25);
    EmitU32(buf, off, (uint32_t)(uintptr_t)page);
    
    // mov r11, [rsp]
    EmitByte(buf, off, 0x4C); EmitByte(buf, off, 0x8B);
    EmitByte(buf, off, 0x1C); EmitByte(buf, off, 0x24);
    
    // mov [page+0x10], r11 (saved_game_ret)
    EmitByte(buf, off, 0x4C); EmitByte(buf, off, 0x89);
    EmitByte(buf, off, 0x1C); EmitByte(buf, off, 0x25);
    EmitU32(buf, off, (uint32_t)((uintptr_t)page + 0x10));
    
    // sub rsp, 0x1008
    EmitByte(buf, off, 0x48); EmitByte(buf, off, 0x81);
    EmitByte(buf, off, 0xEC);
    EmitU32(buf, off, 0x1008);
    
    // mov r11, <detour>
    EmitByte(buf, off, 0x49); EmitByte(buf, off, 0xBB);
    EmitU64(buf, off, (uint64_t)detour);
    
    // call r11
    EmitByte(buf, off, 0x41); EmitByte(buf, off, 0xFF);
    EmitByte(buf, off, 0xD3);
    
    // add rsp, 0x1008
    EmitByte(buf, off, 0x48); EmitByte(buf, off, 0x81);
    EmitByte(buf, off, 0xC4);
    EmitU32(buf, off, 0x1008);
    
    // mov r11, [page+0x10] (saved_game_ret)
    EmitByte(buf, off, 0x4C); EmitByte(buf, off, 0x8B);
    EmitByte(buf, off, 0x1C); EmitByte(buf, off, 0x25);
    EmitU32(buf, off, (uint32_t)((uintptr_t)page + 0x10));
    
    // mov [rsp], r11
    EmitByte(buf, off, 0x4C); EmitByte(buf, off, 0x89);
    EmitByte(buf, off, 0x1C); EmitByte(buf, off, 0x24);
    
    // ret
    EmitByte(buf, off, 0xC3);
}

void BuildTrampolineWrapper(uint8_t* buf, void* page, void* rawTrampoline) {
    int off = 0;
    
    // mov [page+0x08], rsp (stub_rsp)
    EmitByte(buf, off, 0x48); EmitByte(buf, off, 0x89);
    EmitByte(buf, off, 0x24); EmitByte(buf, off, 0x25);
    EmitU32(buf, off, (uint32_t)((uintptr_t)page + 0x08));
    
    // mov rsp, [page+0x00] (captured_rsp)
    EmitByte(buf, off, 0x48); EmitByte(buf, off, 0x8B);
    EmitByte(buf, off, 0x24); EmitByte(buf, off, 0x25);
    EmitU32(buf, off, (uint32_t)(uintptr_t)page);
    
    // lea r11, [rip+return_point]
    EmitByte(buf, off, 0x4C); EmitByte(buf, off, 0x8D);
    EmitByte(buf, off, 0x1D);
    int returnPointOff = 25;  // Will be filled after we know stub size
    EmitU32(buf, off, returnPointOff);
    
    // mov [rsp], r11
    EmitByte(buf, off, 0x4C); EmitByte(buf, off, 0x89);
    EmitByte(buf, off, 0x1C); EmitByte(buf, off, 0x24);
    
    // mov rax, rsp
    EmitByte(buf, off, 0x48); EmitByte(buf, off, 0x8B);
    EmitByte(buf, off, 0xC4);
    
    // jmp rawTrampoline+3
    EmitByte(buf, off, 0xE9);
    int32_t jmpDisp = (int32_t)((uintptr_t)rawTrampoline + 3 - ((uintptr_t)buf + off + 4));
    EmitU32(buf, off, (uint32_t)jmpDisp);
    
    // return_point:
    int returnPointStart = off;
    
    // mov rsp, [page+0x08] (stub_rsp)
    EmitByte(buf, off, 0x48); EmitByte(buf, off, 0x8B);
    EmitByte(buf, off, 0x24); EmitByte(buf, off, 0x25);
    EmitU32(buf, off, (uint32_t)((uintptr_t)page + 0x08));
    
    // ret
    EmitByte(buf, off, 0xC3);
    
    // Patch return_point displacement (was placeholder earlier)
    int32_t actualDisp = returnPointStart - (returnPointOff - 4);
    std::memcpy(buf + (returnPointOff - 4), &actualDisp, 4);
}

} // namespace MovRaxRspFix
```

### Which Functions Need This?

Check the first 3 bytes of the function:
```cpp
bool NeedsMovRaxRspFix(void* funcAddr) {
    uint8_t* p = (uint8_t*)funcAddr;
    return (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4);  // mov rax, rsp
}
```

**Kenshi functions requiring fix (Steam v1.0.68):**
- CharacterSpawn (0x581770)
- ApplyDamage (0x7A33A0)
- StartAttack (0x7B2A20)
- HealthUpdate (0x86B2B0)
- CharacterDeath (0x7A6200)
- ItemPickup (0x74C8B0)
- ItemDrop (0x745DE0)
- FactionRelation (0x872E00)
- SquadCreate (0x480B50)

---

## Common Pitfalls

### 1. Sign Extension Bugs

**Symptom:** Pointers become huge (0xFFFFFFFF12345678) and crash when dereferenced.

**Cause:** Reading a 32-bit value from memory and treating it as a pointer:
```cpp
int32_t factionId = *(int32_t*)(character + 0x10);  // Read 4 bytes
Faction* faction = (Faction*)factionId;  // WRONG: sign-extends if bit 31 set!
```

**Fix:** Read 8 bytes as `uintptr_t` or `void*`:
```cpp
void* faction = *(void**)(character + 0x10);  // Read 8 bytes
```

### 2. Hook Reentrancy Corruption

**Symptom:** Crash on second call to hook during a hooked function's execution.

**Cause:** MovRaxRsp stub's global data slots (captured_rsp, stub_rsp) are shared across all calls.

**Fix:** Use raw trampoline for reentrant calls:
```cpp
void* Hook_CharacterCreate(void* factory, void* templateData) {
    if (IsReentrantCall()) {
        // Bypass MovRaxRsp wrapper entirely
        return s_rawCreateTrampoline(factory, templateData);
    }
    
    void* character = s_origCreate(factory, templateData);  // Normal path
    // ...
    return character;
}
```

### 3. Loading Detection False Positives

**Symptom:** Hooks trigger during game load, causing save corruption or crashes.

**Cause:** Hook modifies game state during deserialization.

**Fix:** Detect loading via create burst timing:
```cpp
bool IsLoading() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - s_lastCreateTime).count();
    
    // Loading = 50+ creates within 2 seconds
    return (s_createBurstCount > 50 && elapsed < 2000);
}

void* Hook_CharacterCreate(void* factory, void* templateData) {
    if (IsLoading()) {
        // Passthrough mode: don't modify anything
        return s_origCreate(factory, templateData);
    }
    
    // Connected mode: inject multiplayer logic
    // ...
}
```

### 4. Struct Offset Drift

**Symptom:** Offset works on player character but crashes on NPCs, or vice versa.

**Cause:** Reading a field that's only present in derived classes:
```cpp
// Character base class: +0x10 = faction
// CharacterHuman derived: +0x450 = stats
// Animal derived: NO stats field!

float strength = *(float*)(character + 0x450);  // Crashes on animals
```

**Fix:** Type-check before accessing derived fields:
```cpp
enum class CharacterType { Human, Animal, Unknown };

CharacterType GetType(void* character) {
    void* vtable = *(void**)character;
    // Check vtable against known vtable addresses
    if (vtable == g_humanVTable) return CharacterType::Human;
    if (vtable == g_animalVTable) return CharacterType::Animal;
    return CharacterType::Unknown;
}

float GetStrength(void* character) {
    if (GetType(character) != CharacterType::Human) {
        return 0.0f;  // Animals have no stats
    }
    return *(float*)(character + 0x450);
}
```

### 5. MSVC String SSO (Small String Optimization)

**Symptom:** Name reads 16 bytes correctly but crashes on long strings.

**Cause:** MSVC's `std::string` uses SSO (Small String Optimization):
```cpp
// std::string layout (32 bytes total):
struct string {
    union {
        char shortBuf[16];   // SSO: strings <= 15 bytes stored inline
        char* ptr;           // Heap: strings > 15 bytes allocated on heap
    };
    size_t length;
    size_t capacity;
};
```

**Fix:** Always use the string class's methods (via vtable) or read length first:
```cpp
std::string ReadName(void* character) {
    auto* str = (std::string*)(character + 0x18);
    size_t len = str->length;
    
    if (len <= 15) {
        // SSO: data is inline
        return std::string(str->shortBuf, len);
    } else {
        // Heap: follow pointer
        return std::string(str->ptr, len);
    }
}
```

---

## IDA Pro Workflow

### Setup

1. Open `kenshi_x64.exe` in IDA Pro 7.5+
2. Wait for auto-analysis (15-30 minutes for ~35MB binary)
3. Load .pdata: `File → Load File → Additional binary file... → .pdata section`
4. Set ImageBase: `Edit → Segments → Rebase program... → 0x140000000`

### Finding Functions via String Xrefs

1. **Open Strings Window:** `View → Open subviews → Strings` (Shift+F12)
2. **Search for anchor string:** Ctrl+F, type `"[RootObjectFactory::process] Character"`
3. **Double-click string** to jump to .rdata location
4. **Find xrefs:** Press `X`, see list of code locations that reference this string
5. **Jump to xref:** Double-click the code xref
6. **Find function start:** Press `P` to convert to function, or scroll up to find prologue

Example: CharacterSpawn
```
.rdata:0000000141673ABC aRootobjectfact db '[RootObjectFactory::process] Character ',27h,0

Code xref from .text:
.text:0000000140582324   lea   rcx, aRootobjectfact  ; "[RootObjectFactory::process]..."
.text:000000014058232B   call  Logger_log

Function start (0x140581770):
.text:0000000140581770   mov   rax, rsp
.text:0000000140581773   push  rbp
.text:0000000140581774   push  rsi
.text:0000000140581775   push  rdi
```

### Validating with .pdata

1. **Jump to suspected address:** Press `G`, enter `0x140581770`
2. **Check .pdata:** In Hex View, navigate to .pdata section
3. **Search for RVA:** Find `RUNTIME_FUNCTION` entry with `BeginAddress = 0x00581770`
4. **Verify function size:** `EndAddress - BeginAddress` should match IDA's function size

### Analyzing Function Parameters

1. **Set function signature:** Press `Y` at function start, enter:
   ```cpp
   void* __fastcall CharacterSpawn(void* factory, void* templateData);
   ```
2. **Watch register usage:**
   - `RCX` = first parameter (factory)
   - `RDX` = second parameter (templateData)
   - `R8`, `R9` = third/fourth parameters
   - `[RSP+0x20]`, `[RSP+0x28]`... = fifth+ parameters
3. **Rename registers:** Right-click `rcx` → Rename, type `factory_ptr`
4. **Track pointer chains:** Follow `mov rax, [rcx+0x10]` to find struct offsets

### Exporting Patterns

1. **Select function prologue:** Click first instruction, Shift+click 32 bytes later
2. **Copy hex:** `Edit → Copy → Copy as hex bytes`
3. **Create pattern:** Paste into text editor, replace variable bytes with `??`:
   ```
   48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ??
   ```
4. **Test pattern:** Use pattern scanner to verify uniqueness

### Call Graph Analysis

1. **Find function calls:** In CharacterSpawn, find `call <address>`
2. **Jump to callee:** Double-click call target
3. **Identify helper functions:** Look for small functions called multiple times
4. **Name callees:** Press `N`, give meaningful names (e.g., `ValidateTemplateData`)

---

## Future Function Discovery

### Process for New Functions

1. **Identify gameplay system** to reverse engineer (e.g., crafting, trading)
2. **Find candidate strings:** Search for UI text, log messages, error strings
3. **Run string scanner:**
   ```cpp
   RuntimeStringScanner scanner(moduleBase, moduleSize);
   uintptr_t addr = scanner.FindFunctionByString("Crafting item", 13);
   ```
4. **Validate with .pdata:**
   ```cpp
   if (!IsValidFunctionStart(addr)) {
       spdlog::error("String xref landed mid-function, needs manual analysis");
   }
   ```
5. **Test in-game:** Install hook, log calls, verify it triggers at expected times
6. **Reverse parameter types:** Use IDA/x64dbg to watch registers/stack during calls
7. **Add to patterns.h:**
   ```cpp
   constexpr const char* CRAFTING_ITEM = "48 8B C4 55 56 57 41 54 ...";
   constexpr StringAnchor ANCHORS[] = {
       // ...
       {"CraftingItem", "Crafting item", 13},
   };
   ```

### Tools for Dynamic Analysis

**x64dbg breakpoint workflow:**
1. Attach to `kenshi_x64.exe`
2. Set breakpoint: `bp kenshi_x64.exe+0x581770`
3. Trigger in-game (e.g., recruit a character)
4. Breakpoint hits → inspect registers, stack, call stack
5. Step through with F8 (step over) / F7 (step into)
6. Watch memory: Right-click register → Follow in Dump

**Cheat Engine pointer chain discovery:**
1. Search for known value (e.g., player health = 100.0)
2. Find base address of character object
3. "Pointer scan for this address" → save results
4. Change health in-game, re-scan
5. Valid chains remain → document offsets

### Automated Pattern Generation

```python
# re_scanner.py - Extract patterns from IDA database

import idc
import idautils

def generate_pattern(func_addr, length=32):
    """Extract first N bytes of function as pattern with auto-wildcarding."""
    pattern = []
    opcodes = set([0xE8, 0xE9, 0x0F84, 0x0F85])  # CALL, JMP, JCC opcodes
    
    for i in range(length):
        byte = idc.get_wide_byte(func_addr + i)
        
        # Check if this is part of a displacement operand (wildcard it)
        if is_displacement_byte(func_addr + i):
            pattern.append("??")
        else:
            pattern.append(f"{byte:02X}")
    
    return " ".join(pattern)

# Run on all named functions
for func_addr in idautils.Functions():
    name = idc.get_func_name(func_addr)
    if name.startswith("sub_"):
        continue  # Skip unnamed
    
    pattern = generate_pattern(func_addr)
    print(f'{{"{name}", "{pattern}"}},')
```

### Testing Matrix

Before merging new function discoveries:

| Test | Pass Criteria |
|------|---------------|
| **Pattern uniqueness** | Scanner finds exactly 1 match in .text |
| **.pdata validation** | Match is at exact function start (offset 0) |
| **Steam + GOG** | String anchor works on both (or two patterns provided) |
| **Hook stability** | 1000+ calls without crash |
| **Loading passthrough** | Hook disables during save load/save |
| **Reentrancy safety** | Calling original from hook doesn't corrupt MovRaxRsp slots |

---

## Appendix: Key Files

| File | Purpose |
|------|---------|
| `KenshiMP.Scanner/include/kmp/patterns.h` | Pattern definitions, string anchors, function table |
| `KenshiMP.Scanner/src/patterns.cpp` | RuntimeStringScanner, ResolveGameFunctions |
| `KenshiMP.Scanner/src/orchestrator.cpp` | Multi-strategy resolution pipeline |
| `KenshiMP.Scanner/src/pdata_enumerator.cpp` | .pdata validation, function boundary detection |
| `KenshiMP.Scanner/src/mov_rax_rsp_fix.cpp` | Naked detour + trampoline wrapper for MSVC functions |
| `KenshiMP.Core/hooks/entity_hooks.cpp` | CharacterSpawn/Destroy hook implementation |
| `docs/reminders/07-steam-re-findings.md` | Original Steam RE audit results |
| `docs/reminders/02-hook-status.md` | Hook installation status, validation results |

---

## Appendix: Quick Reference

### Common Prologue Patterns

```asm
# MovRaxRsp (64% of large functions)
48 8B C4                # mov rax, rsp
55                      # push rbp
48 8D A8 ?? ?? ?? ??    # lea rbp, [rax-0x???]

# Standard (36% of functions)
40 55                   # push rbp (REX prefix)
48 83 EC ??             # sub rsp, 0x??
48 8B EC                # mov rbp, rsp

# Leaf function (no frame)
48 83 EC 28             # sub rsp, 0x28 (shadow space only)
```

### x64 Calling Convention (Windows)

| Param # | Register | Stack |
|---------|----------|-------|
| 1 | RCX | [RSP+0x08] (shadowed) |
| 2 | RDX | [RSP+0x10] |
| 3 | R8 | [RSP+0x18] |
| 4 | R9 | [RSP+0x20] |
| 5+ | — | [RSP+0x28], [RSP+0x30]... |

**Return value:** RAX (integers/pointers), XMM0 (floats)

### RIP-Relative Addressing

```asm
lea rcx, [rip+0x12345]     # Load address relative to next instruction
# Target = (address of next instruction) + displacement
# Example: 0x140582324 (lea) + 0x7 (instruction size) + 0x12345 = 0x140594670
```

### Useful IDA Shortcuts

| Key | Action |
|-----|--------|
| `G` | Jump to address |
| `X` | Cross-references to |
| `Ctrl+X` | Cross-references from |
| `P` | Create function |
| `U` | Undefine |
| `Y` | Set function type |
| `N` | Rename |
| `;` | Add comment |
| `Shift+F12` | Strings window |

---

**Document Version:** 1.0  
**Last Updated:** 2026-06-04  
**Author:** KenshiMP Development Team  
**License:** MIT

For questions or contributions, see `README.md` or open an issue on GitHub.
