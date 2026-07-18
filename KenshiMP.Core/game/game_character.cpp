#include "game_types.h"
#include "game_offset_prober.h"
#include "spawn_manager.h"     // SpawnManager::ReadKenshiString (estática) para el volcado [DIAG]
#include "../hooks/ai_hooks.h" // ai_hooks::IsRemoteControlled — para el volcado [DIAG-COMBAT] del estado del char del jugador
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cmath>
#include <vector>
#include <mutex>
#include <atomic>

namespace kmp::game {

static GameOffsets s_offsets;
static bool s_offsetsInitialized = false;

GameOffsets& GetOffsets() {
    if (!s_offsetsInitialized) {
        // All verified offsets are now set as struct defaults in game_types.h.
        // Sources: KServerMod structs.h, KenshiLib GameWorld.h, CE pointer chains.
        // The scanner may override these with runtime-discovered values later.
        //
        // Offset status legend:
        //   Verified values: faction=0x10, name=0x18, position=0x48, rotation=0x58,
        //                    inventory=0x2E8, stats=0x450, gameSpeed=0x700,
        //                    characterList=0x0888, zoneManager=0x08B0
        //   Runtime probed:  equipment, squad, animClassOffset
        //   Chain-based:     health (2B8→5F8→40), money (298→78→88), position write
        //   Unknown (-1):    sceneNode, aiPackage, currentTask, isAlive,
        //                    isPlayerControlled, moveSpeed, animState,
        //                    timeOfDay (on TimeManager not GameWorld), weatherState
        s_offsetsInitialized = true;
        spdlog::info("GameOffsets: Initialized with KServerMod/KenshiLib verified values");
    }
    return s_offsets;
}

void InitOffsetsFromScanner() {
    // This would be called with values from the re_scanner.py output
    // or from the runtime string scanner's offset discovery.
    // For now, the CE fallbacks in GetOffsets() are used.
    // When the scanner provides JSON, we can parse it here.
    s_offsets.discoveredByScanner = false;
    spdlog::debug("InitOffsetsFromScanner: Using CE fallback offsets");
}

// ── Runtime Offset Discovery ──

// Probe the character struct to find animClassOffset by searching for
// a pointer chain that leads to a position matching the character's cached position.
// Chain: character+X → AnimClass → +charMovementOffset → +writablePosOffset+writablePosVecOffset → Vec3
// This is called lazily on the first WritePosition attempt for a character.
static bool s_animClassProbed = false;
static int  s_discoveredAnimClassOffset = -1;

static void ProbeAnimClassOffset(uintptr_t charPtr) {
    if (s_animClassProbed) return;

    auto& offsets = GetOffsets().character;

    // Read the character's known cached position for validation
    Vec3 cachedPos;
    if (offsets.position < 0) return;
    Memory::ReadVec3(charPtr + offsets.position, cachedPos.x, cachedPos.y, cachedPos.z);
    // Don't set s_animClassProbed if position is (0,0,0) — let another character try
    if (cachedPos.x == 0.f && cachedPos.y == 0.f && cachedPos.z == 0.f) return;

    // Only mark probed AFTER we have a valid position to test against
    s_animClassProbed = true;

    // Scan offsets 0x60 through 0x200 in 8-byte steps (pointer alignment)
    // looking for a pointer that leads through the known chain to a matching position.
    for (int probe = 0x60; probe <= 0x200; probe += 8) {
        uintptr_t candidate = 0;
        if (!Memory::Read(charPtr + probe, candidate) || candidate == 0) continue;

        // Validate: candidate should be a valid heap pointer (above 0x10000, below user limit)
        if (candidate < 0x10000 || candidate > 0x00007FFFFFFFFFFF) continue;

        // Follow the chain: candidate → +charMovementOffset → CharMovement
        uintptr_t charMovement = 0;
        if (!Memory::Read(candidate + offsets.charMovementOffset, charMovement) ||
            charMovement == 0) continue;
        if (charMovement < 0x10000 || charMovement > 0x00007FFFFFFFFFFF) continue;

        // Read position at the known writable offset
        uintptr_t posAddr = charMovement + offsets.writablePosOffset + offsets.writablePosVecOffset;
        float px = 0.f, py = 0.f, pz = 0.f;
        if (!Memory::Read(posAddr, px)) continue;
        if (!Memory::Read(posAddr + 4, py)) continue;
        if (!Memory::Read(posAddr + 8, pz)) continue;

        // Check if the position matches the cached position (within tolerance)
        float dx = std::abs(px - cachedPos.x);
        float dy = std::abs(py - cachedPos.y);
        float dz = std::abs(pz - cachedPos.z);

        if (dx < 1.0f && dy < 1.0f && dz < 1.0f) {
            s_discoveredAnimClassOffset = probe;
            offsets.animClassOffset = probe;
            spdlog::info("GameOffsets: Discovered animClassOffset = 0x{:X} via runtime probe", probe);
            return;
        }
    }

    spdlog::debug("GameOffsets: animClassOffset probe failed — Method 2 unavailable");
}

// SEH wrapper for ProbeAnimClassOffset — character pointers in the deferred queue
// may have been freed by the game engine during zone transitions.  Without SEH,
// an access violation here causes a silent crash.
static bool SEH_ProbeAnimClassOffset(uintptr_t charPtr) {
    __try {
        ProbeAnimClassOffset(charPtr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Probe for equipment array offset.
// Equipment is an array of 14 item pointers (EquipSlot::Count).
// We look for a region between inventory (0x2E8) and stats (0x450) containing
// an array where entries are either 0 (empty slot) or valid heap pointers.
static bool s_equipmentProbed = false;

static void ProbeEquipmentOffset(uintptr_t charPtr) {
    if (s_equipmentProbed) return;
    s_equipmentProbed = true;

    auto& offsets = GetOffsets().character;
    constexpr int SLOT_COUNT = 14;
    constexpr int ARRAY_SIZE = SLOT_COUNT * static_cast<int>(sizeof(uintptr_t)); // 14 * 8 = 112 bytes

    // Scan from after inventory to before stats, 8-byte aligned
    for (int probe = 0x2F0; probe <= 0x440 - ARRAY_SIZE; probe += 8) {
        int validPtrs = 0;
        int nullPtrs = 0;
        bool invalid = false;

        for (int slot = 0; slot < SLOT_COUNT; slot++) {
            uintptr_t itemPtr = 0;
            if (!Memory::Read(charPtr + probe + slot * sizeof(uintptr_t), itemPtr)) {
                invalid = true;
                break;
            }
            if (itemPtr == 0) {
                nullPtrs++;
            } else if (itemPtr > 0x10000 && itemPtr < 0x00007FFFFFFFFFFF) {
                validPtrs++;
            } else {
                invalid = true;
                break;
            }
        }

        if (invalid) continue;

        // A valid equipment array should have: some null + some valid ptrs,
        // and not ALL null (that could be any zeroed memory).
        // At minimum, a character should have at least 1 equipped item (usually body/legs).
        if (validPtrs >= 1 && (validPtrs + nullPtrs == SLOT_COUNT)) {
            offsets.equipment = probe;
            spdlog::info("GameOffsets: Discovered equipment offset = 0x{:X} ({} equipped, {} empty)",
                         probe, validPtrs, nullPtrs);
            return;
        }
    }

    spdlog::debug("GameOffsets: equipment offset probe failed");
}

// ── CharacterAccessor ──

Vec3 CharacterAccessor::GetPosition() const {
    Vec3 pos;
    int offset = GetOffsets().character.position;
    if (offset >= 0) {
        Memory::ReadVec3(m_ptr + offset, pos.x, pos.y, pos.z);
    }
    return pos;
}

Quat CharacterAccessor::GetRotation() const {
    Quat rot;
    int offset = GetOffsets().character.rotation;
    if (offset >= 0) {
        // Ogre quaternion layout: w, x, y, z (4 consecutive floats)
        Memory::Read(m_ptr + offset, rot);
    }
    return rot;
}

float CharacterAccessor::GetHealth(BodyPart part) const {
    auto& offsets = GetOffsets().character;

    // Method 1: Direct offset (if scanner found it).
    // NOTA: el modelo "array plano de floats" NO existe en 1.0.68 (la salud vive en
    // HealthPartStatus* individuales) — offsets.health es siempre -1, esta rama nunca
    // se activa. Se conserva solo como compatibilidad si un scanner futuro lo poblara.
    if (offsets.health >= 0) {
        float health = 0.f;
        Memory::Read(m_ptr + offsets.health + static_cast<int>(part) * offsets.healthStride, health);
        return health;
    }

    // Cadena canónica MedicalSystem (inline en char+0x458):
    // [char+0x5F8] = HealthPartStatus** → [array + part*8] → flesh @ +0x40
    if (offsets.healthPartArray >= 0 && offsets.healthBase >= 0) {
        uintptr_t partArray = 0;
        if (!Memory::Read(m_ptr + offsets.healthPartArray, partArray) || partArray == 0) return 0.f;
        uintptr_t partPtr = 0;
        if (!Memory::Read(partArray + static_cast<int>(part) * offsets.healthStride, partPtr) || partPtr == 0)
            return 0.f;
        float health = 0.f;
        Memory::Read(partPtr + offsets.healthBase, health);
        return health;
    }

    return 0.f;
}

bool CharacterAccessor::IsAlive() const {
    // First check the alive flag if available
    int offset = GetOffsets().character.isAlive;
    if (offset >= 0) {
        bool alive = false;
        Memory::Read(m_ptr + offset, alive);
        return alive;
    }

    // Fallback: check if chest health > -100 (Kenshi KO/death threshold)
    float chestHealth = GetHealth(BodyPart::Chest);
    float headHealth = GetHealth(BodyPart::Head);
    // In Kenshi, death occurs when chest or head health drops to approximately -100
    return chestHealth > -100.f && headHealth > -100.f;
}

bool CharacterAccessor::IsPlayerControlled() const {
    int offset = GetOffsets().character.isPlayerControlled;
    if (offset >= 0) {
        bool controlled = false;
        Memory::Read(m_ptr + offset, controlled);
        return controlled;
    }
    return false;
}

float CharacterAccessor::GetMoveSpeed() const {
    int offset = GetOffsets().character.moveSpeed;
    if (offset < 0) return 0.f;

    float speed = 0.f;
    Memory::Read(m_ptr + offset, speed);
    return speed;
}

uint8_t CharacterAccessor::GetAnimState() const {
    int offset = GetOffsets().character.animState;
    if (offset < 0) return 0;

    uint8_t state = 0;
    Memory::Read(m_ptr + offset, state);
    return state;
}

std::string CharacterAccessor::GetName() const {
    int offset = GetOffsets().character.name;
    if (offset < 0) return "Unknown";

    // MSVC x64 std::string layout:
    // +0x00: buf[16] (small string optimization buffer)
    // +0x10: size (uint64_t)
    // +0x18: capacity (uint64_t)
    // If capacity > 15, buf[0..7] is a pointer to heap-allocated data

    uintptr_t strAddr = m_ptr + offset;
    uint64_t size = 0, capacity = 0;
    Memory::Read(strAddr + 0x10, size);
    Memory::Read(strAddr + 0x18, capacity);

    if (size == 0 || size > 256) return "Unknown";

    char buffer[257] = {};
    if (capacity > 15) {
        // Heap-allocated: first 8 bytes are a pointer to the string data
        uintptr_t dataPtr = 0;
        Memory::Read(strAddr, dataPtr);
        if (dataPtr == 0) return "Unknown";
        for (size_t i = 0; i < size && i < 256; i++) {
            Memory::Read(dataPtr + i, buffer[i]);
        }
    } else {
        // SSO: data is inline in the buffer
        for (size_t i = 0; i < size && i < 256; i++) {
            Memory::Read(strAddr + i, buffer[i]);
        }
    }

    return std::string(buffer, size);
}

bool CharacterAccessor::WriteName(const std::string& name) {
    int offset = GetOffsets().character.name;
    if (offset < 0 || name.empty()) return false;

    uintptr_t strAddr = m_ptr + offset;

    // Read current capacity to determine SSO vs heap
    uint64_t currentCapacity = 0;
    Memory::Read(strAddr + 0x18, currentCapacity);

    uint64_t newSize = name.size();

    if (newSize <= 15) {
        // SSO: write directly into the inline buffer (first 16 bytes)
        // Zero the buffer first
        char zeroBuf[16] = {};
        for (int i = 0; i < 16; i++) {
            Memory::Write(strAddr + i, zeroBuf[i]);
        }
        // Write new name
        for (size_t i = 0; i < newSize; i++) {
            Memory::Write(strAddr + i, name[i]);
        }
        // Update size
        Memory::Write(strAddr + 0x10, newSize);
        // Set capacity to 15 (SSO mode)
        uint64_t ssoCapacity = 15;
        Memory::Write(strAddr + 0x18, ssoCapacity);
        return true;
    }

    // For names > 15 chars, we can only overwrite if there's already heap allocation
    // with enough capacity. We don't want to allocate game heap memory from our DLL.
    if (currentCapacity >= newSize) {
        uintptr_t dataPtr = 0;
        Memory::Read(strAddr, dataPtr);
        if (dataPtr == 0 || dataPtr < 0x10000) return false;

        for (size_t i = 0; i < newSize; i++) {
            Memory::Write(dataPtr + i, name[i]);
        }
        // Null-terminate
        char nul = 0;
        Memory::Write(dataPtr + newSize, nul);
        // Update size
        Memory::Write(strAddr + 0x10, newSize);
        return true;
    }

    // Can't fit the name — truncate to 15 chars (SSO)
    std::string truncated = name.substr(0, 15);
    return WriteName(truncated);
}

bool CharacterAccessor::WriteNameToGameData(const std::string& name) {
    // Write the name to the GameData template's name field as well.
    // Some UI elements (tooltips, nameplates) may read from the template
    // rather than the character's live name string. This ensures both are updated.
    int gdPtrOffset = GetOffsets().character.gameDataPtr;
    int gdNameOffset = GetOffsets().gameData.name;
    if (gdPtrOffset < 0 || gdNameOffset < 0 || name.empty()) return false;

    uintptr_t gameDataPtr = 0;
    Memory::Read(m_ptr + gdPtrOffset, gameDataPtr);
    if (gameDataPtr < 0x10000 || gameDataPtr >= 0x00007FFFFFFFFFFF) return false;

    // Write to GameData's name string using same SSO logic
    uintptr_t strAddr = gameDataPtr + gdNameOffset;
    uint64_t currentCapacity = 0;
    Memory::Read(strAddr + 0x18, currentCapacity);

    uint64_t newSize = std::min<uint64_t>(name.size(), 15);
    std::string safeName = name.substr(0, static_cast<size_t>(newSize));

    // Always use SSO (15 char max) to avoid allocating game heap
    char zeroBuf[16] = {};
    for (int i = 0; i < 16; i++)
        Memory::Write(strAddr + i, zeroBuf[i]);
    for (size_t i = 0; i < safeName.size(); i++)
        Memory::Write(strAddr + i, safeName[i]);
    Memory::Write(strAddr + 0x10, newSize);
    uint64_t ssoCapacity = 15;
    Memory::Write(strAddr + 0x18, ssoCapacity);
    return true;
}

bool CharacterAccessor::WriteFaction(uintptr_t factionPtr) {
    int offset = GetOffsets().character.faction;
    if (offset < 0) return false;
    // Validate faction pointer: must be heap-allocated, aligned, outside module
    if (factionPtr < 0x10000 || factionPtr >= 0x00007FFFFFFFFFFF || (factionPtr & 0x7) != 0) {
        spdlog::warn("WriteFaction: Rejected invalid faction ptr 0x{:X}", factionPtr);
        return false;
    }
    uintptr_t modBase = Memory::GetModuleBase();
    if (factionPtr >= modBase && factionPtr < modBase + 0x4000000) {
        spdlog::warn("WriteFaction: Rejected in-module faction ptr 0x{:X}", factionPtr);
        return false;
    }
    Memory::Write(m_ptr + offset, factionPtr);
    return true;
}

uintptr_t CharacterAccessor::GetGameDataPtr() const {
    int offset = GetOffsets().character.gameDataPtr;
    if (offset < 0) return 0;

    uintptr_t gdPtr = 0;
    Memory::Read(m_ptr + offset, gdPtr);
    if (gdPtr < 0x10000 || gdPtr >= 0x00007FFFFFFFFFFF || (gdPtr & 0x7) != 0) return 0;
    // GameData is heap-allocated — reject if inside module image
    uintptr_t modBase = Memory::GetModuleBase();
    if (gdPtr >= modBase && gdPtr < modBase + 0x4000000) return 0;
    return gdPtr;
}

uintptr_t CharacterAccessor::GetInventoryPtr() const {
    int offset = GetOffsets().character.inventory;
    if (offset < 0) return 0;

    uintptr_t ptr = 0;
    Memory::Read(m_ptr + offset, ptr);
    if (ptr < 0x10000 || ptr >= 0x00007FFFFFFFFFFF || (ptr & 0x7) != 0) return 0;
    return ptr;
}

// Function pointer for HavokCharacter::setPosition (resolved by patterns.cpp)
// Prologue analysis confirms: params~2 (RCX=this, RDX=Vec3*), stack=288
using SetPositionFn = void(__fastcall*)(void* character, const Vec3* pos);
static SetPositionFn s_setPositionFn = nullptr;

void SetGameSetPositionFn(void* fn) {
    s_setPositionFn = reinterpret_cast<SetPositionFn>(fn);
}

// ── Bridge del setter OFICIAL de pausa GameWorld::setPaused (RVA 0x787D40, 1.0.68) ──
// Firma: void __fastcall(void* gameWorld /*rcx*/, bool paused /*dl*/).
// Lo resuelve patterns.cpp por AOB y Core lo enchufa aquí vía SetGameSetPausedFn().
// game_world.cpp (GameWorldAccessor::SetPaused) lo consume si está disponible.
using SetPausedFn = void(__fastcall*)(void* gameWorld, bool paused);
static SetPausedFn s_setPausedFn = nullptr;

void SetGameSetPausedFn(void* fn) {
    s_setPausedFn = reinterpret_cast<SetPausedFn>(fn);
    spdlog::info("game_character: SetPaused (setter oficial) bridge set to 0x{:X}",
                 reinterpret_cast<uintptr_t>(fn));
}

bool HasGameSetPausedFn() {
    return s_setPausedFn != nullptr;
}

// Accessor interno usado por game_world.cpp para invocar el setter oficial bajo SEH.
// Devuelve true si el puntero existe y la llamada no lanzó excepción.
SetPausedFn GetGameSetPausedFn_Internal() {
    return s_setPausedFn;
}

// Track WritePosition method transitions: log when method changes, not just first call.
// This ensures we see if characters silently fall back to worse methods.
static int s_lastWritePosMethod = 0;  // 0=none, 1=setPositionFn, 2=physicsChain, 3=cached
static int s_writePosMethodCount = 0; // Total calls since last method change

bool CharacterAccessor::WritePosition(const Vec3& pos) {
    auto& offsets = GetOffsets().character;

    // Method 1 (best): Call the game's own HavokCharacter::setPosition function.
    // This properly moves the character through the physics engine.
    // Signature: void __fastcall setPosition(this, const Vec3* pos)
    if (s_setPositionFn) {
        if (s_lastWritePosMethod != 1) {
            spdlog::info("WritePosition: Using Method 1 (setPosition fn) at 0x{:X} for char 0x{:X} (prev method={})",
                         reinterpret_cast<uintptr_t>(s_setPositionFn), m_ptr, s_lastWritePosMethod);
            s_lastWritePosMethod = 1;
            s_writePosMethodCount = 0;
        }
        s_writePosMethodCount++;
        s_setPositionFn(reinterpret_cast<void*>(m_ptr), &pos);
        return true;
    }

    // Method 2: Try the writable physics position chain.
    // Eagerly probe on first access for ANY character, not just this one.
    if (offsets.animClassOffset < 0 && !s_animClassProbed) {
        spdlog::info("WritePosition: Method 1 unavailable (no setPosition fn), probing physics chain...");
        ProbeAnimClassOffset(m_ptr);
    }
    if (offsets.animClassOffset >= 0) {
        uintptr_t animClass = 0;
        if (Memory::Read(m_ptr + offsets.animClassOffset, animClass) && animClass != 0 &&
            animClass > 0x10000 && animClass < 0x00007FFFFFFFFFFF && (animClass & 0x7) == 0) {
            uintptr_t charMovement = 0;
            if (Memory::Read(animClass + offsets.charMovementOffset, charMovement) && charMovement != 0 &&
                charMovement > 0x10000 && charMovement < 0x00007FFFFFFFFFFF && (charMovement & 0x7) == 0) {
                uintptr_t posAddr = charMovement + offsets.writablePosOffset + offsets.writablePosVecOffset;
                Memory::Write(posAddr, pos.x);
                Memory::Write(posAddr + 4, pos.y);
                Memory::Write(posAddr + 8, pos.z);
                if (s_lastWritePosMethod != 2) {
                    spdlog::info("WritePosition: Using Method 2 (physics chain) animClass=0x{:X} for char 0x{:X} (prev method={})",
                                 offsets.animClassOffset, m_ptr, s_lastWritePosMethod);
                    s_lastWritePosMethod = 2;
                    s_writePosMethodCount = 0;
                }
                s_writePosMethodCount++;
                return true;
            }
        }
    }

    // Method 3 (fallback): Write to the cached read-only position.
    // This may be overwritten by the physics engine next frame, but for remote
    // characters that are continuously updated it's acceptable.
    if (offsets.position >= 0) {
        Memory::Write(m_ptr + offsets.position, pos.x);
        Memory::Write(m_ptr + offsets.position + 4, pos.y);
        Memory::Write(m_ptr + offsets.position + 8, pos.z);
        if (s_lastWritePosMethod != 3) {
            spdlog::warn("WritePosition: Using Method 3 (cached position fallback) for char 0x{:X} "
                         "— position may drift due to physics engine overwrite (prev method={})",
                         m_ptr, s_lastWritePosMethod);
            s_lastWritePosMethod = 3;
            s_writePosMethodCount = 0;
        }
        s_writePosMethodCount++;
    } else if (s_lastWritePosMethod != -1) {
        spdlog::error("WritePosition: ALL methods failed for char 0x{:X} — "
                      "no setPosition fn, no physics chain, no cached position offset", m_ptr);
        s_lastWritePosMethod = -1;
    }

    return offsets.position >= 0;
}

uintptr_t CharacterAccessor::GetFactionPtr() const {
    int offset = GetOffsets().character.faction;
    if (offset < 0) return 0;

    uintptr_t ptr = 0;
    Memory::Read(m_ptr + offset, ptr);
    return ptr;
}

TaskType CharacterAccessor::GetCurrentTask() const {
    int offset = GetOffsets().character.currentTask;
    if (offset < 0) return TaskType::NULL_TASK;

    uint32_t task = 0;
    Memory::Read(m_ptr + offset, task);
    return static_cast<TaskType>(task);
}

uintptr_t CharacterAccessor::GetStatsPtr() const {
    int offset = GetOffsets().character.stats;
    if (offset < 0) return 0;

    // Stats are stored inline in the character (not a pointer),
    // so return character address + offset
    return m_ptr + offset;
}

uintptr_t CharacterAccessor::GetEquipmentSlot(EquipSlot slot) const {
    int offset = GetOffsets().character.equipment;
    if (offset < 0) {
        // Try runtime probe on first access
        ProbeEquipmentOffset(m_ptr);
        offset = GetOffsets().character.equipment;
        if (offset < 0) return 0;
    }

    int slotIndex = static_cast<int>(slot);
    if (slotIndex < 0 || slotIndex >= static_cast<int>(EquipSlot::Count)) return 0;

    uintptr_t itemPtr = 0;
    Memory::Read(m_ptr + offset + slotIndex * sizeof(uintptr_t), itemPtr);
    return itemPtr;
}

uintptr_t CharacterAccessor::GetSquadPtr() const {
    auto& offsets = GetOffsets().character;

    // If squad offset already discovered, use it directly
    if (offsets.squad >= 0) {
        uintptr_t ptr = 0;
        Memory::Read(m_ptr + offsets.squad, ptr);
        return (ptr > 0x10000 && ptr < 0x00007FFFFFFFFFFF) ? ptr : 0;
    }

    // Heuristic probe: scan pointers near faction offset (0x10) for a KSquad*.
    // In Kenshi, squad pointers are typically at +0x08 or within the first 0x40 bytes.
    // A valid KSquad* will have a name string at squad+0x10.
    uintptr_t factionPtr = GetFactionPtr();
    if (factionPtr == 0) return 0; // Can't validate without faction context

    // Check common squad offsets: 0x08, 0x20, 0x28, 0x30, 0x38
    static const int candidateOffsets[] = { 0x08, 0x20, 0x28, 0x30, 0x38 };
    for (int off : candidateOffsets) {
        uintptr_t candidate = 0;
        if (!Memory::Read(m_ptr + off, candidate)) continue;
        if (candidate < 0x10000 || candidate > 0x00007FFFFFFFFFFF || (candidate & 0x7) != 0) continue;
        if (candidate == factionPtr) continue; // Skip the faction pointer itself
        // Overflow guard: candidate + 0x30 must not wrap
        if (candidate > 0x00007FFFFFFFFFE0ULL) continue;

        // Validate: a KSquad should have a readable name at +0x10
        // MSVC std::string layout: if size <= 15 (SSO), chars at +0x00; else ptr at +0x00
        // Check if squad+0x10 looks like a valid std::string (capacity field at +0x18 should be small)
        uint64_t capacity = 0;
        if (!Memory::Read(candidate + 0x10 + 0x18, capacity)) continue;
        if (capacity < 1 || capacity > 256) continue; // Reasonable string capacity

        uint64_t size = 0;
        if (!Memory::Read(candidate + 0x10 + 0x10, size)) continue;
        if (size > capacity || size > 64) continue; // Name shouldn't be > 64 chars

        // Looks like a valid squad pointer — cache the offset
        GetOffsets().character.squad = off;
        spdlog::info("CharacterAccessor: Discovered squad offset at +0x{:X} (squad=0x{:X})",
                     off, candidate);
        return candidate;
    }

    return 0;
}

uintptr_t CharacterAccessor::GetAIPackagePtr() const {
    int offset = GetOffsets().character.aiPackage;
    if (offset < 0) return 0;

    uintptr_t ptr = 0;
    Memory::Read(m_ptr + offset, ptr);
    if (ptr < 0x10000 || ptr >= 0x00007FFFFFFFFFFF || (ptr & 0x7) != 0) return 0;
    return ptr;
}

int CharacterAccessor::GetMoney() const {
    auto& offsets = GetOffsets().character;

    // Money via pointer chain: char+0x298 -> +0x78 -> +0x88
    if (offsets.moneyChain1 >= 0 && offsets.moneyChain2 >= 0 && offsets.moneyBase >= 0) {
        uintptr_t ptr1 = 0;
        if (!Memory::Read(m_ptr + offsets.moneyChain1, ptr1) || ptr1 == 0) return 0;

        uintptr_t ptr2 = 0;
        if (!Memory::Read(ptr1 + offsets.moneyChain2, ptr2) || ptr2 == 0) return 0;

        int money = 0;
        Memory::Read(ptr2 + offsets.moneyBase, money);
        return money;
    }

    return 0;
}

// ── CharacterIterator ──

CharacterIterator::CharacterIterator() {
    Reset();
}

void CharacterIterator::Reset() {
    m_index = 0;
    m_count = 0;
    m_listBase = 0;

    // Safety guard: during loading, the game is actively resizing the lektor
    // (dynamic array). Reading count/pointer non-atomically while the game thread
    // modifies them corrupts the heap. Skip all game memory reads during loading.
    if (IsGameLoading()) {
        static bool s_loggedLoadingSkip = false;
        if (!s_loggedLoadingSkip) {
            spdlog::warn("CharacterIterator skipped — game is loading");
            s_loggedLoadingSkip = true;
        }
        return;
    }

    // Get module range for heap-vs-module discrimination
    uintptr_t modBase = Memory::GetModuleBase();
    size_t modSize = 0x4000000; // 64MB fallback
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                modSize = nt->OptionalHeader.SizeOfImage;
        }
    }

    auto isValidHeapPtr = [modBase, modSize](uintptr_t val) -> bool {
        if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) return false;
        // Must be 8-byte aligned (x64 heap allocations are at least 16-byte aligned)
        if ((val & 0x7) != 0) return false;
        // Must be outside module image — game objects are heap-allocated
        if (val >= modBase && val < modBase + modSize) return false;
        return true;
    };

    // ── Strategy 1: PlayerBase ──
    // Read PlayerBase from the runtime-resolved address (found by patterns.cpp).
    uintptr_t playerBaseAddr = GetResolvedPlayerBase();
    if (playerBaseAddr != 0) {
        uintptr_t playerBase = 0;
        if (Memory::Read(playerBaseAddr, playerBase) && isValidHeapPtr(playerBase)) {
            m_listBase = playerBase;
        }
    }

    // If PlayerBase gave a pointer, validate it by walking the array.
    // If the first entry is 0 or invalid, PlayerBase isn't a character array —
    // clear m_listBase so we fall through to the GameWorld path.
    if (m_listBase != 0) {
        uintptr_t firstEntry = 0;
        if (!Memory::Read(m_listBase, firstEntry) || !isValidHeapPtr(firstEntry)) {
            spdlog::debug("CharacterIterator: PlayerBase dereference 0x{:X} has no valid entries — trying GameWorld",
                         m_listBase);
            m_listBase = 0;
        }
    }

    // ── Strategy 2: GameWorld + characterList fallback ──
    // The container at GameWorld+0x0888 is a "lektor" — verified 24-byte layout:
    //   +0x00 qword : vtable/header    (lektor is polymorphic — do NOT read as count or ptr)
    //   +0x08 dword : size             (uint32_t)
    //   +0x0C dword : capacity         (uint32_t)
    //   +0x10 qword : T** data         (backing array of Character*)
    // Verified against kenshi_x64.exe v1.0.68 Steam by disassembling push_back at
    // RVA 0x00787512 and RVA 0x00799BFF (both grow+append the container at +0x0888).
    // Previous code speculated between {count,ptr} and {ptr,count} 16-byte layouts —
    // both misread the vtable as a count or the packed size|capacity as a pointer,
    // producing garbage like faction=0xFA3000007FF71F5A, char="race" on Steam.
    auto tryReadLektor = [&](uintptr_t lektorBase) -> bool {
        uint32_t size = 0, capacity = 0;
        uintptr_t dataPtr = 0;
        if (!Memory::Read(lektorBase + 0x08, size))     return false;
        if (!Memory::Read(lektorBase + 0x0C, capacity)) return false;
        if (!Memory::Read(lektorBase + 0x10, dataPtr))  return false;
        if (size == 0)                return false;
        if (size > capacity)          return false;
        if (capacity > 100000)        return false;
        if (!isValidHeapPtr(dataPtr)) return false;
        m_listBase = dataPtr;
        m_count    = static_cast<int>(size);
        return true;
    };

    // ── Strategy 2: GameWorld -> player(+0x580) -> PlayerInterface.playerCharacters(+0x2B0) ──
    // El +0x888 antiguo (removal queue) NO contiene la lista de personajes del jugador.
    // La lista REAL del jugador es un lektor<Character*> dentro de PlayerInterface, al que
    // se llega así: GameWorld -> +0x580 (player) -> +0x2B0 (playerCharacters).
    // Reutilizamos el mismo tryReadLektor (layout de 24 bytes: size@+0x08, cap@+0x0C, data@+0x10).
    if (m_listBase == 0) {
        uintptr_t gameWorldAddr = GetResolvedGameWorld();
        if (gameWorldAddr != 0) {
            const auto& offsets = GetOffsets();
            int playerOff   = offsets.world.player;                   // 0x0580
            int charsOff    = offsets.playerInterface.playerCharacters; // 0x02B0

            // GetResolvedGameWorld() puede devolver:
            //   (a) la DIRECCIÓN de un puntero A GameWorld — hay que dereferenciar una vez
            //   (b) la dirección de GameWorld directamente (objeto estático)
            // Tomamos el objeto GameWorld real en gameWorld.
            uintptr_t gameWorld = 0;
            if (!(Memory::Read(gameWorldAddr, gameWorld) && isValidHeapPtr(gameWorld))) {
                // Caso (b): la dirección resuelta ES el objeto GameWorld.
                gameWorld = gameWorldAddr;
            }

            // GameWorld -> player (PlayerInterface*)
            uintptr_t playerIface = 0;
            if (Memory::Read(gameWorld + playerOff, playerIface) && isValidHeapPtr(playerIface)) {
                // PlayerInterface -> playerCharacters (lektor<Character*>)
                if (tryReadLektor(playerIface + charsOff)) {
                    spdlog::info("CharacterIterator: PlayerInterface.playerCharacters — {} characters at 0x{:X}",
                                 m_count, m_listBase);
                    return;
                }
            }

            // ── Último fallback histórico: GameWorld+0x888 (removal queue, DEPRECADO) ──
            // Solo se intenta si la cadena del PlayerInterface falló. Mantenido por si
            // alguna versión/plataforma difiere; en 1.0.68 Steam normalmente da vacío.
            int charListOff = offsets.world.characterList; // 0x0888 [DEPRECADO]
            if (tryReadLektor(gameWorld + charListOff)) {
                spdlog::warn("CharacterIterator: usando fallback DEPRECADO GameWorld+0x{:X} — {} chars at 0x{:X}",
                             charListOff, m_count, m_listBase);
                return;
            }

            static bool s_loggedFail = false;
            if (!s_loggedFail) {
                spdlog::warn("CharacterIterator: GameWorld fallback failed — "
                             "addr=0x{:X}, playerOff=0x{:X}, charsOff=0x{:X}",
                             gameWorldAddr, playerOff, charsOff);
                s_loggedFail = true;
            }
        }
    }

    if (m_listBase == 0) {
        static bool s_loggedNone = false;
        if (!s_loggedNone) {
            spdlog::warn("CharacterIterator: No character list source available (PlayerBase and GameWorld both failed)");
            s_loggedNone = true;
        }
        // [DIAG] La sonda v2 YA NO se dispara aquí (timing equivocado). Ahora se bombea
        // desde DiagTickPump() en OnGameTick cuando el PlayerBase ya es heap válido.
        return;
    }

    // Walk the pointer array to count valid entries (only needed for PlayerBase path)
    if (m_count == 0) {
        int estimatedCount = 0;
        for (int j = 0; j < 10000; j++) {
            uintptr_t charPtr = 0;
            if (!Memory::Read(m_listBase + j * sizeof(uintptr_t), charPtr) || charPtr == 0) {
                break;
            }
            if (!isValidHeapPtr(charPtr)) {
                break;
            }
            estimatedCount++;
        }
        m_count = estimatedCount;
    }
}

bool CharacterIterator::HasNext() const {
    return m_index < m_count;
}

CharacterAccessor CharacterIterator::Next() {
    if (!HasNext()) return CharacterAccessor(nullptr);

    uintptr_t charPtr = 0;
    Memory::Read(m_listBase + m_index * sizeof(uintptr_t), charPtr);
    m_index++;

    // Validate: must be non-zero, 8-byte aligned, in user-space
    if (charPtr == 0 || (charPtr & 0x7) != 0 || charPtr >= 0x00007FFFFFFFFFFF)
        return CharacterAccessor(nullptr);

    // Vtable check: first 8 bytes must point into the module (game class vtable in .rdata)
    uintptr_t vtable = 0;
    if (!Memory::Read(charPtr, vtable))
        return CharacterAccessor(nullptr);
    uintptr_t modBase = Memory::GetModuleBase();
    // Accept vtable within module image (typically in .rdata section)
    if (vtable < modBase || vtable >= modBase + 0x4000000)
        return CharacterAccessor(nullptr);

    return CharacterAccessor(reinterpret_cast<void*>(charPtr));
}

// ── GetPlayerFactionDirect ──
// Resuelve la facción del jugador SIN iterar la lista de personajes:
//   GameWorld -> +0x580 (player/PlayerInterface*) -> +0x2A0 (participant) = Faction*
// Cada salto se valida como puntero de heap (mismas guardas que CharacterIterator::Reset).
// Devuelve 0 si la cadena no es válida. Esta es la fuente PRIMARIA de facción.
uintptr_t GetPlayerFactionDirect() {
    uintptr_t gameWorldAddr = GetResolvedGameWorld();
    if (gameWorldAddr == 0) return 0;

    // Rango de módulo para distinguir punteros de heap de direcciones dentro del binario.
    uintptr_t modBase = Memory::GetModuleBase();
    size_t modSize = 0x4000000; // 64MB fallback
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                modSize = nt->OptionalHeader.SizeOfImage;
        }
    }
    auto isValidHeapPtr = [modBase, modSize](uintptr_t val) -> bool {
        if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) return false;
        if ((val & 0x7) != 0) return false;                    // 8-byte aligned
        if (val >= modBase && val < modBase + modSize) return false; // fuera del módulo
        return true;
    };

    const auto& offsets = GetOffsets();

    // GetResolvedGameWorld() puede devolver la DIRECCIÓN de la variable global (caso a)
    // o el objeto GameWorld directamente (caso b). Dereferenciamos una vez y validamos.
    uintptr_t gameWorld = 0;
    if (!(Memory::Read(gameWorldAddr, gameWorld) && isValidHeapPtr(gameWorld))) {
        gameWorld = gameWorldAddr; // caso (b)
    }

    // GameWorld -> player (PlayerInterface*)
    uintptr_t playerIface = 0;
    if (!Memory::Read(gameWorld + offsets.world.player, playerIface) || !isValidHeapPtr(playerIface))
        return 0;

    // PlayerInterface -> participant (Faction*)
    uintptr_t faction = 0;
    if (!Memory::Read(playerIface + offsets.playerInterface.participant, faction) || !isValidHeapPtr(faction))
        return 0;

    return faction;
}

// ── GetPlayerPrimaryCharacterDirect ──
// Devuelve el personaje PRIMARIO del jugador (el que controla) SIN iterar por nombre.
// Cadena (misma que CharacterIterator Strategy 2, pero apuntando directo a data[0]):
//   GameWorld -> +0x580 (player/PlayerInterface*) -> +0x2B0 (playerCharacters, lektor<Character*>)
//     layout lektor: size@+0x08, capacity@+0x0C, data@+0x10  ->  data[0] = char primario.
// Devuelve 0 si la cadena no está poblada (p.ej. justo tras la carga) — el caller reintenta.
// Esta es la vía ROBUSTA para el flujo connected-then-load: no depende del nombre "Player N"
// ni de que el hook CharacterCreate capturase la creación durante la carga.
uintptr_t GetPlayerPrimaryCharacterDirect() {
    uintptr_t gameWorldAddr = GetResolvedGameWorld();
    if (gameWorldAddr == 0) return 0;

    uintptr_t modBase = Memory::GetModuleBase();
    size_t modSize = 0x4000000; // 64MB fallback
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                modSize = nt->OptionalHeader.SizeOfImage;
        }
    }
    auto isValidHeapPtr = [modBase, modSize](uintptr_t val) -> bool {
        if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) return false;
        if ((val & 0x7) != 0) return false;
        if (val >= modBase && val < modBase + modSize) return false;
        return true;
    };

    const auto& offsets = GetOffsets();

    // Caso (a) puntero a GameWorld vs (b) instancia embebida: deref una vez y valida.
    uintptr_t gameWorld = 0;
    if (!(Memory::Read(gameWorldAddr, gameWorld) && isValidHeapPtr(gameWorld))) {
        gameWorld = gameWorldAddr; // caso (b)
    }

    // GameWorld -> player (PlayerInterface*)
    uintptr_t playerIface = 0;
    if (!Memory::Read(gameWorld + offsets.world.player, playerIface) || !isValidHeapPtr(playerIface))
        return 0;

    // PlayerInterface -> playerCharacters (lektor<Character*>): size@+0x08, data@+0x10.
    uintptr_t lektorBase = playerIface + offsets.playerInterface.playerCharacters; // +0x2B0
    uint32_t size = 0; uintptr_t dataPtr = 0;
    if (!Memory::Read(lektorBase + 0x08, size))   return 0;
    if (!Memory::Read(lektorBase + 0x10, dataPtr)) return 0;
    if (size == 0 || size > 100000)               return 0;  // aún sin poblar / basura
    if (!isValidHeapPtr(dataPtr))                 return 0;

    // data[0] = primer Character*. Validamos que tenga vtable dentro del módulo.
    uintptr_t firstChar = 0;
    if (!Memory::Read(dataPtr, firstChar) || !isValidHeapPtr(firstChar)) return 0;
    uintptr_t vtable = 0;
    if (!Memory::Read(firstChar, vtable)) return 0;
    if (vtable < modBase || vtable >= modBase + modSize) return 0; // no es objeto del juego

    return firstChar;
}

// ── IsInPlayerCharactersList ──
// [FIX-GHOST 2026-07] Comprueba si charPtr está en la lista NATIVA de personajes
// del jugador (PlayerInterface+0x2B0, lektor<Character*>). Misma cadena y mismas
// guardas de puntero que GetPlayerPrimaryCharacterDirect, pero iterando TODAS las
// entradas del lektor en vez de solo data[0].
// Motivo: FindAndClaimModCharacters reclamaba como entidad LOCAL cualquier char
// cuyo nombre encajase con "Player N" — incluidos NPCs fantasma del mundo que solo
// COMPARTEN el patrón de nombre. Esta función da el criterio de verdad del motor.
// Devuelve:  1 = está;  0 = lista legible pero NO está;  -1 = lista no disponible.
int IsInPlayerCharactersList(uintptr_t charPtr) {
    if (charPtr == 0) return 0;

    uintptr_t gameWorldAddr = GetResolvedGameWorld();
    if (gameWorldAddr == 0) return -1;

    // Rango del módulo para distinguir punteros de heap (mismas guardas que arriba).
    uintptr_t modBase = Memory::GetModuleBase();
    size_t modSize = 0x4000000; // 64MB fallback
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                modSize = nt->OptionalHeader.SizeOfImage;
        }
    }
    auto isValidHeapPtr = [modBase, modSize](uintptr_t val) -> bool {
        if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) return false;
        if ((val & 0x7) != 0) return false;
        if (val >= modBase && val < modBase + modSize) return false;
        return true;
    };

    const auto& offsets = GetOffsets();

    // Caso (a) puntero a GameWorld vs (b) instancia embebida: deref una vez y valida.
    uintptr_t gameWorld = 0;
    if (!(Memory::Read(gameWorldAddr, gameWorld) && isValidHeapPtr(gameWorld))) {
        gameWorld = gameWorldAddr; // caso (b)
    }

    // GameWorld -> player (PlayerInterface*)
    uintptr_t playerIface = 0;
    if (!Memory::Read(gameWorld + offsets.world.player, playerIface) || !isValidHeapPtr(playerIface))
        return -1;

    // PlayerInterface -> playerCharacters (lektor<Character*>): size@+0x08, data@+0x10.
    uintptr_t lektorBase = playerIface + offsets.playerInterface.playerCharacters; // +0x2B0
    uint32_t size = 0; uintptr_t dataPtr = 0;
    if (!Memory::Read(lektorBase + 0x08, size))   return -1;
    if (!Memory::Read(lektorBase + 0x10, dataPtr)) return -1;
    if (size == 0 || size > 100000)               return -1; // aún sin poblar / basura
    if (!isValidHeapPtr(dataPtr))                 return -1;

    // Recorrer las entradas comparando punteros (cap de seguridad: 64 chars de jugador).
    uint32_t count = (size < 64) ? size : 64;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t entry = 0;
        if (!Memory::Read(dataPtr + i * sizeof(uintptr_t), entry)) continue;
        if (entry == charPtr) return 1; // confirmado: es un personaje REAL del jugador
    }
    return 0; // lista legible y poblada, pero este char NO es del jugador
}

// ── DiagDumpPlayerFaction — SONDA v2 ──
// VOLCADO DE DIAGNÓSTICO (solo log, NO cambia comportamiento, NO escribe memoria).
// Objetivo: localizar el offset REAL de la facción del jugador escaneando en RANGO,
// y filtrar candidatos con un test de string LEGIBLE (ASCII imprimible). Loguea con "[DIAG]".
//
// Resuelve dos objetos al inicio:
//   - gwObj: objeto GameWorld real (deref de GetResolvedGameWorld() si es heap, si no la addr).
//   - pbObj: objeto PlayerBase/PlayerInterface real (deref de GetResolvedPlayerBase() si es heap).
// Tres escaneos:
//   A) pbObj como PlayerInterface: offsets +0x00..+0x400 buscando Faction* legible.
//   B) gwObj como GameWorld: offsets +0x400..+0xA00; cada qword se prueba como PlayerInterface*
//      (sub-offsets +0x2A0/+0x320 -> Faction*) y como Faction* directo.
//   C) vía Character: char[0].faction (char+0x10, VERIFICADO) = facción del jugador con CERTEZA.
// Cada Faction* candidato se loguea SIEMPRE en hex para poder cruzarlos entre escaneos.
//
// Se limita a un máximo de DIAG_MAX_DUMPS volcados globales para no spamear el log.
// Se dispara desde DiagTickPump() (game tick), cuando el PlayerBase ya es heap válido.
//
// ⚠ DESACTIVADO (=0) 2026-06-18: los escaneos A/B de esta sonda recorren rangos de memoria
//   (+0x00..+0x400 de PlayerBase, +0x400..+0xA00 de GameWorld) interpretando CADA qword como
//   Faction* y leyendo +0x1A8 (faction.nameStr) vía ReadKenshiString. Aunque todo va bajo SEH
//   y NO es fatal, dispara cientos de AVs internos (KenshiOnline_CRASH.log: READ at this+0x1B8
//   con this basura tipo 0x14...01A8) que ensucian el crash log y el VEH. La facción del player
//   YA está resuelta de forma fiable por GetPlayerFactionDirect() (GameWorld+0x580 -> +0x2A0),
//   así que este escaneo bruto de RE ya cumplió su propósito. Para reactivarlo puntualmente en
//   una sesión de RE, subir DIAG_MAX_DUMPS a 6 de nuevo.
static std::atomic<int> s_diagDumpCount{0};
static constexpr int DIAG_MAX_DUMPS = 0;

// ── isHeap compartido a nivel de fichero ──
// Valida que un qword parece un puntero de heap del juego:
//   >= 0x10000, < 0x00007FFFFFFFFFFF, 8-byte alineado, y FUERA de la imagen del módulo.
// Se usa tanto en la sonda como en DiagTickPump. Calcula el rango del módulo una vez.
static bool DiagIsHeap(uintptr_t v) {
    static uintptr_t s_modBase = 0;
    static size_t    s_modSize = 0x4000000; // fallback 64MB
    if (s_modBase == 0) {
        s_modBase = Memory::GetModuleBase();
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(s_modBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(s_modBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                s_modSize = nt->OptionalHeader.SizeOfImage;
        }
    }
    if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
    if ((v & 0x7) != 0) return false;                          // 8-byte alineado
    if (v >= s_modBase && v < s_modBase + s_modSize) return false; // fuera del módulo
    return true;
}

// ── esLegible ──
// Un string solo cuenta como nombre de facción si NO está vacío, tiene >=3 chars y
// >=80% de caracteres ASCII imprimibles (letra/dígito/espacio/puntuación visible).
// ReadKenshiString ya tiene SEH y devuelve "" si falla, pero NO valida ASCII, así que
// puede colar basura binaria — la filtramos aquí.
static bool DiagEsLegible(const std::string& s) {
    if (s.size() < 3) return false;
    int printable = 0;
    for (unsigned char c : s) {
        // ASCII imprimible estándar: 0x20 (espacio) .. 0x7E (~).
        if (c >= 0x20 && c <= 0x7E) printable++;
    }
    return (printable * 100) >= static_cast<int>(s.size()) * 80;
}

// ── Cuerpo real de la sonda v2 ──
// Separado en función propia porque DiagDumpPlayerFaction se invoca dentro de un wrapper
// SEH (__try/__except), y MSVC no permite mezclar SEH con objetos C++ de destructor no
// trivial (std::string) en la MISMA función. Aquí sí podemos usar std::string libremente.
static void DiagDumpPlayerFactionBody(int dumpId) {
    const auto& offsets = GetOffsets();
    const int facNameOff = offsets.factionExtra.nameStr;            // 0x01A8
    const int facName2   = offsets.faction.name;                   // 0x01A8 (corregido; antes 0x0010 erroneo)
    const int charFacOff = offsets.character.faction;              // 0x0010 (VERIFICADO)
    // Sub-offsets de PlayerInterface a probar como Faction* en el escaneo B (b1).
    const int piPart1    = offsets.playerInterface.participant;     // 0x02A0
    const int piPart2    = 0x0320;                                  // candidato alternativo

    // Lee un Faction* candidato, valida heap, y devuelve su nombre legible (o "").
    // Prueba +0x1A8 (faction.nameStr) y, como respaldo, +0x10 (faction.name).
    // 'usadoOff' recibe el offset que dio el nombre (para loguearlo). Loguea NADA aquí:
    // el llamante decide el formato según el escaneo.
    auto leerNombreFaccion = [&](uintptr_t facPtr, int& usadoOff) -> std::string {
        usadoOff = -1;
        if (!DiagIsHeap(facPtr)) return std::string();
        std::string n1 = SpawnManager::ReadKenshiString(facPtr + facNameOff);
        if (DiagEsLegible(n1)) { usadoOff = facNameOff; return n1; }
        std::string n2 = SpawnManager::ReadKenshiString(facPtr + facName2);
        if (DiagEsLegible(n2)) { usadoOff = facName2; return n2; }
        return std::string();
    };

    // ── Resolución de objetos al inicio ──
    // gwObj: objeto GameWorld real. GetResolvedGameWorld() puede devolver la ADDR de la
    // global (dentro del módulo) o el objeto directo. Si el deref es heap, usamos el deref.
    uintptr_t gwAddr = GetResolvedGameWorld();
    uintptr_t gwDeref = 0;
    bool gwDerefOk = (gwAddr != 0) && Memory::Read(gwAddr, gwDeref) && DiagIsHeap(gwDeref);
    uintptr_t gwObj = gwDerefOk ? gwDeref : gwAddr;

    // pbObj: objeto PlayerBase/PlayerInterface real (misma lógica de deref).
    uintptr_t pbAddr = GetResolvedPlayerBase();
    uintptr_t pbDeref = 0;
    bool pbDerefOk = (pbAddr != 0) && Memory::Read(pbAddr, pbDeref) && DiagIsHeap(pbDeref);
    uintptr_t pbObj = pbDerefOk ? pbDeref : pbAddr;

    spdlog::info("[DIAG] ===== Volcado #{}/{} (sonda v2: escaneo en rango) =====", dumpId, DIAG_MAX_DUMPS);
    spdlog::info("[DIAG] gwObj=0x{:X} (esHeap={}, deref={})  pbObj=0x{:X} (esHeap={}, deref={})",
                 gwObj, DiagIsHeap(gwObj), gwDerefOk, pbObj, DiagIsHeap(pbObj), pbDerefOk);
    spdlog::info("[DIAG] offsets: facName=+0x{:X}/+0x{:X}  char.faction=+0x{:X}  piPart=+0x{:X}/+0x{:X}",
                 facNameOff, facName2, charFacOff, piPart1, piPart2);

    // ── ESCANEO A — PlayerBase como PlayerInterface ──
    // Recorre pbObj +0x00..+0x400 de 8 en 8. Cada qword isHeap() se interpreta como Faction*.
    if (DiagIsHeap(pbObj)) {
        spdlog::info("[DIAG] A: escaneo PlayerBase(0x{:X}) +0x000..+0x400 como Faction*:", pbObj);
        for (int off = 0x00; off <= 0x400; off += 8) {
            uintptr_t cand = 0;
            if (!Memory::Read(pbObj + off, cand)) continue;
            if (!DiagIsHeap(cand)) continue;
            int usadoOff = -1;
            std::string nm = leerNombreFaccion(cand, usadoOff);
            if (!nm.empty()) {
                spdlog::info("[DIAG] A: PlayerBase+0x{:X} -> Faction?=0x{:X} name@+0x{:X}='{}'  <-- CANDIDATO",
                             off, cand, usadoOff, nm);
            }
        }
    } else {
        spdlog::info("[DIAG] A: pbObj 0x{:X} no es heap — escaneo A omitido", pbObj);
    }

    // ── ESCANEO B — GameWorld objeto ──
    // gwObj +0x400..+0xA00 de 8 en 8. Cada qword isHeap() se prueba de dos formas.
    if (DiagIsHeap(gwObj)) {
        spdlog::info("[DIAG] B: escaneo GameWorld(0x{:X}) +0x400..+0xA00:", gwObj);
        for (int off = 0x400; off <= 0xA00; off += 8) {
            uintptr_t q = 0;
            if (!Memory::Read(gwObj + off, q)) continue;
            if (!DiagIsHeap(q)) continue;

            // (b1) q como PlayerInterface*: probar SU +0x2A0 y +0x320 como Faction*.
            int piOffs[2] = { piPart1, piPart2 };
            for (int piOff : piOffs) {
                uintptr_t facPtr = 0;
                if (!Memory::Read(q + piOff, facPtr)) continue;
                if (!DiagIsHeap(facPtr)) continue;
                int usadoOff = -1;
                std::string nm = leerNombreFaccion(facPtr, usadoOff);
                if (!nm.empty()) {
                    spdlog::info("[DIAG] B(b1): GameWorld+0x{:X} -> PlayerIface?=0x{:X} +0x{:X} "
                                 "-> Faction?=0x{:X} name@+0x{:X}='{}'  <-- CANDIDATO",
                                 off, q, piOff, facPtr, usadoOff, nm);
                }
            }

            // (b2) q directamente como Faction*: probar su +0x1A8/+0x10.
            int usadoOff2 = -1;
            std::string nm2 = leerNombreFaccion(q, usadoOff2);
            if (!nm2.empty()) {
                spdlog::info("[DIAG] B(b2): GameWorld+0x{:X} -> Faction?=0x{:X} name@+0x{:X}='{}'  <-- CANDIDATO",
                             off, q, usadoOff2, nm2);
            }
        }
    } else {
        spdlog::info("[DIAG] B: gwObj 0x{:X} no es heap — escaneo B omitido", gwObj);
    }

    // ── ESCANEO C — vía Character (la facción con CERTEZA) ──
    // Vía 1: pbObj interpretado como array de Character* (pbObj+0 = Character* con vtable módulo).
    // Vía 2: lektor playerCharacters @ pbObj+0x2B0 (size@+0x08, data@+0x10) -> data[0].
    uintptr_t modBase = Memory::GetModuleBase();
    auto esCharacter = [&](uintptr_t p) -> bool {
        if (!DiagIsHeap(p)) return false;
        uintptr_t vtable = 0;
        if (!Memory::Read(p, vtable)) return false;
        // La vtable de un Character apunta dentro de la imagen del módulo (.rdata).
        return (vtable >= modBase && vtable < modBase + 0x4000000);
    };

    uintptr_t firstChar = 0;

    // Vía 1: pbObj+0 como Character* directo.
    if (firstChar == 0 && DiagIsHeap(pbObj)) {
        uintptr_t c0 = 0;
        if (Memory::Read(pbObj, c0) && esCharacter(c0)) {
            firstChar = c0;
            spdlog::info("[DIAG] C(via1): pbObj+0 -> Character* 0x{:X} (vtable OK)", firstChar);
        }
    }

    // Vía 2: lektor playerCharacters en pbObj+0x2B0.
    if (firstChar == 0 && DiagIsHeap(pbObj)) {
        const int charsOff = offsets.playerInterface.playerCharacters; // 0x2B0
        uint32_t lsize = 0; uintptr_t ldata = 0;
        Memory::Read(pbObj + charsOff + 0x08, lsize);
        Memory::Read(pbObj + charsOff + 0x10, ldata);
        if (DiagIsHeap(ldata) && lsize > 0 && lsize <= 100000) {
            uintptr_t c0 = 0;
            if (Memory::Read(ldata, c0) && esCharacter(c0)) {
                firstChar = c0;
                spdlog::info("[DIAG] C(via2): pbObj+0x{:X} lektor data[0] -> Character* 0x{:X} (size={})",
                             charsOff, firstChar, lsize);
            }
        }
    }

    if (esCharacter(firstChar)) {
        uintptr_t charFac = 0;
        Memory::Read(firstChar + charFacOff, charFac); // char+0x10 (VERIFICADO)
        int usadoOff = -1;
        std::string nm = leerNombreFaccion(charFac, usadoOff);
        spdlog::info("[DIAG] C: char[0]=0x{:X} faction@+0x{:X}=0x{:X} name='{}'  <-- FACCION DEL JUGADOR (CERTEZA)",
                     firstChar, charFacOff, charFac, nm);
        spdlog::info("[DIAG] C: cruza este Faction*=0x{:X} con los CANDIDATOS de A y B para hallar el offset",
                     charFac);

        // ── DIAG-COMBAT: estado del personaje del jugador (host) ──
        // Objetivo: confirmar que el char del host está correctamente "controlado por
        // el jugador" y NO marcado como remote-controlled por el mod (lo que bloquearía
        // sus órdenes). Si el host apareciese como remote-controlled, ESA sería la causa
        // de que no pueda atacar (el motor trataría sus decisiones como de red).
        //
        // 1) Flag isPlayerControlled (si el offset ya fue descubierto por la sonda de diff).
        int pcOff = offsets.character.isPlayerControlled;
        if (pcOff >= 0) {
            uint8_t pcVal = 0xFF;
            Memory::Read(firstChar + pcOff, pcVal);
            spdlog::info("[DIAG-COMBAT] C: char[0]=0x{:X} isPlayerControlled@+0x{:X} = {} "
                         "(1=jugador controla, 0=IA controla)",
                         firstChar, pcOff, (int)pcVal);
        } else {
            spdlog::info("[DIAG-COMBAT] C: char[0]=0x{:X} isPlayerControlled offset AÚN no descubierto "
                         "(la sonda de diff necesita comparar player vs NPC primero)", firstChar);
        }

        // 2) ¿El mod marcó este char del host como remote-controlled? NO debería.
        //    Si devuelve true, el mod estaría suprimiendo las órdenes/IA del host por error.
        bool remoteCtl = kmp::ai_hooks::IsRemoteControlled(reinterpret_cast<void*>(firstChar));
        spdlog::info("[DIAG-COMBAT] C: char[0]=0x{:X} IsRemoteControlled(mod) = {} "
                     "{}", firstChar, remoteCtl,
                     remoteCtl ? "<-- ¡¡ANOMALÍA!! El host NO debería estar remote-controlled — "
                                 "esto BLOQUEARÍA MoveTo/órdenes si esos hooks estuvieran activos"
                               : "(correcto: el host controla su propio personaje)");

        // 3) Candidato a squad pointer cerca del inicio del Character (+0x08/+0x20/+0x28/+0x30/+0x38).
        //    Solo log: ayuda a confirmar que el host está en un squad válido (de su facción).
        const int squadCandOffs[] = {0x08, 0x20, 0x28, 0x30, 0x38};
        for (int so : squadCandOffs) {
            uintptr_t sq = 0;
            if (Memory::Read(firstChar + so, sq) && DiagIsHeap(sq)) {
                uintptr_t sqVt = 0;
                bool vtOk = Memory::Read(sq, sqVt) && sqVt >= modBase && sqVt < modBase + 0x4000000;
                spdlog::info("[DIAG-COMBAT] C: char[0]+0x{:X} -> 0x{:X} (heap, vtableEnModulo={}) "
                             "candidato squad/objeto", so, sq, vtOk);
            }
        }
    } else {
        spdlog::info("[DIAG] C: no se encontró Character* válido (via1/via2 fallaron) — sin certeza esta sesión");
    }

    spdlog::info("[DIAG] ===== Fin volcado #{}/{} =====", dumpId, DIAG_MAX_DUMPS);
}

// ── Wrapper SEH para el cuerpo de la sonda ──
// Los escaneos en rango derefencian punteros como Faction* y leen +0x1A8: aunque cada
// candidato pasa isHeap() y Memory::Read/ReadKenshiString toleran fallos, blindamos el
// volcado completo contra cualquier access violation residual. Sin objetos C++ aquí.
static void SEH_DiagDumpPlayerFactionBody(int dumpId) {
    __try {
        DiagDumpPlayerFactionBody(dumpId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::warn("[DIAG] Volcado #{} abortado por excepción (SEH) — memoria no mapeada", dumpId);
    }
}

void DiagDumpPlayerFaction() {
    // Límite de volcados (atómico, thread-safe entre hilo de red y de lógica).
    int n = s_diagDumpCount.fetch_add(1);
    if (n >= DIAG_MAX_DUMPS) return;
    int dumpId = n + 1;

    // No tocar memoria del juego mientras carga (el lektor se está redimensionando).
    if (IsGameLoading()) {
        spdlog::warn("[DIAG] Volcado #{} abortado — el juego está cargando", dumpId);
        return;
    }

    // Cuerpo real bajo SEH (separado por la restricción MSVC: SEH + std::string).
    SEH_DiagDumpPlayerFactionBody(dumpId);
}

// ── DiagTickPump ──
// Bombea la sonda desde el game tick (OnGameTick) con throttle de 2s REALES vía steady_clock
// (OnGameTick corre a ~framerate, NO asumimos 60fps). CONDICIÓN de disparo: el PlayerBase
// resuelto debe ser un puntero de HEAP válido y no nulo (así esperamos a que el player exista).
// Si no es heap válido, NO contamos el tiempo (no reseteamos el reloj, no disparamos).
// DiagDumpPlayerFaction ya respeta su límite global de 6 volcados.
void DiagTickPump() {
    using clock = std::chrono::steady_clock;
    static clock::time_point s_lastDiag{};   // último volcado (epoch por defecto = nunca)
    static bool s_armed = false;             // false hasta el primer tick con PlayerBase válido

    // Resolver PlayerBase real (deref si es heap, si no la addr directa).
    uintptr_t pbAddr = GetResolvedPlayerBase();
    if (pbAddr == 0) return;
    uintptr_t pbDeref = 0;
    uintptr_t pbObj = (Memory::Read(pbAddr, pbDeref) && DiagIsHeap(pbDeref)) ? pbDeref : pbAddr;

    // Condición: el player debe existir ya (PlayerBase resuelto es heap válido).
    if (!DiagIsHeap(pbObj)) return; // no es heap -> no contamos tiempo ni disparamos.

    auto now = clock::now();
    if (!s_armed) {
        // Primer tick con player válido: dispara ya y arma el reloj.
        s_armed = true;
        s_lastDiag = now;
        DiagDumpPlayerFaction();
        return;
    }
    // A partir de ahí, 1 volcado cada ~2 segundos reales.
    if (now - s_lastDiag >= std::chrono::seconds(2)) {
        s_lastDiag = now;
        DiagDumpPlayerFaction();
    }
}

// ── FixCharacterFactionTo ──
// Arregla la FACCIÓN del personaje del HOST: si char+0x10 (faction) no apunta a la
// player faction válida (la que resuelve GameWorld+0x580 -> +0x2A0 = 'Sinnombre'),
// la escribe. Esto es lo que el motor usa en Character::isPlayerCharacter()
//   char.faction(+0x10) == gameWorld.player(+0x580).faction
// para reconocerte como jugador y aceptar tus órdenes de combate.
//
// SEGURIDAD: a diferencia del caso REMOTO (que escribe facciones de NPC que se
// liberan al descargar su zona -> use-after-free), aquí escribimos la PLAYER
// faction, propiedad de PlayerInterface, que vive toda la partida y NO se descarga
// con zonas. Por tanto escribirla en el char del host es seguro (es restaurar el
// valor correcto), no introduce el UAF documentado en player_controller.cpp.
//
// Toda la lectura/escritura va dentro de __try/__except: el char puede haberse
// liberado entre que el registry lo guardó y este momento. Se valida que el char
// sea un puntero de heap con vtable dentro del módulo (mismo criterio que el
// resto del fichero) antes de tocar nada.
//
// Loguea SIEMPRE con prefijo [DIAG-FAC]: char, faction ANTES, player faction, y
// (si escribe) faction DESPUÉS + si coincide. Devuelve el resultado para que el
// orquestador (core.cpp) sepa cuándo dar por arreglado el host.
//
// NOTA: __try no puede coexistir con objetos C++ que requieran unwinding (destructores)
// en el mismo scope (MSVC C2712). Por eso el bloque __try hace SOLO lectura/escritura
// cruda en variables POD locales, y TODO el logging (spdlog crea temporales con
// destructor) se hace FUERA del __try. La parte cruda se aísla en SEH_RawFixFaction.

// Helper SEH puro (sin objetos C++): lee faction antes, escribe si difiere, releé después.
// Devuelve true si la memoria fue accesible; rellena outBefore/outAfter/outWrote/outVtableOk.
static bool SEH_RawFixFaction(uintptr_t cp, int factionOff, uintptr_t playerFaction,
                              uintptr_t modBase, uintptr_t modSize,
                              uintptr_t& outVtable, uintptr_t& outBefore,
                              uintptr_t& outAfter, bool& outWrote) {
    outVtable = 0; outBefore = 0; outAfter = 0; outWrote = false;
    __try {
        outVtable = *reinterpret_cast<uintptr_t*>(cp);
        // Si la vtable no apunta al módulo, no es un objeto de clase del juego: no tocar.
        if (outVtable < modBase || outVtable >= modBase + modSize)
            return true; // memoria accesible pero char inválido (lo decide el caller)

        outBefore = *reinterpret_cast<uintptr_t*>(cp + factionOff);
        if (outBefore != playerFaction) {
            *reinterpret_cast<uintptr_t*>(cp + factionOff) = playerFaction; // escribir +0x10
            outWrote = true;
            outAfter = *reinterpret_cast<uintptr_t*>(cp + factionOff);       // releer
        } else {
            outAfter = outBefore;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false; // char liberado / memoria no accesible
    }
}

FixFactionResult FixCharacterFactionTo(void* charPtr, uintptr_t playerFaction) {
    uintptr_t cp = reinterpret_cast<uintptr_t>(charPtr);

    // Validación previa del char (sin lecturas peligrosas todavía).
    if (cp < 0x10000 || cp >= 0x00007FFFFFFFFFFF || (cp & 0x7) != 0)
        return FixFactionResult::InvalidChar;

    // La player faction debe ser un puntero de heap válido (la fuente de verdad).
    if (playerFaction < 0x10000 || playerFaction >= 0x00007FFFFFFFFFFF || (playerFaction & 0x7) != 0)
        return FixFactionResult::NoPlayerFaction;

    const int factionOff = GetOffsets().character.faction; // +0x10
    if (factionOff < 0) return FixFactionResult::InvalidChar;

    uintptr_t modBase = Memory::GetModuleBase();
    const uintptr_t modSize = 0x4000000; // 64MB — rango del módulo (mismo criterio que el fichero)
    // La player faction NO debe estar dentro del módulo (debe ser objeto de heap del juego).
    if (playerFaction >= modBase && playerFaction < modBase + modSize)
        return FixFactionResult::NoPlayerFaction;

    // ── Parte cruda protegida por SEH (sin objetos C++) ──
    uintptr_t vtable = 0, before = 0, after = 0;
    bool wrote = false;
    bool accessible = SEH_RawFixFaction(cp, factionOff, playerFaction, modBase, modSize,
                                        vtable, before, after, wrote);

    // ── Logging y decisión FUERA del __try (spdlog crea temporales con destructor) ──
    if (!accessible) {
        spdlog::warn("[DIAG-FAC] char=0x{:X} EXCEPCION al leer/escribir faction — char liberado?", cp);
        return FixFactionResult::Exception;
    }
    if (vtable < modBase || vtable >= modBase + modSize) {
        spdlog::warn("[DIAG-FAC] char=0x{:X} vtable=0x{:X} fuera del modulo — no es Character valido",
                     cp, vtable);
        return FixFactionResult::InvalidChar;
    }
    if (!wrote) {
        spdlog::info("[DIAG-FAC] char=0x{:X} faction(+0x{:X}) ANTES=0x{:X} player=0x{:X} -> YA CORRECTA",
                     cp, factionOff, before, playerFaction);
        return FixFactionResult::AlreadyCorrect;
    }
    bool match = (after == playerFaction);
    spdlog::info("[DIAG-FAC] char=0x{:X} faction(+0x{:X}) ANTES=0x{:X} -> DESPUES=0x{:X} "
                 "player=0x{:X} match={} {}",
                 cp, factionOff, before, after, playerFaction,
                 match ? "SI" : "NO", match ? "[FIX OK]" : "[FIX FALLO]");
    return match ? FixFactionResult::Fixed : FixFactionResult::WriteFailed;
}

// ── SetPlayerControlled ──
bool CharacterAccessor::SetPlayerControlled(bool controlled) {
    int offset = GetOffsets().character.isPlayerControlled;
    if (offset < 0) return false;
    uint8_t val = controlled ? 1 : 0;
    Memory::Write(m_ptr + offset, val);
    return true;
}

} // namespace kmp::game

// ── Deferred AnimClass Probing ──
// Characters spawned via in-place replay may not have a settled position on the
// first frame. We queue them and re-probe each game tick until the animClassOffset
// is discovered or the queue is exhausted.
static std::vector<uintptr_t> s_deferredProbeChars;
static std::mutex s_deferredProbeMutex;
static int s_deferredTotalAttempts = 0;

namespace kmp::game {

void ScheduleDeferredAnimClassProbe(uintptr_t charPtr) {
    std::lock_guard lock(s_deferredProbeMutex);
    // Avoid duplicates
    for (auto c : s_deferredProbeChars) {
        if (c == charPtr) return;
    }
    s_deferredProbeChars.push_back(charPtr);
    spdlog::info("game_character: Scheduled deferred AnimClass probe for char 0x{:X} ({} queued)",
                 charPtr, s_deferredProbeChars.size());
}

bool ProcessDeferredAnimClassProbes() {
    // Already discovered — no need to probe
    if (GetOffsets().character.animClassOffset >= 0) {
        std::lock_guard lock(s_deferredProbeMutex);
        s_deferredProbeChars.clear();
        return true;
    }

    std::lock_guard lock(s_deferredProbeMutex);
    if (s_deferredProbeChars.empty()) return false;

    // Try each queued character — stop as soon as one succeeds
    for (auto it = s_deferredProbeChars.begin(); it != s_deferredProbeChars.end(); ) {
        uintptr_t charPtr = *it;

        // Validate pointer still looks sane
        if (charPtr < 0x10000 || charPtr > 0x00007FFFFFFFFFFF) {
            it = s_deferredProbeChars.erase(it);
            continue;
        }

        // Reset the probed flag so ProbeAnimClassOffset will actually try
        s_animClassProbed = false;
        if (!SEH_ProbeAnimClassOffset(charPtr)) {
            spdlog::warn("game_character: Deferred AnimClass probe CRASHED on char 0x{:X} "
                         "— pointer likely freed, removing from queue", charPtr);
            it = s_deferredProbeChars.erase(it);
            continue;
        }

        if (GetOffsets().character.animClassOffset >= 0) {
            spdlog::info("game_character: Deferred AnimClass probe SUCCEEDED on char 0x{:X} "
                         "— animClassOffset=0x{:X}",
                         charPtr, GetOffsets().character.animClassOffset);
            s_deferredProbeChars.clear();
            return true;
        }

        ++it;
    }

    // Prune queue after 50 failed attempts total
    s_deferredTotalAttempts++;
    if (s_deferredTotalAttempts > 50) {
        spdlog::warn("game_character: Deferred AnimClass probe exhausted (50 ticks, {} chars) — giving up",
                     s_deferredProbeChars.size());
        s_deferredProbeChars.clear();
    }

    return false;
}

// ── Reset all probe state for reconnect or second game load ──
// Without this, statics like s_animClassProbed, s_deferredTotalAttempts, s_writePosLogged
// persist across game loads, causing probes to never fire on second load.
void ResetProbeState() {
    s_animClassProbed = false;
    s_discoveredAnimClassOffset = -1;
    s_equipmentProbed = false;
    s_lastWritePosMethod = 0;
    s_writePosMethodCount = 0;
    s_deferredTotalAttempts = 0;
    {
        std::lock_guard lock(s_deferredProbeMutex);
        s_deferredProbeChars.clear();
    }

    // Reset the unified offset prober as well (handles sceneNode, isPlayerControlled, aiPackage, etc.)
    ResetOffsetProber();

    spdlog::info("game_character: ResetProbeState — all probe statics cleared (incl. offset prober)");
}

// ── Player Controlled Offset Discovery ──
// Exploits the fact that the local player's primary character has isPlayerControlled=true
// while NPCs have it as false. Scans a range of offsets looking for this distinguishing byte.
static int s_discoveredPlayerControlledOffset = -1;

void ProbePlayerControlledOffset(uintptr_t playerCharPtr, uintptr_t npcCharPtr) {
    if (s_discoveredPlayerControlledOffset >= 0) return; // Already found
    if (playerCharPtr == 0 || npcCharPtr == 0) return;

    // Scan offsets between 0x100 and 0x500 (well past known fields, before stats at 0x450)
    // Looking for a byte that is 1 on the player char and 0 on the NPC
    for (int off = 0x100; off <= 0x500; off += 1) {
        uint8_t playerVal = 0, npcVal = 0;
        if (!Memory::Read(playerCharPtr + off, playerVal)) continue;
        if (!Memory::Read(npcCharPtr + off, npcVal)) continue;

        if (playerVal == 1 && npcVal == 0) {
            // Cross-validate: check bytes around this offset aren't part of a larger value
            // A true bool flag should have neighboring bytes that are NOT consistently 0/1
            uint8_t playerPrev = 0, playerNext = 0;
            Memory::Read(playerCharPtr + off - 1, playerPrev);
            Memory::Read(playerCharPtr + off + 1, playerNext);

            // Skip if this looks like part of a multi-byte value (e.g., 0x00000001 as uint32)
            if (playerPrev == 0 && playerNext == 0) {
                // Could be a standalone bool — check that the NPC also has 0s around it
                uint8_t npcPrev = 0, npcNext = 0;
                Memory::Read(npcCharPtr + off - 1, npcPrev);
                Memory::Read(npcCharPtr + off + 1, npcNext);

                // Both have 0-padded neighbors — likely a bool field
                if (npcPrev == 0 && npcNext == 0) {
                    s_discoveredPlayerControlledOffset = off;
                    GetOffsets().character.isPlayerControlled = off;
                    spdlog::info("game_character: Discovered isPlayerControlled offset = 0x{:X} "
                                 "(player=1, npc=0)", off);
                    return;
                }
            }
        }
    }

    spdlog::debug("game_character: isPlayerControlled probe failed — "
                  "no distinguishing byte found between player 0x{:X} and npc 0x{:X}",
                  playerCharPtr, npcCharPtr);
}

bool WritePlayerControlled(uintptr_t charPtr, bool controlled) {
    int off = GetOffsets().character.isPlayerControlled;
    if (off < 0) return false;
    uint8_t val = controlled ? 1 : 0;
    Memory::Write(charPtr + off, val);
    spdlog::info("game_character: Set isPlayerControlled={} on char 0x{:X} (offset 0x{:X})",
                 controlled, charPtr, off);
    return true;
}

} // namespace kmp::game

// ── Bridge functions to avoid circular Core include ──
// Core sets these via the game functions resolver. game_character.cpp reads them.
static uintptr_t s_resolvedPlayerBase = 0;
static uintptr_t s_resolvedGameWorld = 0;
static bool s_gameIsLoading = false;

namespace kmp::game {

uintptr_t GetResolvedPlayerBase() {
    return s_resolvedPlayerBase;
}

void SetResolvedPlayerBase(uintptr_t addr) {
    s_resolvedPlayerBase = addr;
}

uintptr_t GetResolvedGameWorld() {
    return s_resolvedGameWorld;
}

void SetResolvedGameWorld(uintptr_t addr) {
    s_resolvedGameWorld = addr;
    spdlog::info("game_character: GameWorld bridge set to 0x{:X}", addr);
}

bool IsGameLoading() {
    return s_gameIsLoading;
}

void SetGameLoadingState(bool loading) {
    s_gameIsLoading = loading;
}

} // namespace kmp::game
