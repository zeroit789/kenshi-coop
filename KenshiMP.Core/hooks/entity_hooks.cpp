// ES: entity_hooks.cpp — hooks de creación y destrucción de personajes (Character).
//     - Hook_CharacterCreate intercepta RootObjectFactory::process (RVA 0x581770), la
//       función del juego que construye TODOS los personajes a partir de una
//       estructura de petición. Su prólogo empieza por `mov rax, rsp`, por lo que el
//       HookManager lo envuelve con el wrapper especial "MovRaxRsp" (cambio de pila).
//       Sirve para: capturar el puntero a la fábrica y una copia de la estructura de
//       petición (necesarios para spawnear jugadores remotos), elegir la facción del
//       jugador durante la carga, detectar el fin de la carga, "secuestrar" NPCs
//       recién creados para representar a jugadores remotos y registrar/enviar al
//       servidor los personajes propios.
//     - Hook_CharacterDestroy intercepta el destructor virtual de Character (vtable[0]),
//       instalado de forma diferida, para limpiar el registro de entidades y avisar al
//       servidor antes de que la memoria desaparezca.
//     - Utilidades para llamar a la fábrica directamente (CallFactory*).
//     Todo corre en el hilo de juego salvo ResumeForNetwork/SuspendForDisconnect, que
//     pueden llamarse desde el hilo de red.
// EN: entity_hooks.cpp — character (Character) create/destroy hooks.
//     - Hook_CharacterCreate intercepts RootObjectFactory::process (RVA 0x581770), the
//       game function that builds EVERY character from a request struct. Its prologue
//       starts with `mov rax, rsp`, so HookManager wraps it with the special
//       "MovRaxRsp" wrapper (stack swap). It is used to: capture the factory pointer
//       and a copy of the request struct (needed to spawn remote players), elect the
//       player's faction while loading, detect the end of loading, "hijack" freshly
//       created NPCs to stand in for remote players, and register/report our own
//       characters to the server.
//     - Hook_CharacterDestroy intercepts Character's virtual destructor (vtable[0]),
//       installed lazily, to clean up the entity registry and notify the server before
//       the memory goes away.
//     - Helpers to call the factory directly (CallFactory*).
//     Everything runs on the game thread except ResumeForNetwork/SuspendForDisconnect,
//     which may be called from the network thread.
#include "entity_hooks.h"
#include "save_hooks.h"
#include "ai_hooks.h"
#include "char_tracker_hooks.h"
#include "squad_hooks.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../game/spawn_manager.h"
#include "../game/player_controller.h"
#include "../game/asset_facilitator.h"
#include "../sync/pipeline_state.h"
#include "kmp/hook_manager.h"
#include "kmp/protocol.h"
#include "kmp/memory.h"
#include "kmp/string_convert.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <mutex>

// ES: Declarado en core.cpp; se actualiza aquí para que el manejador VEH de crashes
//     muestre en qué número de create se produjo el fallo.
// EN: Declared in core.cpp — updated here so VEH crash handler shows which create# crashed
namespace kmp { extern volatile int g_lastCharacterCreateNum; }

namespace kmp::entity_hooks {

// ── FIX-FACTORY-EARLY: definición del flag (declarado en entity_hooks.h) ──
// true = armar el hook desde Install() en modo loading-passthrough para capturar el
//        factory en el 1er create de la carga (Opción A). Ver header para el porqué.
// EN: Flag definition (declared in entity_hooks.h). true = arm the hook from Install()
//     in loading-passthrough mode so the factory is captured on the first create of
//     the load (Option A). See the header for the rationale.
bool kCaptureFactoryEarly = true;

// ── FIX-FACTORY-EARLY: estado de captura temprana ──
// s_earlyFactoryLogged evita spamear el log [FIX-FACTORY-EARLY] (1 línea por captura).
// EN: Early-capture state. s_earlyFactoryLogged avoids spamming the
//     [FIX-FACTORY-EARLY] log (one line per capture).
static bool s_earlyFactoryLogged = false;

// ── Function Types ──
// ES: Firma de RootObjectFactory::process (y de create/createRandomChar, que se
//     llaman con la misma forma): (fábrica, datos de petición) -> Character* o nullptr.
// EN: Signature of RootObjectFactory::process (and of create/createRandomChar, which
//     are called with the same shape): (factory, request data) -> Character* or nullptr.
using CharacterCreateFn = void*(__fastcall*)(void* factory, void* templateData);
// Destructor virtual MSVC (vtable slot 0): "scalar deleting destructor".
// Firma real: void*(this, flags). flags bit0 = liberar memoria tras destruir.
// OJO: NO confundir con funcs.CharacterDestroy — ese RVA resuelve a
// NodeList::destroyNodesByBuilding, función equivocada (confirmado en pattern-verification.md).
// EN: MSVC virtual destructor (vtable slot 0): "scalar deleting destructor".
//     Real signature: void*(this, flags); flags bit0 = free memory after destroying.
//     NOTE: do NOT confuse with funcs.CharacterDestroy — that RVA resolves to
//     NodeList::destroyNodesByBuilding, the wrong function (confirmed in
//     pattern-verification.md).
using CharacterDestroyFn = void*(__fastcall*)(void* character, unsigned int flags);

// ES: Direcciones ORIGINALES de las funciones del juego (no trampolines).
//     Nota: s_destroyTargetAddr no se usa en este fichero (probablemente restos).
// EN: Store the ORIGINAL function addresses (NOT trampolines)
//     Note: s_destroyTargetAddr is unused in this file (probably leftover).
static uintptr_t s_createTargetAddr = 0;
static uintptr_t s_destroyTargetAddr = 0;

// ES: s_origCreate apunta al WRAPPER de trampolín construido por el arreglo MovRaxRsp
//     (restaura la RSP del juego antes de entrar en la original). s_origDestroy es el
//     trampolín normal de MinHook hacia el destructor original.
// EN: s_origCreate: Points to the TRAMPOLINE WRAPPER built by MovRaxRsp fix.
//     s_origDestroy is the regular MinHook trampoline to the original destructor.
static CharacterCreateFn  s_origCreate  = nullptr;
static CharacterDestroyFn s_origDestroy = nullptr;

// ES: Trampolín crudo de MinHook (empieza con `mov rax, rsp`). Se usa en llamadas
//     REENTRANTES para no pisar las ranuras globales del wrapper MovRaxRsp
//     (captured_rsp, stub_rsp, saved_game_ret).
// EN: s_rawTrampoline: MinHook's raw trampoline (starts with `mov rax, rsp`).
// Used for REENTRANT calls to avoid corrupting the MovRaxRsp wrapper's global
// data slots (captured_rsp, stub_rsp, saved_game_ret).
static CharacterCreateFn  s_rawCreateTrampoline = nullptr;

// ES: Stub propio para CallFactoryDirect: hace `mov rax, rsp; jmp rawTrampoline+3`,
//     dando a la original el RAX correcto (RSP del llamador) y entrando después de su
//     propio `mov rax, rsp`. Sin esto, RAX/RBP serían incorrectos y todos los accesos
//     [rbp+XX] leerían/escribirían memoria equivocada -> crash.
// EN: s_directCallStub: Custom caller stub for CallFactoryDirect.
// Does `mov rax, rsp; jmp rawTrampoline+3` — gives the original function the
// correct RAX (caller's RSP after CALL pushed return address) then enters the
// original body past its own `mov rax, rsp`. Without this, calling the raw
// trampoline from C++ gives wrong RAX → wrong RBP → all [rbp+XX] accesses
// read/write wrong memory → sign-extended faction pointers → crash.
static CharacterCreateFn  s_directCallStub = nullptr;
static void*              s_directCallStubAlloc = nullptr;

// ES: Si el hook de CharacterDestroy está realmente instalado.
// EN: Whether CharacterDestroy hook is actually installed
static bool s_destroyHookInstalled = false;

// Instalación diferida del destroy hook (definida tras Hook_CharacterDestroy).
// Necesita un Character* VIVO para leer su vtable — por eso no puede ir en Install().
// EN: Lazy install of the destroy hook (defined after Hook_CharacterDestroy).
//     It needs a LIVE Character* to read its vtable — that is why it cannot run in Install().
static void TryInstallDestroyHookFromCharacter(void* character);

// ── Diagnostic Counters ──
// ES: Contadores de diagnóstico (creates y destroys totales vistos por los hooks).
// EN: Diagnostic counters (total creates and destroys seen by the hooks).
static std::atomic<int> s_totalCreates{0};
static std::atomic<int> s_totalDestroys{0};

// ── Loading detection via create events ──
// ES: Se actualizan en CADA llamada a Hook_CharacterCreate (incluso en passthrough).
//     PollForGameLoad los lee en vez de usar CharacterIterator durante la carga.
// EN: Updated on EVERY Hook_CharacterCreate call (even in loading passthrough mode).
// PollForGameLoad reads these instead of CharacterIterator during loading.
static std::atomic<int64_t> s_lastCreateTimeMs{0};      // steady_clock ms since epoch
static std::atomic<int>     s_loadingCreateCount{0};     // creates during current loading burst
static std::atomic<bool>    s_loadingPassthrough{false};  // ultra-lightweight mode for loading

// ── Mod template character capture during loading ──
// ES: Durante la carga en passthrough se lee (con SEH) el nombre de cada personaje
//     creado; si es "Player 1".."Player 16" (plantillas del mod) se guarda el puntero
//     para usarlo tras la carga sin escanear el heap.
// EN: When a character is created during loading passthrough, SEH-safe read its name.
// If it matches "Player 1" through "Player 16", store the pointer for post-load use.
static constexpr int MAX_MOD_TEMPLATE_CAPTURES = 16;
static void* s_capturedModTemplates[MAX_MOD_TEMPLATE_CAPTURES] = {};
static std::atomic<int> s_capturedModTemplateCount{0};

// ── Position offset in request struct ──
// ES: Offset (detectado en runtime) de la posición Vec3 dentro de la estructura de
//     petición de process(); -1 = no detectado. Máx. 10 intentos.
// EN: Runtime-detected offset of the Vec3 position inside process()'s request
//     struct; -1 = not detected. Up to 10 attempts.
static int s_positionOffsetInStruct = -1;
static int s_positionDetectAttempts = 0;

// ── GameData pointer offset in request struct ──
// ES: Offset (detectado en runtime) del puntero GameData* (plantilla del personaje)
//     dentro de la estructura de petición; SpawnManager lo usa para cambiar la plantilla.
// EN: Runtime-detected offset of the GameData* pointer (character template) inside
//     the request struct; SpawnManager uses it to swap the template.
static int s_gameDataOffsetInStruct = -1;
static int s_gameDataDetectAttempts = 0;

// ── Direct spawn bypass ──
// ES: Si es true, el hook solo reenvía a la original (lo activan las CallFactory*).
// EN: When true the hook just forwards to the original (set by the CallFactory* helpers).
static std::atomic<bool> s_directSpawnBypass{false};

// ── Higher-level factory functions (resolved from known RVAs) ──
// ES: RootObjectFactory::create (RVA 0x583400): despacha a process() pero construye la
//     estructura de petición internamente; recibe (fábrica, GameData*), lo que evita el
//     problema de punteros caducados. createRandomChar (RVA 0x5836E0): NPC aleatorio.
//     Ambas NO se enganchan; se llaman directamente por puntero.
// EN: RootObjectFactory::create (0x583400) — dispatches to process() but builds request struct internally.
// Takes (factory, GameData*), not a raw request struct. This bypasses the stale-pointer problem.
static CharacterCreateFn s_factoryCreate = nullptr;
// RootObjectFactory::createRandomChar (0x5836E0) — creates a random NPC character.
static CharacterCreateFn s_factoryCreateRandomChar = nullptr;

// ── In-place replay tracking ──
// ES: Cuántos spawns in-place (secuestro de NPC) han salido bien y cuándo fue el último.
// EN: How many in-place spawns (NPC hijacks) succeeded and when the last one happened.
static std::atomic<int> s_inPlaceSpawnCount{0};
static auto s_lastInPlaceSpawnTime = std::chrono::steady_clock::now();

// ── Connected-mode counters (file scope for reset from ResumeForNetwork) ──
// ES: Contadores del modo conectado (a nivel de fichero para poder resetearlos desde
//     ResumeForNetwork). Se tocan desde el hilo de juego y el de red, por eso atómicos.
//     Nota: s_connectedBurstStart no es atómico.
// EN: These are accessed from game thread (Hook_CharacterCreate) and network thread
// (ResumeForNetwork), so use atomics for the counters.
//     Note: s_connectedBurstStart is not atomic.
static std::atomic<int> s_connectedCreateNum{0};
static auto s_connectedBurstStart = std::chrono::steady_clock::now();
static std::atomic<int> s_connectedBurstCount{0};
static std::atomic<int> s_earlyProbeCount{0};

// ── Per-player spawn cap ──
// ES: Límite de entidades remotas secuestradas por jugador (máx. 4), protegido con
//     mutex porque se accede desde el hilo de juego y el de red.
// EN: Accessed from game thread (Hook_CharacterCreate) and network thread (ResumeForNetwork).
//     Caps hijacked remote entities per player (max 4), mutex-protected.
static constexpr int MAX_SPAWNS_PER_PLAYER = 4;
static std::mutex s_spawnsPerPlayerMutex;
static std::unordered_map<PlayerID, int> s_spawnsPerPlayer;

// ── Request struct handling ──
// ES: Copia de 1024 bytes de la estructura de petición de process() capturada antes de
//     llamar a la original ("pre-call"), más el puntero a la fábrica guardado.
// EN: 1024-byte copy of process()'s request struct captured before calling the
//     original ("pre-call"), plus the saved factory pointer.
static constexpr size_t REQUEST_STRUCT_SIZE = 1024;
static uint8_t s_preCallStruct[REQUEST_STRUCT_SIZE] = {};
static bool s_havePreCallData = false;
static void* s_savedFactory = nullptr;

// ── Loading capture gate ──
// ES: Tras capturar pre-call + fábrica + facción en los primeros creates, se pasa a
//     passthrough para que el resto de creates de la carga (100+) vayan casi directos
//     a la original. (El comentario inglés dice "se desactiva"; en realidad activa
//     s_loadingPassthrough, no desinstala el hook.)
// EN: After capturing pre-call data + factory + faction from the first CharacterCreate,
// the hook disables itself so all remaining loading creates (100+) go directly
// to the original function — zero MovRaxRsp overhead, zero corruption risk.
//     (In practice it switches to s_loadingPassthrough; the hook stays installed.)
static bool s_loadingCapturesDone = false;

// ── SEH-safe memcpy for request struct capture ──
// ES: memcpy protegido con SEH: devuelve false si la lectura provoca una violación de acceso.
// EN: SEH-protected memcpy: returns false if the read triggers an access violation.
static bool SEH_MemcpySafe(void* dst, const void* src, size_t size) {
    __try {
        memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── Multi-source faction discovery ──
// ES: En vez de fiarse del primer personaje (que puede ser un NPC contratado), se
//     acumulan observaciones de facción de los primeros N personajes de la carga. Gana
//     la más frecuente, con bonus si el nombre del personaje coincide con el de la config.
// EN: Instead of trusting the very first character (which may be a hired NPC),
// we accumulate faction observations from the first N characters during loading.
// The faction seen most frequently wins, with a name-match bonus for the
// character whose name matches the player's config name.

static constexpr int FACTION_SCAN_WINDOW = 8;    // Scan first N characters
static constexpr int FACTION_MAX_CANDIDATES = 4;  // Track up to 4 distinct factions
static constexpr int FACTION_NAME_MATCH_BONUS = 10; // Weight for name-match

// ES: Candidata a facción del jugador: puntero a Faction, puntuación acumulada y si
//     vino de un personaje cuyo nombre coincidía con playerName.
// EN: Player-faction candidate: Faction pointer, accumulated score, and whether it
//     came from a character whose name matched playerName.
struct FactionCandidate {
    uintptr_t ptr = 0;
    int       score = 0;       // Frequency count + bonuses
    bool      nameMatched = false; // True if this faction came from a name-matched character
};

static FactionCandidate s_factionCandidates[FACTION_MAX_CANDIDATES] = {};
static int  s_factionCandidateCount = 0;
static int  s_factionScanCount = 0;          // How many characters we've scanned so far
static bool s_factionVotingDone = false;      // True after we've elected a winner

// ES: Facción del jugador elegida (y si ya está fijada).
// EN: The elected player faction (replaces old s_earlyPlayerFaction)
static std::atomic<uintptr_t> s_earlyPlayerFaction{0};
static std::atomic<bool>      s_earlyFactionLocked{false};

// ES: Lee con SEH el nombre del personaje (std::string de MSVC en char+name offset:
//     puntero/buffer SSO en +0x00, tamaño en +0x10, capacidad en +0x18; si cap > 15 el
//     texto está en el heap) a un buffer fijo. Devuelve la longitud o 0 si falla.
// EN: SEH-safe character name read into a fixed buffer (no std::string in __try).
// Returns length of name read, or 0 on failure.
//     Reads the MSVC std::string at the character's name offset (SSO buffer/pointer at
//     +0x00, size at +0x10, capacity at +0x18; heap-allocated when cap > 15).
static int SEH_ReadCharName(void* character, char* outBuf, int bufSize) {
    __try {
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (charPtr < 0x10000 || charPtr > 0x00007FFFFFFFFFFF) return 0;

        int nameOff = game::GetOffsets().character.name;
        if (nameOff < 0) return 0;

        uintptr_t strAddr = charPtr + nameOff;
        uint64_t strSize = 0, strCap = 0;
        Memory::Read(strAddr + 0x10, strSize);
        Memory::Read(strAddr + 0x18, strCap);

        if (strSize == 0 || strSize >= (uint64_t)(bufSize - 1)) return 0;

        uintptr_t dataPtr = strAddr; // SSO: inline
        if (strCap > 15) Memory::Read(strAddr, dataPtr); // heap
        if (dataPtr < 0x10000) return 0;

        SEH_MemcpySafe(outBuf, reinterpret_cast<void*>(dataPtr), (size_t)strSize);
        outBuf[strSize] = '\0';
        return (int)strSize;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ES: Registra un voto de facción durante la carga (para cada uno de los primeros
//     FACTION_SCAN_WINDOW personajes). Si nameMatched, suma un bonus grande. Ignora
//     punteros fuera de rango o no alineados a 8.
// EN: Record a faction observation during loading. Called for each of the first
// FACTION_SCAN_WINDOW characters. If nameMatched is true, the faction gets
// a large bonus score (this character's name matched the config playerName).
static void RecordFactionVote(uintptr_t factionPtr, bool nameMatched) {
    if (factionPtr == 0 || factionPtr < 0x10000 || factionPtr > 0x00007FFFFFFFFFFF) return;
    if ((factionPtr & 0x7) != 0) return;

    // Check if this faction is already in the candidate list
    for (int i = 0; i < s_factionCandidateCount; i++) {
        if (s_factionCandidates[i].ptr == factionPtr) {
            s_factionCandidates[i].score++;
            if (nameMatched) {
                s_factionCandidates[i].score += FACTION_NAME_MATCH_BONUS;
                s_factionCandidates[i].nameMatched = true;
            }
            return;
        }
    }

    // New faction — add if we have room
    if (s_factionCandidateCount < FACTION_MAX_CANDIDATES) {
        auto& c = s_factionCandidates[s_factionCandidateCount++];
        c.ptr = factionPtr;
        c.score = 1 + (nameMatched ? FACTION_NAME_MATCH_BONUS : 0);
        c.nameMatched = nameMatched;
    }
}

// ES: Elige la facción ganadora (mayor puntuación). 0 si no hay candidatas.
// EN: Elect the winning faction from accumulated votes.
// Returns the faction pointer with the highest score, or 0 if no candidates.
static uintptr_t ElectBestFaction() {
    uintptr_t best = 0;
    int bestScore = 0;
    for (int i = 0; i < s_factionCandidateCount; i++) {
        if (s_factionCandidates[i].score > bestScore) {
            bestScore = s_factionCandidates[i].score;
            best = s_factionCandidates[i].ptr;
        }
    }
    return best;
}

// ES: Comprueba el campo isPlayerFaction de la facción como señal adicional
//     (true si la facción pertenece al jugador).
// EN: Also check the faction's isPlayerFaction flag as an additional signal.
// Returns true if the pointer passes the flag check.
static bool SEH_CheckIsPlayerFaction(uintptr_t factionPtr) {
    __try {
        const int flagOff = game::GetOffsets().faction.isPlayerFaction;
        if (flagOff < 0) return false;
        // audit-14: isPlayerFaction (0x250) = PlayerInterface* (8 bytes), != 0 ⇒ jugador.
        // Antes se leía como bool de 1 byte (offset 0x90 erróneo) → señal basura.
        // EN: isPlayerFaction (faction+0x250) is a PlayerInterface* (8 bytes); non-zero
        //     means player faction. It used to be read as a 1-byte bool at the wrong
        //     offset 0x90, giving a garbage signal.
        uintptr_t playerIface = 0;
        Memory::Read(factionPtr + flagOff, playerIface);
        return playerIface != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Lee con SEH el puntero a facción del personaje (campo char+0x10). 0 si falla o
//     si el valor no parece un puntero válido alineado a 8.
// EN: SEH-protected faction read from a character pointer.
// Returns the faction pointer at char+0x10, or 0 on failure.
static uintptr_t SEH_ReadFaction(void* character) {
    __try {
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (charPtr < 0x10000 || charPtr > 0x00007FFFFFFFFFFF) return 0;
        uintptr_t factionPtr = 0;
        Memory::Read(charPtr + 0x10, factionPtr);
        if (factionPtr > 0x10000 && factionPtr < 0x00007FFFFFFFFFFF && (factionPtr & 0x7) == 0) {
            return factionPtr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

// ── Fallback faction pointer ──
// ES: Se actualiza cada vez que vemos un personaje con facción válida en cualquier
//     create. Último recurso si aún no se conoce la facción local.
// EN: Updated whenever we see a character with a valid faction during ANY creation.
// Used as last-resort when local faction hasn't been discovered yet.
static std::atomic<uintptr_t> s_fallbackFaction{0};

// ES: Guarda factionPtr como facción de respaldo si parece un puntero válido.
// EN: Stores factionPtr as the fallback faction if it looks like a valid pointer.
static void UpdateFallbackFaction(uintptr_t factionPtr) {
    if (factionPtr > 0x10000 && factionPtr < 0x00007FFFFFFFFFFF && (factionPtr & 0x7) == 0) {
        s_fallbackFaction.store(factionPtr, std::memory_order_relaxed);
    }
}

// ── SEH-safe character memory reads ──
// ES: Datos básicos leídos de un Character: posición, rotación, facción e id de facción.
// EN: Basic data read from a Character: position, rotation, faction and faction id.
struct SEH_CharData {
    Vec3 position;
    Quat rotation;
    uintptr_t factionPtr;
    uint32_t factionId;
    bool valid;
};

// ES: Lee con SEH los datos del personaje: posición en char+0x48/0x4C/0x50 (x,y,z),
//     rotación (cuaternión w,x,y,z) en char+0x58..0x64 y facción en char+0x10 (más su
//     id en faction+offset id). De paso actualiza la facción de respaldo.
// EN: SEH-reads character data: position at char+0x48/0x4C/0x50 (x,y,z), rotation
//     (quaternion w,x,y,z) at char+0x58..0x64 and faction at char+0x10 (plus its id at
//     faction+id offset). Also refreshes the fallback faction.
static SEH_CharData SEH_ReadCharacterData(void* character) {
    SEH_CharData result{};
    result.valid = false;
    __try {
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (charPtr < 0x10000 || charPtr > 0x00007FFFFFFFFFFF) return result;

        Memory::Read(charPtr + 0x48, result.position.x);
        Memory::Read(charPtr + 0x4C, result.position.y);
        Memory::Read(charPtr + 0x50, result.position.z);

        Memory::Read(charPtr + 0x58, result.rotation.w);
        Memory::Read(charPtr + 0x5C, result.rotation.x);
        Memory::Read(charPtr + 0x60, result.rotation.y);
        Memory::Read(charPtr + 0x64, result.rotation.z);

        Memory::Read(charPtr + 0x10, result.factionPtr);
        if (result.factionPtr > 0x10000 && result.factionPtr < 0x00007FFFFFFFFFFF && (result.factionPtr & 0x7) == 0) {
            const int fIdOff = game::GetOffsets().faction.id;
            if (fIdOff >= 0) {
                Memory::Read(result.factionPtr + fIdOff, result.factionId);
            }
            UpdateFallbackFaction(result.factionPtr);
        }

        result.valid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_readCrash = 0;
        if (++s_readCrash <= 5) {
            OutputDebugStringA("KMP: SEH_ReadCharacterData CRASHED — character memory invalid\n");
        }
    }
    return result;
}

// ES: Pasa (con SEH) el personaje recién creado a SpawnManager::OnGameCharacterCreated
//     para que aprenda plantillas/fábrica. Devuelve false si hubo crash.
// EN: Feeds (SEH-protected) the new character to SpawnManager::OnGameCharacterCreated
//     so it can learn templates/factory. Returns false on crash.
static bool SEH_FeedSpawnManager(void* factory, void* templateData, void* character) {
    __try {
        Core::Get().GetSpawnManager().OnGameCharacterCreated(factory, templateData, character);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_feedCrash = 0;
        if (++s_feedCrash <= 5) {
            OutputDebugStringA("KMP: SEH_FeedSpawnManager CRASHED — template scan hit bad memory\n");
        }
        return false;
    }
}

// ── SEH-protected NPC hijack ──
// ES: En vez de crear un personaje nuevo, SE APROPIA de un NPC existente (el que el
//     juego acaba de crear), que ya tiene facción, modelo y animaciones válidas. Lo
//     registra con netId, lo teletransporta a spawnPos, lo renombra y le quita la IA.
//     Comprueba antes que su vtable apunta dentro del módulo. true si lo consiguió.
// EN: Instead of creating a new character, TAKE OVER an existing NPC.
// The NPC already has a valid faction, model, animations — zero crash risk.
// We just: register it, teleport it, rename it, disable AI.
static bool SEH_HijackNPC(void* npcChar, EntityID netId, PlayerID owner, Vec3 spawnPos) {
    uintptr_t charAddr = reinterpret_cast<uintptr_t>(npcChar);
    if (charAddr < 0x10000 || charAddr >= 0x00007FFFFFFFFFFF || (charAddr & 0x7) != 0) {
        spdlog::warn("SEH_HijackNPC: Invalid char 0x{:X} for entity {} — skipped", charAddr, netId);
        return false;
    }

    __try {
        // Vtable check: first qword must point into module (.rdata)
        uintptr_t vtable = 0;
        if (!Memory::Read(charAddr, vtable)) return false;
        uintptr_t modBase = Memory::GetModuleBase();
        if (vtable < modBase || vtable >= modBase + 0x4000000) {
            spdlog::warn("SEH_HijackNPC: Bad vtable 0x{:X} for entity {} — not a game object", vtable, netId);
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::warn("SEH_HijackNPC: Vtable read crashed for entity {}", netId);
        return false;
    }

    // Link to entity registry
    Core::Get().GetEntityRegistry().SetGameObject(netId, npcChar);
    Core::Get().GetEntityRegistry().UpdatePosition(netId, spawnPos);

    __try {
        game::CharacterAccessor accessor(npcChar);

        // Teleport to remote player's position
        if (spawnPos.x != 0.f || spawnPos.y != 0.f || spawnPos.z != 0.f) {
            accessor.WritePosition(spawnPos);
        }

        // Rename to remote player's name + disable AI
        Core::Get().GetPlayerController().OnRemoteCharacterSpawned(netId, npcChar, owner);
        ai_hooks::MarkRemoteControlled(npcChar);

        // DO NOT touch the faction — the NPC's original faction is already valid.
        // This is the whole point of hijacking: zero faction issues.

        game::ScheduleDeferredAnimClassProbe(charAddr);

        spdlog::info("SEH_HijackNPC: HIJACKED NPC 0x{:X} as entity {} for player {} at ({:.1f},{:.1f},{:.1f})",
                     charAddr, netId, owner, spawnPos.x, spawnPos.y, spawnPos.z);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_hijackCrash = 0;
        if (++s_hijackCrash <= 10) {
            char buf[256];
            sprintf_s(buf, "KMP: SEH_HijackNPC CRASHED for entity %u (AV caught)\n", netId);
            OutputDebugStringA(buf);
        }
        return false;
    }
}

// ES: Ver entity_hooks.h.
// EN: See entity_hooks.h.
void SetDirectSpawnBypass(bool bypass) {
    s_directSpawnBypass.store(bypass, std::memory_order_release);
}

// ES: Llamada directa a la CharacterCreate original. Sin __try: la original deriva su
//     marco de RAX y marcos extra desplazarían la RSP. Se desactiva la optimización
//     para que el compilador no altere la pila. Prefiere el wrapper MovRaxRsp.
// EN: Direct call to original CharacterCreate.
// No __try wrapper: the raw trampoline starts with mov rax,rsp and the original
// function derives its frame from RAX. Extra stack frames shift RSP and corrupt
// the frame. If it crashes, VEH logs it and the game handler deals with it.
#pragma optimize("", off)
static void* CallOriginalCreate(void* factory, void* templateData) {
    // CRITICAL: Prefer the MovRaxRsp wrapper (s_origCreate) over the raw trampoline.
    // When called from within Hook_CharacterCreate (which was invoked via the naked
    // detour), the wrapper correctly restores the captured game RSP before calling
    // the original function. The raw trampoline's `mov rax, rsp` would get the WRONG
    // RSP (our C++ stack instead of the game caller's stack), corrupting the frame.
    // This fixes CallFactoryCreate: create() → process() → hook → wrapper → correct RSP.
    CharacterCreateFn fn = s_origCreate ? s_origCreate : s_rawCreateTrampoline;
    return fn(factory, templateData);
}
#pragma optimize("", on)

// ── SEH wrapper for in-place spawn replay via trampoline ──
// ES: Reejecuta la fábrica sobre reqStruct usando una copia pre-call guardada y restaura
//     siempre la estructura original (también si hay crash) para no corromper la pila.
//     Nota: no se llama desde ningún sitio de este fichero (probable código muerto
//     tras pasar al secuestro de NPC).
// EN: Replays the factory on reqStruct using a saved pre-call copy and always restores
//     the original struct (even on crash) to avoid stack corruption.
//     Note: not called anywhere in this file (likely dead code since NPC hijack).
// CRITICAL: Must restore reqStruct even on crash to prevent stack corruption
static uint8_t s_replayBackup[REQUEST_STRUCT_SIZE];
static void* SEH_ReplayFactory(CharacterCreateFn trampoline, void* factory, void* reqStruct,
                                const uint8_t* preCallData, size_t structSize) {
    // Save original BEFORE __try (so we can always restore)
    memcpy(s_replayBackup, reqStruct, structSize);
    memcpy(reqStruct, preCallData, structSize);
    __try {
        void* result = trampoline(factory, reqStruct);
        // Restore original on success
        memcpy(reqStruct, s_replayBackup, structSize);
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // ALWAYS restore original on crash — prevents stack corruption
        memcpy(reqStruct, s_replayBackup, structSize);
        static int s_replayCrash = 0;
        if (++s_replayCrash <= 5) {
            OutputDebugStringA("KMP: SEH_ReplayFactory CRASHED — reqStruct restored\n");
        }
        return nullptr;
    }
}

// ── SEH-safe loading data capture (pure C, no C++ objects) ──
// ES: Copia (una sola vez) la estructura de petición y la pasa a SpawnManager.
//     Nota: no se llama desde ningún sitio (código muerto; la lógica está inline en
//     Hook_CharacterCreate y en SEH_PropagateToSpawnManager).
// EN: Copies the request struct (once) and hands it to SpawnManager.
//     Note: never called (dead code; the logic is inlined in Hook_CharacterCreate and
//     SEH_PropagateToSpawnManager).
static void SEH_CaptureTemplateData(void* templateData) {
    __try {
        if (templateData && !s_havePreCallData) {
            SEH_MemcpySafe(s_preCallStruct, templateData, REQUEST_STRUCT_SIZE);
            s_havePreCallData = true;
            Core::Get().GetSpawnManager().SetPreCallData(
                s_preCallStruct, REQUEST_STRUCT_SIZE,
                reinterpret_cast<uintptr_t>(templateData));
            Core::Get().GetSpawnManager().SetSavedRequestStruct(
                s_preCallStruct, REQUEST_STRUCT_SIZE);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Silent failure — factory capture is best-effort
    }
}

// ── FIX-FACTORY-EARLY: validación "puntero de heap x64 real" ──
// El 1er arg de RootObjectFactory::process() (RVA 0x581770) es el RootObjectFactory*
// (this), confirmado por RE de bytes (kenshi-re-memory.md:54-67): create() guarda el
// factory en rcx/r14 y se lo pasa a process() intacto. Aquí filtramos basura: el
// slot global theFactory@0x21345B0 contiene en runtime un valor de 32 bits con la
// mitad alta NULA (p.ej. 0x590301A0) que NO es un puntero de heap x64 -> ese es el
// motivo por el que la captura vía global falla. El 1er arg del hook, en cambio, es
// el puntero de heap real. Aceptamos solo: no-null, alineado a 8, mitad alta NO nula
// (>= 0x100000000), dentro del rango user-space x64 y fuera del módulo.
// EN: "Real x64 heap pointer" check. The first arg of RootObjectFactory::process()
//     (RVA 0x581770) is the RootObjectFactory* (this). The global slot
//     theFactory@0x21345B0 holds at runtime a 32-bit value with a NULL upper half
//     (e.g. 0x590301A0), which is not a heap pointer — that is why capturing via the
//     global fails. We only accept: non-null, 8-byte aligned, non-zero upper half,
//     inside x64 user space and outside the game module.
static bool IsRealHeapFactory(void* factory) {
    uintptr_t p = reinterpret_cast<uintptr_t>(factory);
    if (p <= 0x10000 || p >= 0x00007FFFFFFFFFFF) return false;  // rango user-space
    if (p & 0x7) return false;                                   // alineado a 8 (vtable)
    if ((p >> 32) == 0) return false;                            // mitad alta NULA = basura 32-bit
    uintptr_t modBase = Memory::GetModuleBase();
    if (modBase != 0 && p >= modBase && p < modBase + 0x4000000) return false; // no en módulo
    return true;
}

// ── SEH-safe SpawnManager propagation (for already-captured pre-call data) ──
// ES: Pasa a SpawnManager la copia pre-call ya capturada y, si aún no está listo, la
//     fábrica. Se llama desde el passthrough de carga. SEH: fallo no fatal.
// EN: Called from loading passthrough after s_preCallStruct is populated.
//     Hands SpawnManager the pre-call copy and, if not ready yet, the factory.
static void SEH_PropagateToSpawnManager(void* templateData, void* factory) {
    __try {
        auto& spawnMgr = Core::Get().GetSpawnManager();
        spawnMgr.SetPreCallData(s_preCallStruct, REQUEST_STRUCT_SIZE,
                                reinterpret_cast<uintptr_t>(templateData));
        spawnMgr.SetSavedRequestStruct(s_preCallStruct, REQUEST_STRUCT_SIZE);
        if (factory && !spawnMgr.IsReady()) {
            spawnMgr.SetFactory(factory);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // SpawnManager propagation failed — non-fatal, will retry post-load
    }
}

// ── FIX-FACTORY-EARLY: promoción del factory capturado por el hook al SpawnManager ──
// Llamado desde el path de passthrough en el 1er create de la carga. Valida que el
// factory es un puntero de heap real (descarta la basura de 32 bits del global) y, si
// lo es, lo promueve con SetFactory() -> IsReady()=true -> el heap scan y el spawn de
// remotos arrancan. SEH-safe y barato: solo valida punteros y llama a SetFactory.
// EN: Promotes the hook-captured factory to SpawnManager. Called from the passthrough
//     path on loading creates. If the factory is a real heap pointer it calls
//     SetFactory() -> IsReady()=true, so heap scan and remote spawning can start.
//     SEH-safe and cheap; logs only the first capture.
static void SEH_PromoteEarlyFactory(void* factory) {
    __try {
        if (!IsRealHeapFactory(factory)) {
            return;  // no es un factory de heap válido -> esperar al siguiente create
        }
        auto& spawnMgr = Core::Get().GetSpawnManager();
        if (!spawnMgr.IsReady()) {
            spawnMgr.SetFactory(factory);  // IsReady() pasa a true sin esperar a OnGameLoaded
        }
        if (!s_earlyFactoryLogged) {
            s_earlyFactoryLogged = true;
            uintptr_t p = reinterpret_cast<uintptr_t>(factory);
            uintptr_t vt = 0; Memory::Read(p, vt);  // 1er qword = vtable (diagnóstico)
            char dbg[192];
            sprintf_s(dbg, "KMP: [FIX-FACTORY-EARLY] capturado factory=0x%llX "
                           "(mitad alta NO nula, vt=0x%llX) en create #%d — IsReady()=true\n",
                      (unsigned long long)p, (unsigned long long)vt,
                      s_loadingCreateCount.load(std::memory_order_relaxed));
            OutputDebugStringA(dbg);
            spdlog::info("[FIX-FACTORY-EARLY] capturado factory=0x{:X} (vt=0x{:X}) en create #{} "
                         "— promovido a SpawnManager (IsReady()=true)",
                         p, vt, s_loadingCreateCount.load(std::memory_order_relaxed));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // promoción best-effort — no fatal, se reintenta en el siguiente create
    }
}

// ── SEH-safe entity data reading (pure C, no C++ objects) ──
// ES: Resultado de leer y registrar una entidad propia: datos del personaje, netId,
//     dueño, id y nombre de la plantilla (UTF-8) y si se registró.
// EN: Result of reading and registering one of our entities: character data, netId,
//     owner, template id and name (UTF-8), and whether it was registered.
struct SEH_EntityInfo {
    SEH_CharData charData;
    EntityID netId;
    PlayerID owner;
    uint32_t templateId;
    char templateNameBuf[256];
    int templateNameLen;
    bool registered;
};

// ES: Lee el personaje recién creado y, SOLO si pertenece a la facción del jugador
//     local, lo registra en el EntityRegistry como NPC propio. También lee de
//     templateData el id de plantilla (+0x08) y su nombre (std::string en +0x28).
//     Posición (0,0,0) = aún no colocado -> se ignora. Protegido con SEH.
// EN: Reads the freshly created character and, ONLY if it belongs to the local
//     player's faction, registers it in the EntityRegistry as our own NPC. It also
//     reads the template id (+0x08) and name (std::string at +0x28) from templateData.
//     Position (0,0,0) = not placed yet -> ignored. SEH-protected.
static SEH_EntityInfo SEH_ReadAndRegisterEntity(void* character, void* templateData) {
    SEH_EntityInfo info{};
    info.registered = false;
    __try {
        auto& coreRef = Core::Get();
        info.charData = SEH_ReadCharacterData(character);
        if (!info.charData.valid) return info;
        if (info.charData.position.x == 0.f && info.charData.position.y == 0.f && info.charData.position.z == 0.f) return info;

        // ── Faction matching: only register entities that belong to the LOCAL player ──
        // ES: Solo se registran entidades de la facción LOCAL. Prioridad: PlayerController
        //     -> facción elegida por votación -> facción de respaldo.
        // EN: Priority chain: PlayerController → elected faction (multi-source voting) → fallback
        uintptr_t playerFaction = coreRef.GetPlayerController().GetLocalFactionPtr();

        if (playerFaction == 0) {
            // Use faction elected during savegame loading (multi-source voting with name match)
            playerFaction = s_earlyPlayerFaction.load(std::memory_order_relaxed);
        }
        if (playerFaction == 0) {
            // Last resort: any valid faction seen from recent creates
            playerFaction = s_fallbackFaction.load(std::memory_order_relaxed);
        }

        // If we found a faction from loading/fallback but PlayerController doesn't have it, set it
        if (playerFaction != 0 && coreRef.GetPlayerController().GetLocalFactionPtr() == 0) {
            const_cast<PlayerController&>(coreRef.GetPlayerController())
                .SetLocalFactionPtr(playerFaction);
            spdlog::info("SEH_ReadAndRegisterEntity: Set local faction 0x{:X} from early/fallback capture", playerFaction);
        }

        // Must have a player faction AND entity must match it — prevents registering random NPCs/buildings
        if (playerFaction == 0 || info.charData.factionPtr != playerFaction) return info;

        info.owner = coreRef.GetLocalPlayerId();
        info.netId = coreRef.GetEntityRegistry().Register(
            character, EntityType::NPC, info.owner);
        coreRef.GetEntityRegistry().UpdatePosition(info.netId, info.charData.position);

        info.templateId = 0;
        info.templateNameLen = 0;
        memset(info.templateNameBuf, 0, sizeof(info.templateNameBuf));
        if (templateData) {
            Memory::Read(reinterpret_cast<uintptr_t>(templateData) + 0x08, info.templateId);
            // Read Kenshi string manually (SSO-aware) to avoid std::string in SEH
            uintptr_t strAddr = reinterpret_cast<uintptr_t>(templateData) + 0x28;
            uint64_t strSize = 0, strCap = 0;
            Memory::Read(strAddr + 0x10, strSize);
            Memory::Read(strAddr + 0x18, strCap);
            if (strSize > 0 && strSize < 255) {
                uintptr_t dataPtr = strAddr; // SSO: data inline
                if (strCap > 15) Memory::Read(strAddr, dataPtr); // heap
                SEH_MemcpySafe(info.templateNameBuf, reinterpret_cast<void*>(dataPtr), (size_t)strSize);
                // Convert from system ANSI codepage to UTF-8 (handles CJK on Chinese/Japanese/Korean Windows)
                info.templateNameLen = kmp::AnsiToUtf8InPlace(
                    info.templateNameBuf, (int)strSize, (int)sizeof(info.templateNameBuf));
            }
        }

        info.registered = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_readCrash = 0;
        if (++s_readCrash <= 5) {
            OutputDebugStringA("KMP: SEH_ReadAndRegisterEntity CRASHED (SEH caught)\n");
        }
    }
    return info;
}

// ── Connected post-processing: reads entity data (SEH), then builds & sends packet ──
// ES: Post-proceso en modo conectado: registra la entidad (parte peligrosa con SEH en
//     la función de arriba) y envía al servidor un C2S_EntitySpawnReq fiable con netId,
//     tipo, dueño, plantilla, posición, rotación comprimida, facción y nombre. Sin
//     __try aquí porque PacketWriter tiene destructor (error C2712 de MSVC).
// EN: No __try here — PacketWriter has a destructor (C2712). The dangerous part
// (game memory reads) is in SEH_ReadAndRegisterEntity above.
static void SEH_ConnectedPostProcess(void* character, void* templateData, int connectedCreateNum) {
    SEH_EntityInfo info = SEH_ReadAndRegisterEntity(character, templateData);
    if (!info.registered) return;

    if (info.charData.factionPtr != 0 && connectedCreateNum <= 3) {
        char dbgBuf[128];
        sprintf_s(dbgBuf, "KMP: FACTION BOOTSTRAP — faction 0x%llX\n",
                  (unsigned long long)info.charData.factionPtr);
        OutputDebugStringA(dbgBuf);
    }

    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_EntitySpawnReq);
    writer.WriteU32(info.netId);
    writer.WriteU8(static_cast<uint8_t>(EntityType::NPC));
    writer.WriteU32(info.owner);
    writer.WriteU32(info.templateId);
    writer.WriteF32(info.charData.position.x);
    writer.WriteF32(info.charData.position.y);
    writer.WriteF32(info.charData.position.z);
    writer.WriteU32(info.charData.rotation.Compress());
    writer.WriteU32(info.charData.factionId);
    writer.WriteU16(static_cast<uint16_t>(info.templateNameLen));
    if (info.templateNameLen > 0)
        writer.WriteRaw(info.templateNameBuf, info.templateNameLen);

    Core::Get().GetClient().SendReliable(writer.Data(), writer.Size());
}

// ═══════════════════════════════════════════════════════════════════════════
//  Hook_CharacterCreate
//
//  ES: Detour de RootObjectFactory::process (RVA 0x581770, patrón "CharacterSpawn",
//      prólogo `mov rax, rsp` -> envuelto por el wrapper MovRaxRsp). Corre en el hilo
//      de juego cada vez que el motor crea un personaje. Caminos, en orden:
//      0) Guard de reentrada y bypass de spawn directo -> solo llama a la original.
//      1) LOADING PASSTHROUGH: timestamp + contador, guarda/promueve la fábrica, copia
//         la estructura de petición la 1ª vez, llama a la original y captura
//         plantillas "Player N" del mod.
//      2) NO CONECTADO (sin passthrough): captura pre-call + fábrica, votación de
//         facción con los primeros personajes y luego vuelve a passthrough.
//      3) CONECTADO en ráfaga (>5 creates en 100 ms, carga de zona): passthrough ligero.
//      4) CONECTADO normal: refresca la copia pre-call, llama a la original, detecta
//         offsets de posición/GameData, secuestra el NPC si hay un spawn remoto
//         pendiente, instala el destroy hook diferido y registra/envía los propios.
//      Nota: el texto inglés de abajo ("First 2 calls", "raw trampoline") describe una
//      versión anterior; hoy se usan FACTION_SCAN_WINDOW creates y CallOriginalCreate.
//  EN: Detour of RootObjectFactory::process (RVA 0x581770, "CharacterSpawn" pattern,
//      `mov rax, rsp` prologue -> wrapped by the MovRaxRsp wrapper). Runs on the game
//      thread every time the engine creates a character. Paths as numbered above
//      (guard/bypass, loading passthrough, not connected + faction voting, connected
//      burst passthrough, full connected path). Note: the older text below ("First 2
//      calls", "raw trampoline") describes a previous version.
//
//  Two modes:
//  (A) LOADING MODE (not connected): First 2 calls capture factory + template
//      data for SpawnManager. After call 2, sets the software bypass flag so
//      the remaining 100+ loading creates go directly to the raw trampoline
//      (zero overhead, no MovRaxRsp stack swap).
//  (B) CONNECTED MODE: Full hook body — entity registration, spawn replay,
//      faction bootstrap, packet sending.
// ═══════════════════════════════════════════════════════════════════════════

static void* __fastcall Hook_CharacterCreate(void* factory, void* templateData) {
    // ── Re-entrancy guard ──
    // ES: Si el hook se reentra (la original crea otro personaje dentro), se llama
    //     directo al trampolín crudo para no pisar las ranuras del wrapper MovRaxRsp.
    // EN: If re-entered (the original creates another character inside), call the raw
    //     trampoline directly so the MovRaxRsp wrapper's slots are not clobbered.
    static thread_local int s_hookDepth = 0;
    if (s_hookDepth > 0) {
        if (s_rawCreateTrampoline)
            return s_rawCreateTrampoline(factory, templateData);
        return CallOriginalCreate(factory, templateData);
    }
    s_hookDepth++;

    // ES: Bypass de spawn directo (SpawnManager llamando a la fábrica): sin lógica.
    // EN: Direct spawn bypass (SpawnManager calling factory — skip all logic)
    if (s_directSpawnBypass.load(std::memory_order_acquire)) {
        void* r = CallOriginalCreate(factory, templateData);
        s_hookDepth--;
        return r;
    }

    int createNum = s_totalCreates.fetch_add(1) + 1;
    g_lastCharacterCreateNum = createNum;

    // ── Always update loading detection timestamp ──
    // ES: Siempre se actualiza la marca de tiempo del último create; PollForGameLoad la
    //     usa para detectar el fin de la ráfaga de carga sin CharacterIterator.
    // EN: PollForGameLoad uses this to detect when the loading burst has finished
    // without needing CharacterIterator (which corrupts the heap during loading).
    {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        s_lastCreateTimeMs.store(ms, std::memory_order_relaxed);
    }

    // ── Loading passthrough: ultra-lightweight mode ──
    // ES: Durante la carga de partida el hook hace lo mínimo: contador, guardar/promover
    //     la fábrica, copiar la estructura de petición la 1ª vez, llamar a la original y
    //     leer el nombre para capturar plantillas del mod. Seguro con MovRaxRsp porque
    //     usa poca pila y las llamadas son secuenciales.
    // EN: During savegame loading, the hook only updates timestamp/counter and calls
    // original. No game memory reads, no faction voting, no entity registration.
    // Safe with MovRaxRsp because minimal stack usage + sequential (not reentrant).
    if (s_loadingPassthrough.load(std::memory_order_acquire)) {
        s_loadingCreateCount.fetch_add(1, std::memory_order_relaxed);

        // One-time factory pointer save (no game memory read, just saving the arg)
        if (!s_savedFactory) s_savedFactory = factory;

        // ── FIX-FACTORY-EARLY: promoción del factory al SpawnManager en passthrough ──
        // Ésta es la captura temprana: el 1er arg de process() es el RootObjectFactory*
        // (this). En cuanto vemos un factory de heap válido, lo promovemos -> IsReady()
        // pasa a true sin esperar a OnGameLoaded. Se intenta en CADA create hasta lograrlo
        // (no solo en el 1º) por si el 1er create trae un arg no plausible. Coste: validar
        // 1 puntero + (una vez) SetFactory. NO ejecuta lógica pesada — seguro en passthrough.
        // EN: Early capture: promote the factory (1st arg = RootObjectFactory* this) as
        //     soon as it looks like a valid heap pointer; retried on every create until
        //     it succeeds. Cheap, no heavy logic.
        if (kCaptureFactoryEarly && factory) {
            SEH_PromoteEarlyFactory(factory);
        }

        // Capture pre-call data from the FIRST call during loading (just memcpy the arg)
        if (templateData && !s_havePreCallData) {
            if (SEH_MemcpySafe(s_preCallStruct, templateData, REQUEST_STRUCT_SIZE)) {
                s_havePreCallData = true;
                // Propagate to SpawnManager via SEH wrapper (separate function
                // required because Hook_CharacterCreate has C++ objects with dtors)
                SEH_PropagateToSpawnManager(templateData, factory);
            }
        }

        void* r = CallOriginalCreate(factory, templateData);

        // ── Mod template character name capture ──
        // ES: Lee (SEH) el nombre del personaje creado; si es "Player N" (1-16) guarda
        //     el puntero para no tener que escanear el heap tras la carga.
        // EN: SEH-safe read just the name string. If it matches "Player N" (1-16),
        // store the pointer so post-load code doesn't need a heap scan.
        if (r && s_capturedModTemplateCount.load(std::memory_order_relaxed) < MAX_MOD_TEMPLATE_CAPTURES) {
            char nameBuf[64] = {};
            int nameLen = SEH_ReadCharName(r, nameBuf, sizeof(nameBuf));
            if (nameLen >= 8 && nameLen <= 9) { // "Player 1" (8) to "Player 16" (9)
                if (strncmp(nameBuf, "Player ", 7) == 0) {
                    int num = atoi(nameBuf + 7);
                    if (num >= 1 && num <= 16) {
                        int idx = s_capturedModTemplateCount.fetch_add(1, std::memory_order_relaxed);
                        if (idx < MAX_MOD_TEMPLATE_CAPTURES) {
                            s_capturedModTemplates[idx] = r;
                            // Log first few captures only
                            if (idx < 3) {
                                char dbg[128];
                                sprintf_s(dbg, "KMP: Loading captured mod template '%s' at 0x%llX\n",
                                          nameBuf, (unsigned long long)reinterpret_cast<uintptr_t>(r));
                                OutputDebugStringA(dbg);
                            }
                        }
                    }
                }
            }
        }

        s_hookDepth--;
        return r;
    }

    // ES: Guardado único del puntero a la fábrica.
    // EN: One-time factory pointer save
    if (!s_savedFactory) s_savedFactory = factory;

    auto& coreRef = Core::Get();

    // ── NOT CONNECTED: capture factory+pre-call data, then passthrough ──
    // ES: Aun sin conexión se capturan fábrica y pre-call para que el sistema de spawn
    //     esté listo al conectar (evita tener que "pasear cerca de una ciudad"). Usa el
    //     wrapper MovRaxRsp, seguro porque los creates de carga son secuenciales.
    // EN: Even when not connected, capture factory pointer and pre-call data so
    // the spawn system is ready when the player connects. This prevents the
    // "walk near a town" requirement — data is captured during loading.
    // Uses s_origCreate (MovRaxRsp wrapper) which correctly restores RSP for
    // the original function. Safe here because loading creates are sequential
    // (not reentrant), so the wrapper's global slots don't conflict.
    if (!coreRef.IsConnected()) {
        // Capture pre-call data + factory from the FIRST call, then accumulate
        // faction votes from the first FACTION_SCAN_WINDOW characters to handle
        // the case where the first character is a hired NPC, not the squad leader.
        // After the scan window closes, elect the best faction and DISABLE the hook.
        // ES: Captura pre-call + fábrica del 1er create y acumula votos de facción de los
        //     primeros FACTION_SCAN_WINDOW personajes (el 1º puede ser un NPC contratado).
        //     Al cerrar la ventana se elige facción y se pasa a passthrough.
        if (!s_loadingCapturesDone) {
            // ES: Pre-call + fábrica: una sola vez, del primer personaje.
            // EN: Pre-call data + factory: capture once from the first character
            if (templateData && !s_havePreCallData) {
                if (SEH_MemcpySafe(s_preCallStruct, templateData, REQUEST_STRUCT_SIZE)) {
                    s_havePreCallData = true;
                    coreRef.GetSpawnManager().SetPreCallData(
                        s_preCallStruct, REQUEST_STRUCT_SIZE,
                        reinterpret_cast<uintptr_t>(templateData));
                    coreRef.GetSpawnManager().SetSavedRequestStruct(
                        s_preCallStruct, REQUEST_STRUCT_SIZE);
                }
            }
            if (factory && !coreRef.GetSpawnManager().IsReady()) {
                coreRef.GetSpawnManager().SetFactory(factory);
            }

            void* r = CallOriginalCreate(factory, templateData);

            // ── Multi-source faction voting ──
            // ES: Voto por personaje: +1 base, +10 si el nombre coincide con playerName
            //     y +2 extra si la facción tiene isPlayerFaction.
            // EN: Instead of trusting only the first character, accumulate votes
            // from the first FACTION_SCAN_WINDOW characters. A character whose
            // name matches the config playerName gets a large bonus.
            if (r && !s_factionVotingDone) {
                s_factionScanCount++;

                uintptr_t fac = SEH_ReadFaction(r);
                if (fac != 0) {
                    UpdateFallbackFaction(fac);

                    // Check if this character's name matches the config player name
                    bool isNameMatch = false;
                    char charName[64] = {};
                    int nameLen = SEH_ReadCharName(r, charName, sizeof(charName));
                    if (nameLen > 0) {
                        const std::string& cfgName = coreRef.GetConfig().playerName;
                        if (cfgName.size() > 0 && cfgName.size() == (size_t)nameLen &&
                            _strnicmp(charName, cfgName.c_str(), nameLen) == 0) {
                            isNameMatch = true;
                        }
                    }

                    // Also check the isPlayerFaction flag on the faction object
                    bool isFlaggedPlayer = SEH_CheckIsPlayerFaction(fac);
                    if (isFlaggedPlayer) {
                        // isPlayerFaction flag is a strong signal — boost score significantly
                        RecordFactionVote(fac, false); // Extra vote
                        RecordFactionVote(fac, false); // Extra vote (total +3 with the main one below)
                    }

                    RecordFactionVote(fac, isNameMatch);

                    if (isNameMatch) {
                        spdlog::info("entity_hooks: Loading char #{} NAME MATCH '{}' faction=0x{:X}",
                                     s_factionScanCount, charName, fac);
                    }
                }

                // ES: Se pasa a SpawnManager el primer personaje escaneado.
                // EN: Feed SpawnManager the first character with valid data
                if (s_factionScanCount == 1) {
                    SEH_FeedSpawnManager(factory, templateData, r);
                }

                // ES: Tras escanear suficientes personajes O con una coincidencia de
                //     nombre, se elige la mejor facción y se fija.
                // EN: After scanning enough characters OR if a name match gave us certainty,
                // elect the best faction and lock it.
                bool haveNameMatch = false;
                for (int i = 0; i < s_factionCandidateCount; i++) {
                    if (s_factionCandidates[i].nameMatched) { haveNameMatch = true; break; }
                }

                if (s_factionScanCount >= FACTION_SCAN_WINDOW || haveNameMatch) {
                    uintptr_t bestFac = ElectBestFaction();
                    if (bestFac != 0) {
                        s_earlyPlayerFaction.store(bestFac, std::memory_order_relaxed);
                        s_earlyFactionLocked.store(true, std::memory_order_relaxed);
                        spdlog::info("entity_hooks: Faction ELECTED 0x{:X} after {} chars ({} candidates, nameMatch={})",
                                     bestFac, s_factionScanCount, s_factionCandidateCount, haveNameMatch);
                        // Log all candidates for diagnostics
                        for (int i = 0; i < s_factionCandidateCount; i++) {
                            spdlog::info("  candidate[{}]: 0x{:X} score={} nameMatch={}",
                                         i, s_factionCandidates[i].ptr, s_factionCandidates[i].score,
                                         s_factionCandidates[i].nameMatched);
                        }
                    }
                    s_factionVotingDone = true;
                }
            }

            // ES: Tope de seguridad: si tras 2x la ventana (+2) la votación no terminó, se
            //     fuerza la elección con lo que haya.
            // EN: DISABLE hook once we have pre-call data + factory AND faction voting is done.
            // Safety: also disable after 2x the scan window to prevent hanging in the hook
            // if characters consistently fail to create or have no factions.
            bool safetyTimeout = (createNum > FACTION_SCAN_WINDOW * 2 + 2);
            if (safetyTimeout && !s_factionVotingDone) {
                // Force election with whatever we have
                uintptr_t bestFac = ElectBestFaction();
                if (bestFac != 0) {
                    s_earlyPlayerFaction.store(bestFac, std::memory_order_relaxed);
                    s_earlyFactionLocked.store(true, std::memory_order_relaxed);
                    spdlog::warn("entity_hooks: Faction SAFETY ELECT 0x{:X} after {} creates (voting stalled)",
                                 bestFac, createNum);
                }
                s_factionVotingDone = true;
            }

            // ES: Con todo capturado, se vuelve a passthrough para el resto de la carga.
            // EN: Switch to loading passthrough for remaining loads.
            // This prevents 100+ calls through full hook body during savegame load.
            if (s_havePreCallData && s_savedFactory && s_factionVotingDone) {
                s_loadingCapturesDone = true;
                s_loadingPassthrough.store(true, std::memory_order_release);
                OutputDebugStringA("KMP: Loading passthrough re-enabled after faction voting\n");
            }

            s_hookDepth--;
            return r;
        }

        // ES: Capturas hechas pero el hook sigue activo sin passthrough: solo la original.
        // EN: If we reach here, captures are done but hook is somehow still active.
        // Use CallOriginalCreate which prefers the MovRaxRsp wrapper (correct RSP).
        void* r = CallOriginalCreate(factory, templateData);
        s_hookDepth--;
        return r;
    }

    // ES: A partir de aquí, MODO CONECTADO. Contador de creates (log mínimo para no
    //     presionar el heap en contexto MovRaxRsp).
    // EN: Connected create counter (minimal logging to avoid heap pressure in MovRaxRsp context)
    int connNum = s_connectedCreateNum.fetch_add(1) + 1;
    if (connNum <= 5) {
        char dbgBuf[128];
        sprintf_s(dbgBuf, "KMP: Connected CharacterCreate #%d\n", connNum);
        OutputDebugStringA(dbgBuf);
    }

    // ES: Detección de ráfaga: >5 creates en 100 ms = carga de zona -> modo ligero.
    // EN: Rapid-fire detection: if >5 creates in 100ms, go lightweight (zone load burst)
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_connectedBurstStart);
    if (elapsed.count() < 100) {
        s_connectedBurstCount++;
    } else {
        s_connectedBurstStart = now;
        s_connectedBurstCount = 1;
    }

    // ES: En ráfaga: passthrough ligero; solo se actualiza la facción de respaldo si
    //     aún no hay facción fijada (sin fijarla: pueden ser NPCs ajenos).
    // EN: During zone-load burst (>5 creates in 100ms): lightweight passthrough
    // Still capture faction if we haven't locked one yet (backup path).
    if (s_connectedBurstCount > 5) {
        if (s_connectedBurstCount <= 10 || s_connectedBurstCount % 50 == 0) {
            spdlog::debug("entity_hooks: BURST passthrough #{}", s_connectedBurstCount.load());
        }
        void* character = CallOriginalCreate(factory, templateData);

        // Lightweight faction capture — only if we still don't have a player faction
        if (character && !s_earlyFactionLocked.load(std::memory_order_relaxed)) {
            uintptr_t fac = SEH_ReadFaction(character);
            if (fac != 0) {
                UpdateFallbackFaction(fac);
                // Don't lock here — burst characters may be NPCs, not player chars.
                // Loading path locks it; this is just a backup for fallback.
            }
        }

        s_hookDepth--;
        return character;
    }

    // ES: Copia local de la estructura de petición antes de llamar a la original (para
    //     detectar offsets) y refresco de la copia global que usa SpawnManager, porque
    //     la de la carga tiene punteros externos caducados.
    // EN: Pre-call struct capture (needed for in-place spawn replay)
    uint8_t localPreCall[REQUEST_STRUCT_SIZE];
    bool haveLocalPreCall = false;
    if (templateData) {
        memset(localPreCall, 0, REQUEST_STRUCT_SIZE);
        if (SEH_MemcpySafe(localPreCall, templateData, REQUEST_STRUCT_SIZE)) {
            haveLocalPreCall = true;
        }
        // Always refresh pre-call data from connected NPC spawns — the struct
        // captured during loading has stale external pointers (faction, squad, etc.)
        // that crash CallFactoryDirect. Fresh NPC structs have live heap pointers.
        {
            SEH_MemcpySafe(s_preCallStruct, templateData, REQUEST_STRUCT_SIZE);
            s_havePreCallData = true;
            coreRef.GetSpawnManager().SetPreCallData(
                s_preCallStruct, REQUEST_STRUCT_SIZE,
                reinterpret_cast<uintptr_t>(templateData));
            coreRef.GetSpawnManager().SetSavedRequestStruct(
                s_preCallStruct, REQUEST_STRUCT_SIZE);
        }
    }

    // ═══ CALL ORIGINAL FUNCTION ═══
    // ES: Llamada a la RootObjectFactory::process original (crea el personaje).
    // EN: Call the original RootObjectFactory::process (creates the character).
    void* character = CallOriginalCreate(factory, templateData);
    bool wasHijacked = false;

    if (character && haveLocalPreCall) {
        // ES: Detección del offset de posición: busca en la copia pre-call tres floats
        //     (paso 4 bytes) a menos de 2 unidades de la posición real del personaje.
        // EN: Position offset detection: scans the pre-call copy for three floats
        //     (4-byte stride) within 2 units of the character's real position.
        if (s_positionOffsetInStruct == -1 && s_positionDetectAttempts < 10) {
            s_positionDetectAttempts++;
            SEH_CharData charData = SEH_ReadCharacterData(character);
            float cx = charData.position.x, cy = charData.position.y, cz = charData.position.z;
            if (cx != 0.f || cy != 0.f || cz != 0.f) {
                for (int off = 0; off < (int)REQUEST_STRUCT_SIZE - 12; off += 4) {
                    float sx, sy, sz;
                    memcpy(&sx, &localPreCall[off], 4);
                    memcpy(&sy, &localPreCall[off + 4], 4);
                    memcpy(&sz, &localPreCall[off + 8], 4);
                    if (fabsf(sx - cx) < 2.0f && fabsf(sy - cy) < 2.0f && fabsf(sz - cz) < 2.0f) {
                        s_positionOffsetInStruct = off;
                        spdlog::info("entity_hooks: POSITION OFFSET at struct+0x{:X}", off);
                        break;
                    }
                }
            }
        }

        // ES: Detección del offset del puntero GameData*: busca (paso 8 bytes) el valor
        //     igual al GameData del personaje creado, para que SpawnManager pueda
        //     sustituirlo por plantillas del mod en los spawns remotos.
        // EN: GameData pointer offset detection
        // Finds where in the request struct the GameData* pointer lives, so
        // SpawnManager can swap it with mod templates for remote spawns.
        if (s_gameDataOffsetInStruct == -1 && s_gameDataDetectAttempts < 10) {
            s_gameDataDetectAttempts++;
            game::CharacterAccessor accessor(character);
            uintptr_t charGameData = accessor.GetGameDataPtr();
            if (charGameData != 0 && charGameData > 0x10000 && charGameData < 0x00007FFFFFFFFFFF) {
                for (int off = 0; off + 8 <= (int)REQUEST_STRUCT_SIZE; off += 8) {
                    uintptr_t val = 0;
                    memcpy(&val, &localPreCall[off], 8);
                    if (val == charGameData) {
                        s_gameDataOffsetInStruct = off;
                        coreRef.GetSpawnManager().SetGameDataOffset(off);
                        spdlog::info("entity_hooks: GAMEDATA OFFSET at struct+0x{:X} = 0x{:X}",
                                     off, charGameData);
                        break;
                    }
                }
            }
            // Also propagate position offset to SpawnManager
            if (s_positionOffsetInStruct >= 0) {
                coreRef.GetSpawnManager().SetPositionOffset(s_positionOffsetInStruct);
            }
        }

        // ═══ NPC HIJACK ═══
        // ES: Si hay un spawn remoto pendiente y el dueño no superó su límite, en vez de
        //     volver a llamar a la fábrica se toma el NPC que el juego acaba de crear y
        //     se convierte en el personaje del jugador remoto. Si falla, se reencola
        //     (hasta MAX_SPAWN_RETRIES); si se superó el límite, se reencola sin más.
        // EN: Instead of calling the factory again (replay), take over the NPC that
        // the game just created. The NPC already has a valid faction, model, and
        // animations — zero crash risk from faction pointer issues.
        // We just: register it, teleport it to the remote player's position,
        // rename it, and disable its AI.
        if (character && coreRef.IsGameLoaded()) {
            auto& spawnMgr = coreRef.GetSpawnManager();
            SpawnRequest spawnReq;
            if (spawnMgr.PopNextSpawn(spawnReq)) {
                bool canSpawnForPlayer = false;
                {
                    std::lock_guard lock(s_spawnsPerPlayerMutex);
                    canSpawnForPlayer = s_spawnsPerPlayer[spawnReq.owner] < MAX_SPAWNS_PER_PLAYER;
                }
                if (canSpawnForPlayer) {
                    spdlog::info("entity_hooks: NPC HIJACK for entity {} owner={} — using just-created NPC",
                                 spawnReq.netId, spawnReq.owner);

                    if (SEH_HijackNPC(character, spawnReq.netId, spawnReq.owner, spawnReq.position)) {
                        s_inPlaceSpawnCount.fetch_add(1);
                        s_lastInPlaceSpawnTime = std::chrono::steady_clock::now();
                        wasHijacked = true;
                        {
                            std::lock_guard lock(s_spawnsPerPlayerMutex);
                            s_spawnsPerPlayer[spawnReq.owner]++;
                        }
                    } else {
                        spawnReq.retryCount++;
                        if (spawnReq.retryCount < MAX_SPAWN_RETRIES)
                            spawnMgr.RequeueSpawn(spawnReq);
                    }
                } else {
                    spawnMgr.RequeueSpawn(spawnReq);
                }
            }
        }
    }

    if (!character) {
        s_hookDepth--;
        return nullptr;
    }

    // ── Instalación diferida del destroy hook (una sola vez) ──
    // EN: Lazy install of the destroy hook (only once), using this live character.
    if (!s_destroyHookInstalled) {
        TryInstallDestroyHookFromCharacter(character);
    }

    // ES: Sonda diferida de AnimClass (solo los 5 primeros personajes).
    // EN: AnimClass probe (first 5 only)
    {
        if (s_earlyProbeCount < 5) {
            game::ScheduleDeferredAnimClassProbe(reinterpret_cast<uintptr_t>(character));
            s_earlyProbeCount++;
        }
    }

    // ES: Los NPCs secuestrados ya están registrados como REMOTOS: no se registran como
    //     locales ni se envía un C2S_EntitySpawnReq espurio al servidor.
    // EN: Skip local entity registration for hijacked NPCs — they are already
    // registered as REMOTE via SEH_HijackNPC. Feeding them to SpawnManager
    // and SEH_ConnectedPostProcess would re-register as LOCAL and send a
    // spurious C2S_EntitySpawnReq to the server.
    if (!wasHijacked) {
        // Feed SpawnManager
        SEH_FeedSpawnManager(factory, templateData, character);

        // Register player faction characters in EntityRegistry (SEH-protected, no C++ objects)
        if (coreRef.IsGameLoaded()) {
            SEH_ConnectedPostProcess(character, templateData, s_connectedCreateNum);
        }
    }

    s_hookDepth--;
    return character;
}

// Hook sobre el scalar deleting destructor de Character (resuelto en runtime vía vtable[0]).
// Corre en el hilo de juego (quien destruye es el motor). Toda la limpieza del registro va
// ANTES de llamar al original: después, la memoria del Character ya no existe.
// Si hay conexión y la entidad está registrada: envía C2S_EntityDespawnReq si es nuestra,
// libera el cupo de spawn si es remota, borra su interpolación y la desregistra.
// EN: Hook on Character's scalar deleting destructor (resolved at runtime via
//     vtable[0]). Runs on the game thread (the engine destroys it). All registry
//     cleanup happens BEFORE calling the original: afterwards the Character's memory is
//     gone. When connected and registered: send C2S_EntityDespawnReq if it is ours,
//     free the spawn-cap slot if remote, drop its interpolation state and unregister it.
static void* __fastcall Hook_CharacterDestroy(void* character, unsigned int flags) {
    int destroyNum = s_totalDestroys.fetch_add(1) + 1;

    uintptr_t charAddr = reinterpret_cast<uintptr_t>(character);
    if (charAddr < 0x10000 || charAddr > 0x00007FFFFFFFFFFF) {
        return s_origDestroy(character, flags);
    }

    auto& core = Core::Get();
    if (core.IsConnected()) {
        EntityID netId = core.GetEntityRegistry().GetNetId(character);
        if (netId != INVALID_ENTITY) {
            auto info = core.GetEntityRegistry().GetInfo(netId);
            bool isOurs = info.has_value() && info->ownerPlayerId == core.GetLocalPlayerId();

            if (isOurs) {
                PacketWriter writer;
                writer.WriteHeader(MessageType::C2S_EntityDespawnReq);
                writer.WriteU32(netId);
                writer.WriteU8(0);
                core.GetClient().SendReliable(writer.Data(), writer.Size());
            }

            // BUG 2 FIX: Decrement spawn cap for remote entities so the slot
            // is freed for future spawns. Without this, the per-player cap
            // saturates and blocks all new remote entity spawns.
            if (info.has_value() && info->isRemote) {
                DecrementSpawnCount(info->ownerPlayerId);
            }

            // BUG 3 FIX: Clean up orphaned interpolation state for this entity.
            core.GetInterpolation().RemoveEntity(netId);

            core.GetEntityRegistry().Unregister(netId);
        }
    }

    // Cambio 2.3: purgar este Character del char_tracker ANTES de destruirlo. Se hace siempre
    // (haya o no conexión) porque el tracker cachea punteros de cualquier personaje del juego;
    // si no se purga, queda una entrada colgada que FindByName/FindByPtr podría devolver
    // apuntando a memoria que el motor recicla tras destruir/recargar el personaje (UAF).
    // EN: Change 2.3: purge this Character from char_tracker BEFORE destroying it,
    //     always (connected or not), since the tracker caches pointers to any character;
    //     otherwise a dangling entry could be returned by FindByName/FindByPtr (UAF).
    char_tracker_hooks::RemoveByPtr(character);

    // Destruir de verdad AL FINAL — la limpieza de arriba necesita el puntero vivo.
    // EN: Actually destroy LAST — the cleanup above needs the pointer alive.
    return s_origDestroy(character, flags);
}

// ── Instalación diferida del destroy hook vía vtable slot 0 ──
// El patrón "CharacterDestroy" del scanner resuelve a la función equivocada. El RVA del
// destructor real de Character se resuelve leyendo vtable[0] del primer Character vivo,
// siguiendo el thunk de 5 bytes si aplica, y validando con .pdata que es inicio de función real
// (mismo criterio que validateFactoryFunc en este mismo archivo).
// EN: Lazy install of the destroy hook via vtable slot 0. The scanner's
//     "CharacterDestroy" pattern resolves to the wrong function, so the real
//     Character destructor is found by reading vtable[0] of the first live Character,
//     following a 5-byte `jmp rel32` thunk if present, and validating with .pdata that
//     it is a real function start. Then it is hooked and enabled right away.
static void TryInstallDestroyHookFromCharacter(void* character) {
    if (s_destroyHookInstalled || !character) return;

    uintptr_t charAddr = reinterpret_cast<uintptr_t>(character);
    if (charAddr < 0x10000 || charAddr > 0x00007FFFFFFFFFFF || (charAddr & 0x7) != 0) return;

    uintptr_t modBase = Memory::GetModuleBase();

    uintptr_t vtable = 0;
    if (!Memory::Read(charAddr, vtable)) return;
    if (vtable < modBase || vtable >= modBase + 0x4000000) return;

    uintptr_t dtorAddr = 0;
    if (!Memory::Read(vtable, dtorAddr)) return;
    if (dtorAddr < modBase || dtorAddr >= modBase + 0x4000000) return;

    // Seguir el thunk de 5 bytes (jmp rel32) si vtable[0] no es la función real directamente.
    // EN: Follow the 5-byte thunk (jmp rel32, opcode E9) if vtable[0] is not the real function.
    uint8_t firstByte = 0;
    if (Memory::Read(dtorAddr, firstByte) && firstByte == 0xE9) {
        int32_t rel32 = 0;
        if (Memory::Read(dtorAddr + 1, rel32)) {
            uintptr_t target = dtorAddr + 5 + static_cast<intptr_t>(rel32);
            if (target >= modBase && target < modBase + 0x4000000) {
                dtorAddr = target;
            }
        }
    }

    // Validación .pdata: tiene que ser un INICIO de función real
    // EN: .pdata check: it must be the START of a real function (unwind table entry).
    DWORD64 imageBase = 0;
    auto* rtFunc = RtlLookupFunctionEntry(static_cast<DWORD64>(dtorAddr), &imageBase, nullptr);
    if (!rtFunc || static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress != dtorAddr) {
        spdlog::warn("entity_hooks: destroy-hook — vtable[0]=0x{:X} no es inicio de función — NO instalado", dtorAddr);
        return;
    }

    if (!HookManager::Get().InstallAt("CharacterDestroy", dtorAddr,
                                      &Hook_CharacterDestroy, &s_origDestroy)) {
        spdlog::error("entity_hooks: destroy-hook — InstallAt FALLÓ en 0x{:X} (RVA 0x{:X})",
                      dtorAddr, dtorAddr - modBase);
        return;
    }
    HookManager::Get().Enable("CharacterDestroy");
    s_destroyHookInstalled = true;
    spdlog::info("entity_hooks: destroy-hook INSTALADO vía vtable[0] — dtor=0x{:X} (RVA 0x{:X})",
                 dtorAddr, dtorAddr - modBase);
}

// ── Direct Call Stub Builder ──

// ES: Construye un stub ejecutable de 17 bytes: `mov rax, rsp; jmp [rip+0]; dq destino`
//     que salta a rawTrampoline+3 (saltándose el `mov rax, rsp` del trampolín) con RAX
//     = RSP del llamador. Memoria RWX al escribir y luego protegida como RX.
// EN: Build a small executable stub: `mov rax, rsp; jmp [rip+0]; dq target`
// This gives the original function the correct RAX value (our caller's RSP)
// and jumps to rawTrampoline+3 (skipping the trampoline's own `mov rax, rsp`).
// Total: 17 bytes. Allocated as PAGE_EXECUTE_READWRITE, locked to PAGE_EXECUTE_READ.
static void* BuildDirectCallStub(void* rawTrampoline) {
    void* mem = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return nullptr;

    auto* code = static_cast<uint8_t*>(mem);
    int off = 0;

    // mov rax, rsp  (48 8B C4)
    code[off++] = 0x48;
    code[off++] = 0x8B;
    code[off++] = 0xC4;

    // jmp qword ptr [rip+0]  (FF 25 00 00 00 00)
    code[off++] = 0xFF;
    code[off++] = 0x25;
    code[off++] = 0x00;
    code[off++] = 0x00;
    code[off++] = 0x00;
    code[off++] = 0x00;

    // 8-byte target: rawTrampoline + 3 (skip its mov rax, rsp)
    uintptr_t target = reinterpret_cast<uintptr_t>(rawTrampoline) + 3;
    memcpy(&code[off], &target, 8);

    DWORD oldProtect;
    VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &oldProtect);

    spdlog::info("entity_hooks: Built direct call stub at 0x{:X} -> trampoline+3 = 0x{:X}",
                 reinterpret_cast<uintptr_t>(mem), target);
    return mem;
}

// ── Install/Uninstall ──

// ES: Ver entity_hooks.h. Fases: (1) resolver y validar create/createRandomChar por
//     RVA; (2) enganchar CharacterSpawn (process) con MovRaxRsp, obtener trampolín
//     crudo y construir el stub directo; (3) poner passthrough de carga y armar el hook
//     (FIX-FACTORY-EARLY); (4) dejar el destroy hook para instalación diferida.
// EN: See entity_hooks.h. Phases: (1) resolve and validate create/createRandomChar by
//     RVA; (2) hook CharacterSpawn (process) with MovRaxRsp, grab the raw trampoline
//     and build the direct stub; (3) set loading passthrough and arm the hook
//     (FIX-FACTORY-EARLY); (4) leave the destroy hook for lazy install.
bool Install() {
    auto& core = Core::Get();
    auto& hookMgr = HookManager::Get();
    auto& funcs = core.GetGameFunctions();

    bool success = true;

    // ── Resolve higher-level factory functions from known RVAs ──
    // ES: Estas funciones NO se enganchan; se llaman directamente. Se valida con .pdata
    //     que la dirección es inicio de función y se mira el prólogo. Si falla, el
    //     puntero queda a null y el spawn recurre a createRandomChar o al secuestro de NPC.
    // EN: These are NOT hooked — called directly via function pointer. No MinHook
    // trampoline involved, so `mov rax, rsp` prologue works correctly when
    // called from C++ (CPU pushes return address, then function reads RSP).
    //
    // Validation: .pdata check confirms address is a real function start,
    // prologue byte check confirms it looks like executable code.
    // If validation fails, the function pointer is left null and spawn
    // falls back to createRandomChar or NPC hijack — never blocks startup.
    {
        uintptr_t modBase = Memory::GetModuleBase();

        // ES: Valida que addr es inicio de función (.pdata) y registra si el prólogo es
        //     típico; un prólogo raro solo avisa, no bloquea.
        // EN: Validates addr is a function start (.pdata) and logs whether the prologue
        //     looks typical; an unusual prologue only warns, it does not block.
        auto validateFactoryFunc = [](uintptr_t addr, const char* name) -> bool {
            // .pdata validation — confirms this is a real function entry point
            DWORD64 imageBase = 0;
            auto* rtFunc = RtlLookupFunctionEntry(
                static_cast<DWORD64>(addr), &imageBase, nullptr);
            if (rtFunc) {
                uintptr_t funcStart = static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
                if (funcStart != addr) {
                    spdlog::error("entity_hooks: {} at 0x{:X} is MID-FUNCTION "
                                  "(real start 0x{:X}, offset +0x{:X}) — SKIPPING",
                                  name, addr, funcStart, addr - funcStart);
                    return false;
                }
            }
            // Prologue byte check — verify it looks like a function start
            __try {
                auto* p = reinterpret_cast<const uint8_t*>(addr);
                bool validPrologue =
                    (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) || // mov rax, rsp
                    (p[0] == 0x48 && p[1] == 0x89)                  || // mov [rsp+xx], reg
                    (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC)  || // sub rsp, imm8
                    (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC)  || // sub rsp, imm32
                    (p[0] == 0x40 && p[1] >= 0x53 && p[1] <= 0x57)  || // push r64
                    (p[0] == 0x55);                                     // push rbp
                spdlog::info("entity_hooks: {} at 0x{:X} prologue: {:02X} {:02X} {:02X} {}",
                             name, addr, p[0], p[1], p[2],
                             validPrologue ? "[OK]" : "[UNUSUAL]");
                if (!validPrologue) {
                    spdlog::warn("entity_hooks: {} has unexpected prologue — may crash on call", name);
                }
                return true; // Don't block on unusual prologue — SEH catches crashes
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                spdlog::error("entity_hooks: {} at 0x{:X} — cannot read memory", name, addr);
                return false;
            }
        };

        // ES: RootObjectFactory::create (RVA 0x583400), el despachador que usan 11 sistemas del juego.
        // EN: RootObjectFactory::create — the dispatcher called by 11 game systems.
        uintptr_t createAddr = modBase + 0x583400;
        if (validateFactoryFunc(createAddr, "FactoryCreate")) {
            s_factoryCreate = reinterpret_cast<CharacterCreateFn>(createAddr);
            spdlog::info("entity_hooks: FactoryCreate VALIDATED at 0x{:X}", createAddr);
        } else {
            spdlog::warn("entity_hooks: FactoryCreate at 0x{:X} FAILED validation — "
                         "mod template spawn disabled, will use createRandomChar or NPC hijack",
                         createAddr);
        }

        // ES: RootObjectFactory::createRandomChar (RVA 0x5836E0): crea un NPC aleatorio.
        // EN: RootObjectFactory::createRandomChar — creates random NPC character.
        uintptr_t createRandomAddr = modBase + 0x5836E0;
        if (validateFactoryFunc(createRandomAddr, "CreateRandomChar")) {
            s_factoryCreateRandomChar = reinterpret_cast<CharacterCreateFn>(createRandomAddr);
            spdlog::info("entity_hooks: CreateRandomChar VALIDATED at 0x{:X}", createRandomAddr);
        } else {
            spdlog::warn("entity_hooks: CreateRandomChar at 0x{:X} FAILED validation — "
                         "random char fallback disabled, will rely on NPC hijack only",
                         createRandomAddr);
        }

        // ES: Se publican en GameFunctions para otros subsistemas (0 si no validaron).
        // EN: Store in GameFunctions for other subsystems
        core.GetGameFunctions().FactoryCreate = reinterpret_cast<void*>(
            s_factoryCreate ? createAddr : 0);
        core.GetGameFunctions().CreateRandomChar = reinterpret_cast<void*>(
            s_factoryCreateRandomChar ? createRandomAddr : 0);
    }

    // ES: funcs.CharacterSpawn = dirección de RootObjectFactory::process resuelta por el
    //     escáner de patrones. Se engancha con el nombre "CharacterCreate".
    // EN: funcs.CharacterSpawn = RootObjectFactory::process address resolved by the
    //     pattern scanner. It is hooked under the name "CharacterCreate".
    if (funcs.CharacterSpawn) {
        s_createTargetAddr = reinterpret_cast<uintptr_t>(funcs.CharacterSpawn);

        char buf[128];
        sprintf_s(buf, "KMP: entity_hooks — hooking CharacterCreate at 0x%llX\n",
                  (unsigned long long)s_createTargetAddr);
        OutputDebugStringA(buf);

        if (!hookMgr.InstallAt("CharacterCreate",
                               s_createTargetAddr,
                               &Hook_CharacterCreate, &s_origCreate)) {
            spdlog::error("entity_hooks: Failed to hook CharacterCreate");
            OutputDebugStringA("KMP: entity_hooks — InstallAt FAILED\n");
            success = false;
        } else {
            // ES: Trampolín crudo para llamadas reentrantes y stub de llamada directa.
            // EN: Get raw trampoline for reentrant calls
            void* rawTramp = hookMgr.GetRawTrampoline("CharacterCreate");
            if (rawTramp) {
                s_rawCreateTrampoline = reinterpret_cast<CharacterCreateFn>(rawTramp);
                spdlog::info("entity_hooks: Raw trampoline at 0x{:X}", reinterpret_cast<uintptr_t>(rawTramp));

                // Build direct call stub for CallFactoryDirect.
                // The stub does `mov rax, rsp; jmp rawTrampoline+3` — correct RAX
                // for functions that derive RBP from RAX (mov rax,rsp prologue).
                s_directCallStubAlloc = BuildDirectCallStub(rawTramp);
                if (s_directCallStubAlloc) {
                    s_directCallStub = reinterpret_cast<CharacterCreateFn>(s_directCallStubAlloc);
                    spdlog::info("entity_hooks: Direct call stub READY at 0x{:X}",
                                 reinterpret_cast<uintptr_t>(s_directCallStubAlloc));
                }
            }

            // ES: SpawnManager recibe la dirección original de process() (la del juego,
            //     no el trampolín).
            // EN: SpawnManager gets process()'s original address (the game's, not the trampoline).
            core.GetSpawnManager().SetOrigProcess(
                reinterpret_cast<FactoryProcessFn>(s_createTargetAddr));

            // ES: Hook instalado en modo LOADING PASSTHROUGH: los 130+ creates de la
            //     carga siguen el camino ultraligero, y PollForGameLoad detecta el fin
            //     de la carga por estos eventos en vez de por CharacterIterator.
            // EN: Hook installed in LOADING PASSTHROUGH mode. The 130+ character
            // creates during savegame load go through the hook but take the
            // ultra-lightweight path: just update timestamp/counter, capture
            // factory pointer, check for mod template names, and call original.
            // No game memory reads beyond SEH_ReadCharName (SEH-protected).
            // This lets PollForGameLoad detect loading completion via create
            // events instead of CharacterIterator (which corrupts the heap
            // when reading the lektor during active resizing).
            s_loadingPassthrough.store(true, std::memory_order_release);
            spdlog::info("entity_hooks: CharacterCreate installed in LOADING PASSTHROUGH mode");
            OutputDebugStringA("KMP: entity_hooks — CharacterCreate installed (loading passthrough)\n");

            // ── FIX-FACTORY-EARLY: armar el hook DESDE Install() ──
            // InstallAt() deja el hook MovRaxRsp con bypass=1 ("starts BYPASSED"): el naked
            // detour cortocircuita y NUNCA entra al cuerpo del hook, así que la captura del
            // factory en passthrough no ocurría durante la carga del save -> m_factory=0.
            // Limpiamos el bypass aquí (Enable() = InterlockedExchange del flag a 0, barato y
            // seguro) dejando s_loadingPassthrough=true: el cuerpo corre, pero solo el path
            // ultra-ligero (timestamp + guardar factory + promover + delegar al original).
            // Resultado: el factory se captura en el 1er create de la carga.
            // EN: Arm the hook FROM Install(): InstallAt() leaves it bypassed, so the
            //     body never ran during save loading and m_factory stayed 0. Clearing
            //     the bypass here (Enable() = cheap interlocked flag write) while keeping
            //     s_loadingPassthrough=true captures the factory on the first create.
            if (kCaptureFactoryEarly) {
                if (hookMgr.Enable("CharacterCreate")) {
                    spdlog::info("entity_hooks: [FIX-FACTORY-EARLY] hook ARMADO desde Install() "
                                 "(bypass=0, loading passthrough) — captura temprana del factory activa");
                    OutputDebugStringA("KMP: [FIX-FACTORY-EARLY] CharacterCreate armado en Install "
                                       "(passthrough, captura temprana del factory)\n");
                } else {
                    spdlog::warn("entity_hooks: [FIX-FACTORY-EARLY] Enable() falló en Install() — "
                                 "el hook sigue bypassed; la captura temprana NO estará activa");
                }
            } else {
                spdlog::info("entity_hooks: [FIX-FACTORY-EARLY] desactivado (kCaptureFactoryEarly=false) "
                             "— hook arranca BYPASSED (comportamiento previo)");
            }
        }
    }

    // CharacterDestroy: NO se instala aquí a propósito. El patrón del scanner resuelve a
    // NodeList::destroyNodesByBuilding — función equivocada. El hook real se instala DIFERIDO vía
    // TryInstallDestroyHookFromCharacter() (vtable slot 0 del primer Character vivo, siguiendo el
    // thunk si aplica), llamado desde Hook_CharacterCreate (conectado) y ResumeForNetwork().
    // EN: CharacterDestroy is deliberately NOT installed here: the scanner pattern
    //     resolves to NodeList::destroyNodesByBuilding (wrong function). The real hook is
    //     installed lazily by TryInstallDestroyHookFromCharacter() (vtable slot 0 of the
    //     first live Character), called from Hook_CharacterCreate (connected) and
    //     ResumeForNetwork().
    s_destroyHookInstalled = false;

    spdlog::info("entity_hooks: Installed (create={}, destroy={})",
                 funcs.CharacterSpawn != nullptr, s_destroyHookInstalled);
    return success;
}

// ES: Quita los hooks de create/destroy y libera el stub de llamada directa.
// EN: Removes the create/destroy hooks and frees the direct call stub.
void Uninstall() {
    HookManager::Get().Remove("CharacterCreate");
    if (s_destroyHookInstalled) {
        HookManager::Get().Remove("CharacterDestroy");
    }
    // Free direct call stub
    if (s_directCallStubAlloc) {
        VirtualFree(s_directCallStubAlloc, 0, MEM_RELEASE);
        s_directCallStubAlloc = nullptr;
        s_directCallStub = nullptr;
    }
}

// ES: Al conectar: resetea contadores y estado de captura, pasa la facción elegida en
//     la carga al PlayerController, reinicia la votación, quita el passthrough (cuerpo
//     completo del hook), asegura el hook activo e intenta instalar el destroy hook con
//     una plantilla capturada. Puede llamarse desde el hilo de red.
// EN: On connect: resets counters and capture state, hands the faction elected while
//     loading to PlayerController, resets voting, disables passthrough (full hook body),
//     makes sure the hook is enabled and tries to install the destroy hook using a
//     captured template. May be called from the network thread.
void ResumeForNetwork() {
    // ES: Reinicia los límites de spawn por jugador para la nueva conexión.
    // EN: Reset per-player spawn caps for new connection
    {
        std::lock_guard lock(s_spawnsPerPlayerMutex);
        s_spawnsPerPlayer.clear();
    }

    // Reset connected-mode statics that don't reset naturally on reconnect
    s_connectedCreateNum.store(0);
    s_connectedBurstCount.store(0);
    s_inPlaceSpawnCount.store(0);
    s_earlyProbeCount.store(0);

    // Reset loading capture state so fresh factory/template/faction data is
    // captured on the next savegame load or reconnect.
    s_loadingCapturesDone = false;
    s_havePreCallData = false;
    s_savedFactory = nullptr;

    // FIX-FACTORY-EARLY: re-armar el log de captura para la próxima carga/reconexión.
    // (no afecta a la lógica: la promoción se rige por SpawnManager::IsReady()).
    s_earlyFactoryLogged = false;

    // Propagate early faction to PlayerController if we captured one during loading
    // (must read BEFORE resetting the atomic)
    uintptr_t earlyFac = s_earlyPlayerFaction.load(std::memory_order_relaxed);
    if (earlyFac != 0 && Core::Get().GetPlayerController().GetLocalFactionPtr() == 0) {
        const_cast<PlayerController&>(Core::Get().GetPlayerController())
            .SetLocalFactionPtr(earlyFac);
        spdlog::info("entity_hooks: ResumeForNetwork — set PlayerController faction to 0x{:X} (from loading capture)",
                     earlyFac);
    }

    // Reset faction capture state after propagation so next load captures fresh data
    s_earlyFactionLocked.store(false);
    s_earlyPlayerFaction.store(0);

    // Reset multi-source faction voting state for next load
    memset(s_factionCandidates, 0, sizeof(s_factionCandidates));
    s_factionCandidateCount = 0;
    s_factionScanCount = 0;
    s_factionVotingDone = false;

    // ES: Quita el passthrough de carga: cuerpo completo del hook para multijugador.
    //     La carga ya terminó; solo habrá spawns sueltos, que MovRaxRsp maneja bien.
    // EN: Disable loading passthrough — full hook body active for multiplayer.
    // At this point loading is complete — only single/few runtime spawns will
    // trigger the hook (new zone NPCs, remote player injection). MovRaxRsp
    // handles single calls fine; it's only the 130+ loading burst that uses
    // the lightweight passthrough path.
    s_loadingPassthrough.store(false, std::memory_order_release);

    // Ensure hook is enabled (should already be, but re-enable in case
    // the loading capture code path disabled it via HookManager::Disable)
    if (HookManager::Get().Enable("CharacterCreate")) {
        spdlog::info("entity_hooks: ResumeForNetwork — CharacterCreate hook ENABLED (full mode)");
    } else {
        spdlog::warn("entity_hooks: ResumeForNetwork — CharacterCreate Enable() returned false");
    }

    // ── Instalación diferida del destroy hook con un mod template capturado ──
    // EN: Lazy destroy-hook install using a captured mod template character.
    if (!s_destroyHookInstalled) {
        void* tmpl[1] = {};
        if (GetCapturedModTemplates(tmpl, 1) > 0) {
            TryInstallDestroyHookFromCharacter(tmpl[0]);
        }
    }

    spdlog::info("entity_hooks: ResumeForNetwork — hook active for runtime spawns "
                 "(earlyFaction=0x{:X}, fallback=0x{:X})", earlyFac,
                 s_fallbackFaction.load(std::memory_order_relaxed));
}

// ES: Al desconectar: vuelve al modo passthrough ligero (el hook sigue instalado).
// EN: On disconnect: back to lightweight passthrough (the hook stays installed).
void SuspendForDisconnect() {
    // Switch to loading passthrough mode when disconnecting from multiplayer.
    // Zone-load bursts (90+ creates) still fire through the hook, but the
    // lightweight passthrough path only updates timestamp/counter — no game
    // memory reads, no entity registration, no faction voting.
    s_loadingPassthrough.store(true, std::memory_order_release);
    spdlog::info("entity_hooks: SuspendForDisconnect — loading passthrough enabled");
}

// ES: Getters y utilidades simples; ver entity_hooks.h.
// EN: Simple getters and helpers; see entity_hooks.h.
bool HasRecentInPlaceSpawn(int withinSeconds) {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s_lastInPlaceSpawnTime);
    return elapsed.count() < withinSeconds;
}

int GetInPlaceSpawnCount() {
    return s_inPlaceSpawnCount.load();
}

void DecrementSpawnCount(uint32_t owner) {
    std::lock_guard lock(s_spawnsPerPlayerMutex);
    auto it = s_spawnsPerPlayer.find(owner);
    if (it != s_spawnsPerPlayer.end() && it->second > 0) {
        it->second--;
        spdlog::debug("entity_hooks: DecrementSpawnCount owner={} -> {}", owner, it->second);
    }
}

int GetTotalCreates() {
    return s_totalCreates.load();
}

int GetTotalDestroys() {
    return s_totalDestroys.load();
}

// ES: Llama a process() por el stub directo (o el trampolín crudo) con el bypass de
//     spawn directo activo para que el hook no reprocese el create. SEH captura crashes.
// EN: Calls process() via the direct stub (or raw trampoline) with the direct spawn
//     bypass set so the hook does not re-process the create. SEH catches crashes.
void* CallFactoryDirect(void* factory, void* requestStruct) {
    // Use the direct call stub which sets RAX = caller's RSP before entering
    // the original function body. Without this, the raw trampoline's
    // `mov rax, rsp` captures the wrong RSP, causing all [rbp+XX] accesses
    // to read/write wrong memory (sign-extended faction pointers, stack corruption).
    CharacterCreateFn callFn = s_directCallStub ? s_directCallStub : s_rawCreateTrampoline;
    if (!callFn) {
        spdlog::warn("entity_hooks: CallFactoryDirect — no trampoline available");
        return nullptr;
    }

    s_directSpawnBypass.store(true, std::memory_order_release);
    void* result = nullptr;
    __try {
        result = callFn(factory, requestStruct);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_crashCount = 0;
        if (++s_crashCount <= 5) {
            spdlog::error("entity_hooks: CallFactoryDirect CRASHED (SEH caught, attempt {})", s_crashCount);
        }
    }
    s_directSpawnBypass.store(false, std::memory_order_release);

    return result;
}

// ES: Llama a RootObjectFactory::create(fábrica, GameData*) con bypass y SEH. create()
//     construye punteros internos frescos y llama a process() (que pasa por el hook).
// EN: Calls RootObjectFactory::create(factory, GameData*) with bypass and SEH. create()
//     builds fresh internal pointers and calls process() (which goes through the hook).
void* CallFactoryCreate(void* factory, void* gameData) {
    // Call RootObjectFactory::create — the high-level dispatcher that builds
    // a proper request struct from a GameData* and calls process() internally.
    // This bypasses the stale-pointer problem entirely because create()
    // constructs FRESH internal pointers (faction, squad, AI, etc.).
    //
    // NOT hooked by MinHook, so no trampoline/stub issues. The CPU naturally
    // sets RAX = RSP after CALL pushes the return address, which is what the
    // mov rax, rsp prologue expects.
    if (!s_factoryCreate) {
        spdlog::warn("entity_hooks: CallFactoryCreate — function not resolved");
        return nullptr;
    }

    s_directSpawnBypass.store(true, std::memory_order_release);
    void* result = nullptr;
    __try {
        result = s_factoryCreate(factory, gameData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_crashCount = 0;
        if (++s_crashCount <= 5) {
            spdlog::error("entity_hooks: CallFactoryCreate CRASHED (SEH caught, attempt {})", s_crashCount);
        }
    }
    s_directSpawnBypass.store(false, std::memory_order_release);
    return result;
}

// ES: Llama a createRandomChar(fábrica, nullptr) con bypass y SEH. Último recurso.
// EN: Calls createRandomChar(factory, nullptr) with bypass and SEH. Last resort.
void* CallFactoryCreateRandom(void* factory) {
    // Call RootObjectFactory::createRandomChar — creates a random NPC.
    // Takes just the factory pointer (RCX=factory, RDX=0).
    // Useful as last-resort when mod templates fail.
    if (!s_factoryCreateRandomChar) {
        spdlog::warn("entity_hooks: CallFactoryCreateRandom — function not resolved");
        return nullptr;
    }

    s_directSpawnBypass.store(true, std::memory_order_release);
    void* result = nullptr;
    __try {
        result = s_factoryCreateRandomChar(factory, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_crashCount = 0;
        if (++s_crashCount <= 5) {
            spdlog::error("entity_hooks: CallFactoryCreateRandom CRASHED (SEH caught, attempt {})", s_crashCount);
        }
    }
    s_directSpawnBypass.store(false, std::memory_order_release);
    return result;
}

// ── SEH-safe faction pointer validation ──
// ES: true si el puntero de facción sigue siendo legible y su vtable apunta dentro
//     del módulo kenshi_x64.exe.
// EN: Returns true if the pointer is still readable and has a plausible vtable.
static bool SEH_IsFactionValid(uintptr_t factionPtr) {
    if (factionPtr < 0x10000 || factionPtr > 0x00007FFFFFFFFFFF || (factionPtr & 0x7) != 0)
        return false;
    __try {
        uintptr_t vtable = 0;
        Memory::Read(factionPtr, vtable);
        uintptr_t modBase = Memory::GetModuleBase();
        size_t modSize = Core::Get().GetScanner().GetSize();
        return (vtable != 0 && vtable >= modBase && vtable < modBase + modSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Ver entity_hooks.h. Borra el valor guardado si ha caducado.
// EN: See entity_hooks.h. Clears the stored value if stale.
uintptr_t GetFallbackFaction() {
    uintptr_t fac = s_fallbackFaction.load(std::memory_order_relaxed);
    // Validate before returning — the pointer may have become stale after a load
    if (fac != 0 && !SEH_IsFactionValid(fac)) {
        s_fallbackFaction.store(0, std::memory_order_relaxed);
        return 0;
    }
    return fac;
}

// ES: Ver entity_hooks.h. Si caducó, la borra y desbloquea.
// EN: See entity_hooks.h. If stale, clears and unlocks it.
uintptr_t GetEarlyPlayerFaction() {
    uintptr_t fac = s_earlyPlayerFaction.load(std::memory_order_relaxed);
    // Validate before returning — the pointer may have become stale after a load
    if (fac != 0 && !SEH_IsFactionValid(fac)) {
        spdlog::warn("entity_hooks: GetEarlyPlayerFaction — stored faction 0x{:X} is STALE, clearing", fac);
        s_earlyPlayerFaction.store(0, std::memory_order_relaxed);
        s_earlyFactionLocked.store(false, std::memory_order_relaxed);
        return 0;
    }
    return fac;
}

// ES: Ver entity_hooks.h. Orden: facción elegida o del PlayerController válidas ->
//     nada que hacer; respaldo válido -> se promociona; si no, reescaneo de hasta 20
//     personajes con CharacterIterator (prefiere coincidencia de nombre o
//     isPlayerFaction; si no, la primera válida fuera del módulo).
// EN: See entity_hooks.h. Order: elected or PlayerController faction valid -> nothing
//     to do; valid fallback -> promoted; otherwise rescan up to 20 characters with
//     CharacterIterator (prefers name match or isPlayerFaction, else first valid
//     faction outside the module).
bool RevalidateFaction() {
    // Check if the stored faction pointers are still valid.
    // If not, re-scan the local squad (via CharacterIterator) to find a fresh one.
    // Called periodically from game_tick_hooks or on reconnect.

    uintptr_t earlyFac = s_earlyPlayerFaction.load(std::memory_order_relaxed);
    uintptr_t fallbackFac = s_fallbackFaction.load(std::memory_order_relaxed);
    uintptr_t controllerFac = Core::Get().GetPlayerController().GetLocalFactionPtr();

    // If any stored faction is still valid, no work needed
    if ((earlyFac != 0 && SEH_IsFactionValid(earlyFac)) ||
        (controllerFac != 0 && SEH_IsFactionValid(controllerFac))) {
        return true;
    }

    // If fallback is still valid, promote it
    if (fallbackFac != 0 && SEH_IsFactionValid(fallbackFac)) {
        s_earlyPlayerFaction.store(fallbackFac, std::memory_order_relaxed);
        s_earlyFactionLocked.store(true, std::memory_order_relaxed);
        const_cast<PlayerController&>(Core::Get().GetPlayerController())
            .SetLocalFactionPtr(fallbackFac);
        spdlog::info("entity_hooks: RevalidateFaction — promoted fallback 0x{:X}", fallbackFac);
        return true;
    }

    // All stored factions are stale — clear them and re-scan
    spdlog::warn("entity_hooks: RevalidateFaction — all stored factions stale, re-scanning");
    s_earlyPlayerFaction.store(0, std::memory_order_relaxed);
    s_earlyFactionLocked.store(false, std::memory_order_relaxed);
    s_fallbackFaction.store(0, std::memory_order_relaxed);

    // Re-scan via CharacterIterator.
    // NOTE: Cannot use __try here because CharacterIterator/GetName use C++ objects
    // with destructors (MSVC C2712). The game memory reads are individually SEH-safe
    // via the Memory:: helpers and the accessor methods.
    {
        const std::string& cfgName = Core::Get().GetConfig().playerName;
        uintptr_t bestFaction = 0;
        bool foundNameMatch = false;

        game::CharacterIterator iter;
        int scanned = 0;
        while (iter.HasNext() && scanned < 20) {
            game::CharacterAccessor ch = iter.Next();
            if (!ch.IsValid()) continue;
            scanned++;

            uintptr_t fPtr = ch.GetFactionPtr();
            if (fPtr < 0x10000 || fPtr > 0x00007FFFFFFFFFFF || (fPtr & 0x7) != 0) continue;
            if (!SEH_IsFactionValid(fPtr)) continue;

            // Reject module-internal pointers
            uintptr_t modBase = Memory::GetModuleBase();
            if (fPtr >= modBase && fPtr < modBase + 0x4000000) continue;

            // Prefer character whose name matches config playerName
            if (!foundNameMatch && cfgName.size() > 0) {
                std::string charName = ch.GetName();
                if (charName.size() == cfgName.size() &&
                    _strnicmp(charName.c_str(), cfgName.c_str(), cfgName.size()) == 0) {
                    bestFaction = fPtr;
                    foundNameMatch = true;
                    spdlog::info("entity_hooks: RevalidateFaction — name match '{}' -> faction 0x{:X}",
                                 charName, fPtr);
                    continue;
                }
            }

            // Also check isPlayerFaction flag
            if (!foundNameMatch && SEH_CheckIsPlayerFaction(fPtr)) {
                bestFaction = fPtr;
                foundNameMatch = true; // Treat isPlayerFaction as definitive
                spdlog::info("entity_hooks: RevalidateFaction — isPlayerFaction flag -> 0x{:X}", fPtr);
                continue;
            }

            // Take first valid faction as fallback
            if (bestFaction == 0) {
                bestFaction = fPtr;
            }
        }

        if (bestFaction != 0) {
            s_earlyPlayerFaction.store(bestFaction, std::memory_order_relaxed);
            s_earlyFactionLocked.store(true, std::memory_order_relaxed);
            UpdateFallbackFaction(bestFaction);
            const_cast<PlayerController&>(Core::Get().GetPlayerController())
                .SetLocalFactionPtr(bestFaction);
            spdlog::info("entity_hooks: RevalidateFaction — re-discovered faction 0x{:X} (nameMatch={})",
                         bestFaction, foundNameMatch);
            return true;
        }
    }

    spdlog::warn("entity_hooks: RevalidateFaction — FAILED to find any valid faction");
    return false;
}

// ES: Getters de offsets detectados y de detección de carga; ver entity_hooks.h.
// EN: Getters for detected offsets and loading detection; see entity_hooks.h.
int GetGameDataOffsetInStruct() {
    return s_gameDataOffsetInStruct;
}

int GetPositionOffsetInStruct() {
    return s_positionOffsetInStruct;
}

// ── Loading detection via create events ──

int64_t GetTimeSinceLastCreate() {
    int64_t lastMs = s_lastCreateTimeMs.load(std::memory_order_relaxed);
    if (lastMs == 0) return INT64_MAX; // Never fired
    auto now = std::chrono::steady_clock::now();
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return nowMs - lastMs;
}

int GetLoadingCreateCount() {
    return s_loadingCreateCount.load(std::memory_order_relaxed);
}

// ES: Además del contador, borra la marca de tiempo y las plantillas capturadas.
// EN: Besides the counter, clears the timestamp and captured templates.
void ResetLoadingCreateCount() {
    s_loadingCreateCount.store(0, std::memory_order_relaxed);
    s_lastCreateTimeMs.store(0, std::memory_order_relaxed);
    // Also reset mod template captures for the new loading cycle
    s_capturedModTemplateCount.store(0, std::memory_order_relaxed);
    memset(s_capturedModTemplates, 0, sizeof(s_capturedModTemplates));
}

void SetLoadingPassthrough(bool enabled) {
    s_loadingPassthrough.store(enabled, std::memory_order_release);
    if (enabled) {
        spdlog::info("entity_hooks: Loading passthrough ENABLED — lightweight create tracking active");
    } else {
        spdlog::info("entity_hooks: Loading passthrough DISABLED — full hook body active");
    }
}

int GetCapturedModTemplates(void** outPtrs, int maxCount) {
    int count = s_capturedModTemplateCount.load(std::memory_order_relaxed);
    if (count > maxCount) count = maxCount;
    if (count > MAX_MOD_TEMPLATE_CAPTURES) count = MAX_MOD_TEMPLATE_CAPTURES;
    for (int i = 0; i < count; i++) {
        outPtrs[i] = s_capturedModTemplates[i];
    }
    return count;
}

} // namespace kmp::entity_hooks
