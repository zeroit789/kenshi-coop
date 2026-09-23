// ES: game_offset_prober.cpp - Implementación del sondeador de offsets: sondas individuales
//     (sceneNode, isPlayerControlled, aiPackage, moveSpeed, animState), lectura/escritura de la
//     caché JSON validada por la huella del ejecutable y la API pública. Se compila también en
//     KenshiMP.UnitTest. Todas las lecturas de memoria del juego van protegidas con SEH.
// EN: game_offset_prober.cpp - Offset prober implementation: individual probes (sceneNode,
//     isPlayerControlled, aiPackage, moveSpeed, animState), JSON cache read/write validated by
//     the executable fingerprint, and the public API. Also compiled into KenshiMP.UnitTest.
//     Every game memory read is SEH-protected.
#include "game_offset_prober.h"
#include "game_types.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <cmath>
#include <cstdio>

using json = nlohmann::json;

namespace kmp::game {

// ═══════════════════════════════════════════════════════════════════════════
//  INTERNAL STATE
// ═══════════════════════════════════════════════════════════════════════════

// ES: Estado interno: si el sondeo terminó y si ya se hizo una pasada completa.
// EN: Internal state: whether probing finished and whether a full pass already ran.
static bool s_proberComplete = false;
static bool s_proberRanOnce  = false;  // Prevent re-running after first full pass

// ES: Flags por offset: cada sonda se intenta como mucho una vez por sesión.
// EN:
// Per-offset probed flags — each is attempted at most once per session.
static bool s_probedSceneNode        = false;
static bool s_probedIsPlayerCtrl     = false;
static bool s_probedAnimState        = false;
static bool s_probedAIPackage        = false;
static bool s_probedMoveSpeed        = false;

// ES: Nombre del fichero de caché (se guarda junto al ejecutable del juego).
// EN:
// Cache file path (next to the DLL / game exe)
static const char* CACHE_FILE = "KenshiOnline_offset_cache.json";

// ═══════════════════════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════════════════════

// ES: Lectura de un valor protegida con SEH; false si hay violación de acceso.
// EN:
// SEH-protected single-value read. Returns false on access violation.
template<typename T>
static bool SafeRead(uintptr_t addr, T& out) {
    __try {
        out = *reinterpret_cast<const T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: ¿Parece un puntero de heap válido? (modo usuario, alineado a 8 y fuera de la imagen de
//     kenshi_x64.exe, que se supone de 64 MB fijos en vez de leer SizeOfImage).
// EN:
// Check if a pointer looks like a valid heap-allocated object (usermode, aligned, outside module).
static bool IsValidHeapPtr(uintptr_t ptr) {
    if (ptr < 0x10000 || ptr >= 0x00007FFFFFFFFFFF) return false;
    if ((ptr & 0x7) != 0) return false;
    // Reject if inside the game executable's image
    uintptr_t modBase = Memory::GetModuleBase();
    if (ptr >= modBase && ptr < modBase + 0x4000000) return false;
    return true;
}

// ES: ¿La vtable del objeto (primer qword) está dentro del rango de un módulo concreto?
//     Sirve para reconocer la clase del objeto (p.ej. Ogre::SceneNode si cae en OgreMain_x64.dll).
// EN:
// Check if a pointer's vtable lives inside a specific module's image range.
static bool HasVtableInModule(uintptr_t objPtr, uintptr_t moduleBase, uintptr_t moduleEnd) {
    if (moduleBase == 0 || moduleEnd == 0) return false;
    uintptr_t vtable = 0;
    if (!SafeRead(objPtr, vtable)) return false;
    return vtable >= moduleBase && vtable < moduleEnd;
}

// ES: Base y fin de un módulo cargado (leyendo SizeOfImage de la cabecera PE). La caché la hace
//     quien llama (ProbeSceneNode guarda el resultado en estáticos), no esta función.
// EN:
// Get module base and end for a named DLL (cached after first call).
static bool GetModuleRange(const char* dllName, uintptr_t& base, uintptr_t& end) {
    HMODULE hMod = GetModuleHandleA(dllName);
    if (!hMod) return false;
    base = reinterpret_cast<uintptr_t>(hMod);
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        end = base + nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ES: Huella del ejecutable para invalidar la caché: tamaño del fichero (32 bits bajos) +
//     TimeDateStamp de la cabecera PE en memoria (32 bits altos). Si cambia el exe, la caché no vale.
// EN:
// Compute a simple hash of the game executable for cache invalidation.
// Uses file size + PE timestamp as a fast fingerprint (no need for crypto hash).
static uint64_t ComputeExeFingerprint() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    HANDLE hFile = CreateFileA(exePath, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    DWORD fileSize = GetFileSize(hFile, nullptr);
    CloseHandle(hFile);

    // Also read PE timestamp from the executable in memory
    uintptr_t modBase = Memory::GetModuleBase();
    DWORD peTimestamp = 0;
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                peTimestamp = nt->FileHeader.TimeDateStamp;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Combine file size and PE timestamp into a single 64-bit fingerprint
    return (static_cast<uint64_t>(peTimestamp) << 32) | static_cast<uint64_t>(fileSize);
}

// ES: Ruta del fichero de caché: carpeta del ejecutable del juego + CACHE_FILE (el comentario
//     inglés habla del padre de la carpeta KenshiMP, pero el código usa la carpeta del exe).
// EN:
// Get the directory where the cache file should be written.
// Uses the Kenshi game directory (parent of the KenshiMP dir).
static std::string GetCachePath() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path(exePath);
    auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos) {
        path = path.substr(0, pos + 1);
    }
    path += CACHE_FILE;
    return path;
}

// ═══════════════════════════════════════════════════════════════════════════
//  INDIVIDUAL PROBES
// ═══════════════════════════════════════════════════════════════════════════

// ── sceneNode ──
// ES: Sonda de sceneNode: busca en el Character (+0x60..+0x300) un puntero cuya vtable esté en
//     OgreMain_x64.dll (un Ogre::SceneNode, el nodo de escena que lleva la transformación del
//     personaje) y cuya posición coincida (±5) con la posición en caché char+0x48.
// EN:
// Ogre::SceneNode* should be a pointer in the character struct whose vtable
// resides inside OgreMain_x64.dll. The sceneNode also contains the character's
// world transform, so we can cross-check the position.
// ES: True si descubrió el offset. Si la posición es (0,0,0) deja la sonda sin marcar para otro intento.
// EN: True if the offset was discovered. If the position is (0,0,0) the probe stays unmarked for a retry.
static bool ProbeSceneNode(uintptr_t charPtr) {
    if (s_probedSceneNode) return false;
    s_probedSceneNode = true;

    auto& offsets = GetOffsets().character;
    if (offsets.sceneNode >= 0) return false; // Already known

    // ES: Rango del módulo OgreMain (cacheado en estáticos).
    // EN:
    // Resolve OgreMain module range
    static uintptr_t s_ogreBase = 0, s_ogreEnd = 0;
    if (s_ogreBase == 0) {
        if (!GetModuleRange("OgreMain_x64.dll", s_ogreBase, s_ogreEnd)) {
            spdlog::debug("OffsetProber: OgreMain_x64.dll not loaded, skipping sceneNode probe");
            return false;
        }
    }

    // ES: Leer la posición en caché del personaje para validar después.
    // EN:
    // Read the character's cached position for cross-validation
    Vec3 cachedPos;
    if (offsets.position < 0) return false;
    Memory::ReadVec3(charPtr + offsets.position, cachedPos.x, cachedPos.y, cachedPos.z);
    if (cachedPos.x == 0.f && cachedPos.y == 0.f && cachedPos.z == 0.f) {
        s_probedSceneNode = false; // Let another character try later
        return false;
    }

    // ES: Recorrer campos alineados a 8 del Character entre +0x60 y +0x300.
    // EN:
    // Scan pointer-aligned fields in the character struct (0x60..0x300).
    // The sceneNode is typically in the first part of the struct, after
    // basic fields like vtable, squad, faction, name.
    for (int probe = 0x60; probe <= 0x300; probe += 8) {
        uintptr_t candidate = 0;
        if (!SafeRead(charPtr + probe, candidate)) continue;
        if (!IsValidHeapPtr(candidate)) continue;

        // ES: Comprobación 1: la vtable debe estar en OgreMain_x64.dll.
        // EN:
        // Check 1: vtable must point into OgreMain_x64.dll
        if (!HasVtableInModule(candidate, s_ogreBase, s_ogreEnd)) continue;

        // ES: Comprobación 2: probar varios offsets típicos de posición dentro de Ogre::Node
        //     (en tríos x,y,z) y comparar con la posición del personaje.
        // EN:
        // Check 2: Ogre::SceneNode::_getDerivedPosition() stores the world
        // position. In Ogre 1.x, the derived position is typically at
        // SceneNode+0x50..0x5C (Vec3) or at Node+0xD0..0xDC depending on build.
        // Try several common Ogre::Node position offsets.
        static const int ogre_pos_offsets[] = {
            0x4C, 0x50, 0x54,    // Ogre 1.7-1.9 SceneNode internal position
            0xD0, 0xD4, 0xD8,    // Ogre 1.x Node derived position (some builds)
            0xA0, 0xA4, 0xA8,    // Alternative layout
            0x10, 0x14, 0x18,    // Inline after vtable
        };

        for (int i = 0; i < sizeof(ogre_pos_offsets) / sizeof(int); i += 3) {
            float nx = 0.f, ny = 0.f, nz = 0.f;
            if (!SafeRead(candidate + ogre_pos_offsets[i],     nx)) continue;
            if (!SafeRead(candidate + ogre_pos_offsets[i + 1], ny)) continue;
            if (!SafeRead(candidate + ogre_pos_offsets[i + 2], nz)) continue;

            // ES: Coincidencia con tolerancia (la transformación de Ogre puede ir un frame por detrás).
            // EN:
            // Position match within tolerance (Ogre transform may lag by a frame)
            float dx = std::abs(nx - cachedPos.x);
            float dy = std::abs(ny - cachedPos.y);
            float dz = std::abs(nz - cachedPos.z);

            if (dx < 5.0f && dy < 5.0f && dz < 5.0f) {
                offsets.sceneNode = probe;
                spdlog::info("OffsetProber: Discovered sceneNode offset = 0x{:X} "
                             "(Ogre vtable confirmed, pos match at node+0x{:X})",
                             probe, ogre_pos_offsets[i]);
                return true;
            }
        }
    }

    spdlog::debug("OffsetProber: sceneNode probe failed — no Ogre::SceneNode pointer found");
    return false;
}

// ── isPlayerControlled ──
// HALLAZGO DE RE (KenshiLib Character.h + análisis del binario 1.0.68, 2026-06-17):
//   La clase Character NO tiene ningún campo isPlayerControlled/playerControlled/
//   isPlayer/controlledByPlayer. El motor distingue al personaje del jugador con un
//   MÉTODO (Character::isPlayerCharacter()), derivado de la pertenencia a la facción
//   del jugador: character.faction(+0x10) == gameWorld.player(+0x580).faction.
//
//   Por eso el antiguo probe diferencial (buscar un byte que valga 1 en el player y 0
//   en el NPC en el rango 0x80..0x500) NUNCA podía acertar: estaba buscando un campo
//   inexistente y siempre dejaba el offset en -1. Lo neutralizamos para no gastar ciclos
//   ni generar falsas expectativas. La distinción player/NPC se hace por facción en la
//   capa que lo necesita (ver derivación por facción en core.cpp / player_controller).
// EN: isPlayerControlled - RE FINDING (KenshiLib Character.h + 1.0.68 binary, 2026-06-17):
//     Character has NO isPlayerControlled-like field. The engine uses a METHOD
//     (Character::isPlayerCharacter()) derived from faction membership:
//     character.faction(+0x10) == gameWorld.player(+0x580).faction. The old differential probe
//     (a byte equal to 1 for the player and 0 for the NPC in 0x80..0x500) could never succeed,
//     so it is neutralized; player/NPC distinction is done by faction where needed.
static bool ProbeIsPlayerControlled(uintptr_t playerPtr, uintptr_t npcPtr) {
    (void)playerPtr;
    (void)npcPtr;
    if (s_probedIsPlayerCtrl) return false;
    s_probedIsPlayerCtrl = true;

    // EN: Mark the offset as "not applicable" (-2) to tell it apart from "pending" (-1), and log
    //     that this is not a probe failure but a non-existent field.
    // Marcamos el offset como "no aplica" (-2) para diferenciarlo de "pendiente" (-1)
    // y dejar constancia en el log de que NO es un fallo de probe, sino que el campo
    // no existe en la clase Character.
    auto& offsets = GetOffsets().character;
    if (offsets.isPlayerControlled == -1) offsets.isPlayerControlled = -2; // -2 = N/A (campo inexistente)
    spdlog::info("OffsetProber: isPlayerControlled — campo INEXISTENTE en Character "
                 "(KenshiLib + RE 1.0.68). Se deriva por facción: char.faction == player.faction. "
                 "Probe omitido (offset marcado N/A).");
    return false;
}

// ── aiPackage ──
// ES: Sonda de aiPackage: busca en +0x200..+0x400 un puntero de heap con vtable en el módulo del
//     juego cuyos 8 primeros qwords apunten de vuelta al personaje o a su facción.
//     OJO: aiPackage ya vale 0x20 por defecto (AITaskSytem* confirmado por RE), así que esta
//     sonda sale sin hacer nada; además 0x20 queda fuera del rango que escanea.
// EN: NOTE: aiPackage already defaults to 0x20 (RE-confirmed AITaskSytem*), so this probe
//     returns immediately; besides, 0x20 is outside the scanned range.
// The AI package pointer should be a heap-allocated object with a vtable in
// the game module (kenshi_x64.exe). It should be near the inventory/stats region.
// We look for a pointer in range 0x200..0x400 that has a game-module vtable
// and is NOT the inventory, stats, or other known pointers.
// ES: True si descubrió el offset.
// EN: True if the offset was discovered.
static bool ProbeAIPackage(uintptr_t charPtr) {
    if (s_probedAIPackage) return false;
    s_probedAIPackage = true;

    auto& offsets = GetOffsets().character;
    if (offsets.aiPackage >= 0) return false;

    uintptr_t modBase = Memory::GetModuleBase();
    uintptr_t modEnd  = modBase + 0x4000000; // Conservative 64MB estimate

    // ES: Afinar el fin del módulo leyendo SizeOfImage de la cabecera PE.
    // EN:
    // Refine module end from PE headers (safe: PE headers are in our address space)
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                modEnd = modBase + nt->OptionalHeader.SizeOfImage;
        }
    }

    // ES: Offsets conocidos que se saltan (facción, GameData, inventario).
    // EN:
    // Known offsets to skip (these point to known objects, not AI packages)
    const int knownPtrOffsets[] = {
        offsets.faction,     // 0x10
        offsets.gameDataPtr, // 0x40
        offsets.inventory,   // 0x2E8
    };

    for (int probe = 0x200; probe <= 0x400; probe += 8) {
        // Skip known pointer offsets
        bool skip = false;
        for (int known : knownPtrOffsets) {
            if (known >= 0 && probe == known) { skip = true; break; }
        }
        if (skip) continue;

        uintptr_t candidate = 0;
        if (!SafeRead(charPtr + probe, candidate)) continue;
        if (!IsValidHeapPtr(candidate)) continue;

        // ES: La vtable debe estar en kenshi_x64.exe.
        // EN:
        // Vtable must be in the game module
        uintptr_t vtable = 0;
        if (!SafeRead(candidate, vtable)) continue;
        if (vtable < modBase || vtable >= modEnd) continue;

        // ES: Buscar un puntero de vuelta al personaje o a su facción en los primeros qwords del objeto.
        // EN:
        // AI packages typically have a backpointer to the character or faction.
        // Check if any of the first 8 qwords in the AI object point back to
        // the character or its faction.
        uintptr_t factionPtr = 0;
        if (offsets.faction >= 0) SafeRead(charPtr + offsets.faction, factionPtr);

        bool hasBackRef = false;
        for (int i = 1; i <= 8; i++) {
            uintptr_t field = 0;
            if (!SafeRead(candidate + i * 8, field)) continue;
            if (field == charPtr || (factionPtr != 0 && field == factionPtr)) {
                hasBackRef = true;
                break;
            }
        }

        if (hasBackRef) {
            offsets.aiPackage = probe;
            spdlog::info("OffsetProber: Discovered aiPackage offset = 0x{:X} "
                         "(game vtable + backref confirmed)", probe);
            return true;
        }
    }

    spdlog::debug("OffsetProber: aiPackage probe failed");
    return false;
}

// ── moveSpeed ──
// ES: Sonda de moveSpeed: aplazada. Sin observar varios frames no se puede distinguir un float
//     de velocidad, así que solo marca la sonda como intentada y no descubre nada.
// EN:
// Movement speed is a float that should be > 0 when the character is moving
// and ~0 when stationary. We look for a float in range [0, 30] in the
// character struct between offsets 0x200 and 0x450 that is NOT part of a
// known field. Since we can't observe motion in a single probe, we accept
// any reasonable-looking speed float.
static bool ProbeMoveSpeed(uintptr_t charPtr) {
    if (s_probedMoveSpeed) return false;
    s_probedMoveSpeed = true;

    auto& offsets = GetOffsets().character;
    if (offsets.moveSpeed >= 0) return false;

    // This probe has low confidence without observing change over time.
    // We'll scan for a float field that looks like a speed value (0..30 range,
    // not integer-valued which would suggest a counter or flag).
    // For now, skip this probe — it needs multi-frame observation.
    // The moveSpeed offset can be discovered later via a dedicated motion probe.
    spdlog::debug("OffsetProber: moveSpeed probe deferred — needs multi-frame observation");
    return false;
}

// ── animState ──
// ES: Sonda de animState: también aplazada (necesita observar transiciones de animación).
// EN:
// Animation state is typically a small integer (0-255) or enum.
// Similar to moveSpeed, this needs observation over time to identify.
static bool ProbeAnimState(uintptr_t charPtr) {
    if (s_probedAnimState) return false;
    s_probedAnimState = true;

    auto& offsets = GetOffsets().character;
    if (offsets.animState >= 0) return false;

    // Deferred — needs multi-frame observation of animation transitions
    spdlog::debug("OffsetProber: animState probe deferred — needs multi-frame observation");
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  CACHE I/O
// ═══════════════════════════════════════════════════════════════════════════

// ES: Carga la caché JSON: comprueba que la huella del exe coincida, restaura los offsets >= 0 y
//     marca como sondeados los que se recuperaron. True si restauró al menos uno.
// EN: Loads the JSON cache: checks the exe fingerprint matches, restores offsets >= 0 and marks
//     the restored ones as probed. True if at least one was restored.
bool LoadOffsetCache() {
    // ES: Sin __try aquí: los objetos json tienen destructores y MSVC no permite SEH con unwinding C++;
    //     json::parse con allow_exceptions=false maneja los errores de parseo.
    // EN:
    // NOTE: No __try/__except in this function — json objects have destructors,
    // and MSVC forbids SEH in functions that require C++ unwinding.
    // json::parse with allow_exceptions=false handles parse errors safely.

    std::string path = GetCachePath();
    std::ifstream file(path);
    if (!file.is_open()) {
        spdlog::debug("OffsetProber: No cache file at {}", path);
        return false;
    }

    json j = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        spdlog::warn("OffsetProber: Cache file is invalid JSON");
        return false;
    }

    // ES: Validar la huella del ejecutable.
    // EN:
    // Validate fingerprint
    uint64_t cachedFingerprint = j.value("exe_fingerprint", uint64_t(0));
    uint64_t currentFingerprint = ComputeExeFingerprint();
    if (cachedFingerprint == 0 || cachedFingerprint != currentFingerprint) {
        spdlog::info("OffsetProber: Cache invalidated — exe fingerprint changed "
                     "(cached=0x{:X}, current=0x{:X})", cachedFingerprint, currentFingerprint);
        return false;
    }

    // ES: Restaurar los offsets de la caché (solo valores >= 0).
    // EN:
    // Restore offsets from cache
    auto& offsets = GetOffsets().character;
    int restored = 0;

    auto restore = [&](const char* key, int& field) {
        if (j.contains(key) && j[key].is_number_integer()) {
            int val = j[key].get<int>();
            if (val >= 0) {
                field = val;
                restored++;
            }
        }
    };

    restore("sceneNode",          offsets.sceneNode);
    restore("isPlayerControlled", offsets.isPlayerControlled);
    restore("aiPackage",          offsets.aiPackage);
    restore("equipment",          offsets.equipment);
    restore("animClassOffset",    offsets.animClassOffset);
    restore("squad",              offsets.squad);
    restore("moveSpeed",          offsets.moveSpeed);
    restore("animState",          offsets.animState);

    if (restored > 0) {
        spdlog::info("OffsetProber: Restored {} offsets from cache ({})", restored, path);

        // ES: Marcar como hechas las sondas cuyos offsets se restauraron.
        // EN:
        // Mark probes as complete for offsets that were restored
        if (offsets.sceneNode >= 0)          s_probedSceneNode = true;
        if (offsets.isPlayerControlled >= 0) s_probedIsPlayerCtrl = true;
        if (offsets.aiPackage >= 0)          s_probedAIPackage = true;
        if (offsets.moveSpeed >= 0)          s_probedMoveSpeed = true;
        if (offsets.animState >= 0)          s_probedAnimState = true;

        GetOffsets().discoveredByScanner = true;
        return true;
    }

    spdlog::debug("OffsetProber: Cache had no restorable offsets");
    return false;
}

// ES: Escribe la caché JSON (huella, versión y offsets descubiertos >= 0) con sangría de 2.
// EN: Writes the JSON cache (fingerprint, version and discovered offsets >= 0) with 2-space indent.
void SaveOffsetCache() {
    auto& offsets = GetOffsets().character;

    json j;
    j["exe_fingerprint"] = ComputeExeFingerprint();
    j["version"]         = 1;

    // ES: Guardar solo los offsets descubiertos (>= 0).
    // EN:
    // Save all runtime-discovered offsets (only save if >= 0, i.e., discovered)
    auto save = [&](const char* key, int val) {
        if (val >= 0) j[key] = val;
    };

    save("sceneNode",          offsets.sceneNode);
    save("isPlayerControlled", offsets.isPlayerControlled);
    save("aiPackage",          offsets.aiPackage);
    save("equipment",          offsets.equipment);
    save("animClassOffset",    offsets.animClassOffset);
    save("squad",              offsets.squad);
    save("moveSpeed",          offsets.moveSpeed);
    save("animState",          offsets.animState);

    std::string path = GetCachePath();
    std::ofstream file(path);
    if (!file.is_open()) {
        spdlog::warn("OffsetProber: Failed to write cache to {}", path);
        return;
    }

    file << j.dump(2);
    file.close();

    spdlog::info("OffsetProber: Saved offset cache to {} ({} entries)",
                 path, j.size() - 2); // -2 for fingerprint and version
}

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

// ES: Resultado de una pasada de sondas: nº descubiertos, si hubo alguno nuevo y qué sondas
//     crashearon (SEH). La función que las ejecuta no puede tener objetos C++ con destructor.
// EN:
// SEH-safe probe runner — NO C++ objects with destructors allowed in this function.
// MSVC forbids __try in functions that need C++ unwinding.
// Returns: number of newly discovered offsets. Sets flags in |results|.
struct ProbeResults {
    int  discovered;
    bool anyNew;
    bool sceneNodeCrashed;
    bool playerCtrlCrashed;
    bool aiPackageCrashed;
    bool moveSpeedCrashed;
    bool animStateCrashed;
};

// ES: Ejecuta cada sonda dentro de su propio __try; si una crashea, la marca como intentada.
// EN: Runs each probe inside its own __try; if one crashes it is marked as attempted.
static ProbeResults RunProbesSEH(uintptr_t charPtr, uintptr_t npcCharPtr) {
    ProbeResults r = {};

    __try {
        if (ProbeSceneNode(charPtr)) { r.discovered++; r.anyNew = true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.sceneNodeCrashed = true;
        s_probedSceneNode = true;
    }

    __try {
        if (ProbeIsPlayerControlled(charPtr, npcCharPtr)) { r.discovered++; r.anyNew = true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.playerCtrlCrashed = true;
        s_probedIsPlayerCtrl = true;
    }

    __try {
        if (ProbeAIPackage(charPtr)) { r.discovered++; r.anyNew = true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.aiPackageCrashed = true;
        s_probedAIPackage = true;
    }

    __try {
        if (ProbeMoveSpeed(charPtr)) { r.discovered++; r.anyNew = true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.moveSpeedCrashed = true;
        s_probedMoveSpeed = true;
    }

    __try {
        if (ProbeAnimState(charPtr)) { r.discovered++; r.anyNew = true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.animStateCrashed = true;
        s_probedAnimState = true;
    }

    return r;
}

// ES: Punto de entrada: valida el puntero y que el personaje tenga posición, ejecuta las sondas,
//     loguea fallos y el estado de todos los offsets y, si todas las sondas se intentaron, marca
//     el sondeo como completo y guarda la caché. Devuelve si hubo offsets nuevos.
// EN: Entry point: validates the pointer and that the character has a position, runs the probes,
//     logs crashes and the status of every offset and, once all probes were attempted, marks
//     probing complete and saves the cache. Returns whether new offsets were found.
bool RunOffsetProber(uintptr_t charPtr, uintptr_t npcCharPtr) {
    if (s_proberComplete) return false;
    if (charPtr == 0) return false;

    // ES: Validar el puntero del personaje (modo usuario y alineado a 8).
    // EN:
    // Validate character pointer
    if (charPtr < 0x10000 || charPtr >= 0x00007FFFFFFFFFFF || (charPtr & 0x7) != 0)
        return false;

    // ES: Exigir posición no nula (personaje ya colocado en el mundo).
    // EN:
    // Verify we have a valid position (character must be fully initialized)
    auto& offsets = GetOffsets().character;
    Vec3 pos;
    if (offsets.position >= 0) {
        Memory::ReadVec3(charPtr + offsets.position, pos.x, pos.y, pos.z);
        if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) {
            return false; // Character not yet placed in world
        }
    }

    spdlog::info("OffsetProber: Running probe suite on char 0x{:X} (npc=0x{:X})",
                 charPtr, npcCharPtr);

    // ES: Ejecutar las sondas en la función SEH separada.
    // EN:
    // Run all probes in a SEH-safe context (separate function, no C++ destructors)
    ProbeResults results = RunProbesSEH(charPtr, npcCharPtr);

    // ES: Avisos de sondas que crashearon (aquí ya se puede usar spdlog, estamos fuera de __try).
    // EN:
    // Log crash warnings (safe to use spdlog here — we're outside __try)
    if (results.sceneNodeCrashed)
        spdlog::warn("OffsetProber: sceneNode probe crashed (SEH caught)");
    if (results.playerCtrlCrashed)
        spdlog::warn("OffsetProber: isPlayerControlled probe crashed (SEH caught)");
    if (results.aiPackageCrashed)
        spdlog::warn("OffsetProber: aiPackage probe crashed (SEH caught)");
    if (results.moveSpeedCrashed)
        spdlog::warn("OffsetProber: moveSpeed probe crashed (SEH caught)");
    if (results.animStateCrashed)
        spdlog::warn("OffsetProber: animState probe crashed (SEH caught)");

    // Log summary
    if (results.discovered > 0) {
        spdlog::info("OffsetProber: Discovered {} new offsets this run", results.discovered);
    }

    // ES: Volcar al log el estado final de cada offset.
    // EN:
    // Log final offset status — use spdlog's built-in hex formatting
    auto logOff = [](const char* name, int val) {
        if (val >= 0)
            spdlog::info("  {} = 0x{:X}", name, val);
        else
            spdlog::info("  {} = UNKNOWN", name);
    };
    spdlog::info("OffsetProber: Offset status after probe run:");
    logOff("sceneNode         ", offsets.sceneNode);
    logOff("isPlayerControlled", offsets.isPlayerControlled);
    logOff("aiPackage         ", offsets.aiPackage);
    logOff("equipment         ", offsets.equipment);
    logOff("animClassOffset   ", offsets.animClassOffset);
    logOff("squad             ", offsets.squad);
    logOff("moveSpeed         ", offsets.moveSpeed);
    logOff("animState         ", offsets.animState);

    // ES: ¿Se intentaron todas las sondas? Entonces se da por completo y se guarda la caché.
    // EN:
    // Check if all feasible probes have been attempted
    bool allProbed = s_probedSceneNode && s_probedIsPlayerCtrl &&
                     s_probedAIPackage && s_probedMoveSpeed && s_probedAnimState;

    if (allProbed) {
        s_proberComplete = true;
        spdlog::info("OffsetProber: All probes completed");

        // Save cache if anything was discovered
        if (results.anyNew || !s_proberRanOnce) {
            SaveOffsetCache();
        }
    }

    s_proberRanOnce = true;
    return results.anyNew;
}

// ES: Reinicia los flags de sondeo (no restaura los valores de los offsets ya descubiertos).
// EN: Resets the probing flags (does not restore the already discovered offset values).
void ResetOffsetProber() {
    s_proberComplete     = false;
    s_proberRanOnce      = false;
    s_probedSceneNode    = false;
    s_probedIsPlayerCtrl = false;
    s_probedAnimState    = false;
    s_probedAIPackage    = false;
    s_probedMoveSpeed    = false;
    spdlog::info("OffsetProber: Reset all probe state");
}

// ES: True si el sondeo terminó.
// EN: True if probing has finished.
bool IsProberComplete() {
    return s_proberComplete;
}

} // namespace kmp::game
