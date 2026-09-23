// ES: Implementación del módulo de escuadras. Contiene: detours Hook_SquadCreate (enviaría
//     C2S_SquadCreate) y Hook_SquadAddMember (enviaría C2S_SquadAddMember), ambos NO
//     instalados; la validación de SquadAddMember por .pdata; y AddCharacterToLocalSquad(),
//     que localiza el activePlatoon del personaje principal del jugador (comparando vtables) y
//     llama a SquadAddMember(activePlatoon, personaje) protegido con SEH. Se llama desde el
//     hilo de lógica del mod (OnGameTick / packet handler).
// EN: Implementation of the squad module. Contains: the Hook_SquadCreate detour (would send
//     C2S_SquadCreate) and Hook_SquadAddMember (would send C2S_SquadAddMember), both NOT
//     installed; the .pdata validation of SquadAddMember; and AddCharacterToLocalSquad(),
//     which finds the activePlatoon of the player's primary character (by vtable matching)
//     and calls SquadAddMember(activePlatoon, character) under SEH. Called from the mod's
//     logic thread (OnGameTick / packet handler).
#include "squad_hooks.h"
#include "kmp/hook_manager.h"
#include "kmp/patterns.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "../core.h"
#include "../game/game_types.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <unordered_map>
#include <deque>

namespace kmp::squad_hooks {

// ES: Firmas: SquadCreate(gestor de escuadras, plantilla) -> escuadra; SquadAddMember(escuadra, personaje).
// EN: Signatures: SquadCreate(squad manager, template) -> squad; SquadAddMember(squad, character).
// ── Function typedefs ──
using SquadCreateFn    = void*(__fastcall*)(void* squadManager, void* templateData);
using SquadAddMemberFn = void(__fastcall*)(void* squad, void* character);

// ES: Estado: trampolines (nullptr, no se instalan), contadores y bandera de carga.
// EN: State: trampolines (nullptr, not installed), counters and loading flag.
// ── State ──
static SquadCreateFn    s_origSquadCreate    = nullptr;
static SquadAddMemberFn s_origSquadAddMember = nullptr;
static int s_createCount = 0;
static int s_addMemberCount = 0;
static bool s_loading = false;

// ES: Estado de seguridad del puntero crudo a SquadAddMember: si falla la validación .pdata
//     o crashea en la primera llamada se desactiva para siempre y la inyección se degrada sin
//     tumbar el juego.
// Safety state for SquadAddMember raw function pointer usage.
// If the address fails .pdata validation or crashes on first call,
// we permanently disable it so squad injection degrades gracefully
// instead of crashing the game.
static bool s_squadAddMemberValidated = false;
static bool s_squadAddMemberDisabled = false;

// ES: Mapa puntero de escuadra -> netId asignado por el servidor (se rellena con
//     S2C_SquadCreated) y cola de punteros esperando su id.
// Squad pointer → server-assigned net ID mapping.
// Populated when the server responds with S2C_SquadCreated.
static std::unordered_map<void*, uint32_t> s_squadPtrToNetId;
static std::deque<void*> s_pendingSquadPtrs; // Queued squad pointers awaiting server ID

// ── SEH wrappers ──

// ES: El envoltorio SEH NO puede usar objetos C++ (error MSVC C2712); los crashes se cuentan
//     con un contador estático y OutputDebugString.
// SEH wrapper must NOT use C++ objects (MSVC C2712).
// Crash tracking is done via static counter + OutputDebugString.
static void* SEH_SquadCreate(void* squadManager, void* templateData) {
    __try {
        return s_origSquadCreate(squadManager, templateData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_crashCount = 0;
        if (++s_crashCount <= 5) {
            char buf[128];
            sprintf_s(buf, "KMP: SEH_SquadCreate CRASHED #%d\n", s_crashCount);
            OutputDebugStringA(buf);
        }
        return nullptr;
    }
}

// ES: Llamada protegida al SquadAddMember original; false si crashea.
// EN: Protected call to the original SquadAddMember; false on a crash.
static bool SEH_SquadAddMember(void* squad, void* character) {
    __try {
        s_origSquadAddMember(squad, character);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── Hooks ──

// ES: Detour de SquadCreate (NO instalado). Llama al original; si hay conexión, encola el
//     puntero de la escuadra nueva y envía C2S_SquadCreate con el primer personaje local como
//     creador y el nombre "Squad".
// EN: SquadCreate detour (NOT installed). Calls the original; when connected, queues the new
//     squad pointer and sends C2S_SquadCreate with the first local character as creator and
//     the name "Squad".
static void* __fastcall Hook_SquadCreate(void* squadManager, void* templateData) {
    s_createCount++;

    void* result = SEH_SquadCreate(squadManager, templateData);
    if (!result) {
        spdlog::error("squad_hooks: SquadCreate crashed or returned null");
        return nullptr;
    }

    if (s_loading) return result;

    auto& core = Core::Get();
    if (!core.IsConnected()) return result;

    spdlog::info("squad_hooks: SquadCreate #{} (mgr=0x{:X}, template=0x{:X}) -> 0x{:X}",
                  s_createCount, (uintptr_t)squadManager, (uintptr_t)templateData,
                  (uintptr_t)result);

    // ES: Encolar el puntero para que OnSquadNetIdAssigned() lo asocie cuando llegue S2C_SquadCreated.
    // Queue this squad pointer so OnSquadNetIdAssigned() can map it
    // when the server responds with S2C_SquadCreated.
    s_pendingSquadPtrs.push_back(result);

    auto& registry = core.GetEntityRegistry();
    auto localEntities = registry.GetPlayerEntities(core.GetLocalPlayerId());
    EntityID creatorId = localEntities.empty() ? 0 : localEntities[0];

    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_SquadCreate);
    writer.WriteU32(creatorId);
    writer.WriteString("Squad");
    core.GetClient().SendReliable(writer.Data(), writer.Size());

    return result;
}

// ES: Detour de SquadAddMember (NO instalado). Llama al original y, si el personaje tiene
//     netId, envía C2S_SquadAddMember con el netId de la escuadra (0 si no está mapeada).
// EN: SquadAddMember detour (NOT installed). Calls the original and, if the character has a
//     netId, sends C2S_SquadAddMember with the squad netId (0 if unmapped).
static void __fastcall Hook_SquadAddMember(void* squad, void* character) {
    s_addMemberCount++;

    if (!SEH_SquadAddMember(squad, character)) {
        spdlog::error("squad_hooks: SquadAddMember crashed");
        return;
    }

    if (s_loading) return;

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    spdlog::info("squad_hooks: SquadAddMember #{} (squad=0x{:X}, char=0x{:X})",
                  s_addMemberCount, (uintptr_t)squad, (uintptr_t)character);

    auto& registry = core.GetEntityRegistry();
    EntityID memberNetId = registry.GetNetId(character);
    if (memberNetId == INVALID_ENTITY) return;

    // ES: Buscar el netId de la escuadra en nuestro mapa.
    // Look up the squad's server-assigned net ID from our mapping
    uint32_t squadNetId = 0;
    auto it = s_squadPtrToNetId.find(squad);
    if (it != s_squadPtrToNetId.end()) {
        squadNetId = it->second;
    } else {
        spdlog::warn("squad_hooks: SquadAddMember #{} - squad 0x{:X} has no net ID mapping",
                     s_addMemberCount, (uintptr_t)squad);
    }

    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_SquadAddMember);
    MsgSquadMemberUpdate msg{};
    msg.squadNetId = squadNetId;
    msg.memberEntityId = memberNetId;
    msg.action = 0; // added
    writer.WriteRaw(&msg, sizeof(msg));
    core.GetClient().SendReliable(writer.Data(), writer.Size());
}

// ── Install / Uninstall ──

// ES: No instala hooks (motivos abajo). Solo valida el puntero crudo de SquadAddMember.
// EN: Installs no hooks (reasons below). It only validates the raw SquadAddMember pointer.
bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hooks = HookManager::Get();
    (void)hooks;  // hooks kept for future use

    // ES: Hook SquadCreate DESACTIVADO: empieza por `mov rax, rsp` (48 8B C4). El trampolín
    //     parecía seguro pero provocaba crashes silenciosos al cargar zonas, cuando se crean 100+
    //     escuadras de NPC seguidas. El host no lo necesita; solo importa SquadAddMember.
    // SquadCreate hook DISABLED — starts with `mov rax, rsp` (48 8B C4).
    // The raw trampoline appeared safe in theory but caused silent crashes
    // during zone loading when 100+ NPC squads are created rapidly.
    // SquadCreate sync is not needed for host — only SquadAddMember matters
    // (for AddCharacterToLocalSquad injection).
    if (funcs.SquadCreate) {
        spdlog::info("squad_hooks: SquadCreate SKIPPED (mov rax,rsp — crash risk during zone loads)");
    }

    // ES: Hook SquadAddMember DESACTIVADO: se dispara 30-40+ veces al cargar zonas y cada
    //     llamada (búsquedas + paquetes) acumulaba corrupción hasta crashear ~10 s después. Se
    //     guarda el puntero crudo para AddCharacterToLocalSquad (llamada directa).
    // SquadAddMember hook DISABLED — fires 30-40+ times during zone loading
    // when NPC squads are assembled. Each call through the trampoline does entity
    // lookups + packet writes, causing cumulative corruption → crash ~10s later.
    // The raw function pointer is kept for AddCharacterToLocalSquad (direct call).
    if (funcs.SquadAddMember) {
        uintptr_t addMemberAddr = reinterpret_cast<uintptr_t>(funcs.SquadAddMember);

        // ES: Seguridad: comprobar con .pdata (RtlLookupFunctionEntry) que la dirección es el inicio
        //     real de una función. 0x928423 NO está alineada a 16 bytes y podría ser un punto a mitad
        //     de función descubierto por vtable. Si lo es, se desactiva el puntero crudo.
        // Safety: verify via .pdata that the address is a valid function entry point.
        // SquadAddMember is at 0x928423 which is NOT 16-byte aligned and may be
        // a mid-function entry point discovered via vtable.
        DWORD64 imageBase = 0;
        auto* rtFunc = RtlLookupFunctionEntry(
            static_cast<DWORD64>(addMemberAddr), &imageBase, nullptr);

        if (rtFunc) {
            uintptr_t funcStart = static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
            if (funcStart != addMemberAddr) {
                spdlog::warn("squad_hooks: SquadAddMember at 0x{:X} is MID-FUNCTION "
                             "(real function at 0x{:X}, offset +0x{:X}). "
                             "Disabling raw ptr — squad member tracking unavailable.",
                             addMemberAddr, funcStart, addMemberAddr - funcStart);
                s_squadAddMemberDisabled = true;
            } else {
                spdlog::info("squad_hooks: SquadAddMember at 0x{:X} — .pdata VALIDATED as function entry. "
                             "Raw ptr kept for squad injection.",
                             addMemberAddr);
                s_squadAddMemberValidated = true;
            }
        } else {
            // ES: Sin entrada .pdata: puede ser una función hoja o código generado. Se permite con aviso.
            // No .pdata entry found — could be a leaf function or dynamically generated.
            // Allow it but warn.
            spdlog::warn("squad_hooks: SquadAddMember at 0x{:X} — no .pdata entry found. "
                         "Proceeding with caution (may be leaf function).",
                         addMemberAddr);
            s_squadAddMemberValidated = true;
        }

        if (!s_squadAddMemberDisabled) {
            spdlog::info("squad_hooks: SquadAddMember at 0x{:X} — NOT hooked (zone-load crash risk). "
                         "Raw ptr kept for squad injection.",
                         addMemberAddr);
        }
    }

    spdlog::info("squad_hooks: SquadAddMember validated={}, disabled={}", s_squadAddMemberValidated, s_squadAddMemberDisabled);
    return s_squadAddMemberValidated;
}

// ES: Quita los hooks si existieran, olvida los trampolines y vacía los mapas.
// EN: Removes the hooks if present, forgets the trampolines and clears the maps.
void Uninstall() {
    auto& hooks = HookManager::Get();
    if (s_origSquadCreate)    hooks.Remove("SquadCreate");
    if (s_origSquadAddMember) hooks.Remove("SquadAddMember");
    s_origSquadCreate = nullptr;
    s_origSquadAddMember = nullptr;
    s_squadPtrToNetId.clear();
    s_pendingSquadPtrs.clear();
}

// ES: Asocia el puntero pendiente más antiguo (FIFO) con el netId recibido.
// EN: Maps the oldest pending pointer (FIFO) to the received netId.
void OnSquadNetIdAssigned(uint32_t squadNetId) {
    if (s_pendingSquadPtrs.empty()) {
        spdlog::warn("squad_hooks: OnSquadNetIdAssigned({}) but no pending squad pointers", squadNetId);
        return;
    }
    void* squadPtr = s_pendingSquadPtrs.front();
    s_pendingSquadPtrs.pop_front();
    s_squadPtrToNetId[squadPtr] = squadNetId;
    spdlog::info("squad_hooks: Mapped squad 0x{:X} -> netId={}", (uintptr_t)squadPtr, squadNetId);
}

// ES: Activa/desactiva la supresión de envíos durante la carga.
// EN: Enables/disables send suppression during loading.
void SetLoading(bool loading) {
    s_loading = loading;
}

// ES: Inyección en escuadra (aprovechando el motor): añade un personaje remoto a la escuadra
//     del jugador local llamando directamente a SquadAddMember, para que aparezca en el panel,
//     se pueda seleccionar con clic y obedezca órdenes de grupo como si se hubiera reclutado.
// ── Squad Injection (Engine Exploit) ──
// Adds a remote character to the local player's squad by calling the engine's
// own SquadAddMember function directly. This exploits the game's squad system
// to make the character appear in the squad panel, be selectable via click,
// and respond to group orders — as if it was naturally recruited.

// ES: Llamada protegida a addFn(escuadra, personaje); false si da una violación de acceso.
// EN: Protected call to addFn(squad, character); false on an access violation.
static bool SEH_InjectIntoSquad(SquadAddMemberFn addFn, void* squad, void* character) {
    __try {
        addFn(squad, character);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("squad_hooks: SEH_InjectIntoSquad crashed (squad=0x{:X}, char=0x{:X})",
                       (uintptr_t)squad, (uintptr_t)character);
        return false;
    }
}

// ES: Lectura de puntero protegida con SEH (0 si falla) para seguir cadenas de punteros.
// SEH-protected pointer read for following chains
static uintptr_t SEH_ReadPtr(uintptr_t addr) {
    __try {
        return *reinterpret_cast<uintptr_t*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ES: Intenta resolver el activePlatoon a partir del personaje principal. addMember espera
//     activePlatoon como 'this', no platoon. Datos de investigación (GOG): CharacterHuman+0x658
//     = activePlatoon*, platoon+0x1D8 = activePlatoon*. En Steam los offsets cambian, así que
//     se identifica el activePlatoon comparando su vtable con la conocida. Devuelve 0 si no lo halla.
// Try to resolve the activePlatoon from the primary character.
// The addMember function expects activePlatoon as 'this', not platoon.
// Research data (GOG): CharacterHuman+0x658 = activePlatoon*
//                       platoon+0x1D8 = activePlatoon*
// Steam offsets differ — we identify the activePlatoon by matching its vtable
// against the known vtable address from RTTI discovery.
static uintptr_t ResolveActivePlatoon(void* primaryChar) {
    uintptr_t charAddr = reinterpret_cast<uintptr_t>(primaryChar);
    uintptr_t moduleBase = Core::Get().GetScanner().GetBase();
    size_t moduleSize = Core::Get().GetScanner().GetSize();

    spdlog::info("squad_hooks: ResolveActivePlatoon: char=0x{:X}", charAddr);

    // ES: Obtener la vtable conocida de ActivePlatoon: el orquestador encontró SquadAddMember en
    //     el slot 2 (+0x10), así que se busca en el módulo un qword igual a esa dirección y la
    //     vtable empieza 0x10 bytes antes. OJO: el bucle recorre TODO el módulo (no solo .rdata)
    //     y se queda con la primera coincidencia.
    // Get the known ActivePlatoon vtable from RTTI discovery.
    // The orchestrator found SquadAddMember at vtable slot 2 (offset +0x10).
    // We can find the vtable base by scanning .rdata for a pointer table
    // that contains SquadAddMember at offset +0x10.
    auto& funcs = Core::Get().GetGameFunctions();
    uintptr_t knownVTable = 0;
    if (funcs.SquadAddMember) {
        uintptr_t addMemberAddr = reinterpret_cast<uintptr_t>(funcs.SquadAddMember);
        // The vtable is in .rdata (inside the module). Scan for a qword in .rdata
        // that equals addMemberAddr — the vtable base is 0x10 bytes before it.
        for (uintptr_t scan = moduleBase; scan < moduleBase + moduleSize - 0x20; scan += 8) {
            uintptr_t val = SEH_ReadPtr(scan);
            if (val == addMemberAddr) {
                // This is vtable slot 2 (offset +0x10), so vtable base = scan - 0x10
                knownVTable = scan - 0x10;
                spdlog::info("squad_hooks: Found ActivePlatoon vtable at 0x{:X} (slot2 at 0x{:X} = 0x{:X})",
                             knownVTable, scan, addMemberAddr);
                break;
            }
        }
    }

    // ES: Validar un candidato: su vtable debe coincidir con la conocida o, como respaldo,
    //     caer dentro del módulo del juego (respaldo permisivo). Mínimo 16 MB para descartar
    //     datos de cadenas SSO como "one" (0x656E6F) que pasaban el antiguo filtro de 0x10000 y
    //     llenaban el log del VEH de violaciones de acceso.
    // Validate candidate: check if object's vtable matches known ActivePlatoon vtable
    // Minimum 16MB: filters out SSO string data like "one" (0x656E6F) that passes
    // the old 0x10000 check and crashes SEH_ReadPtr + fills VEH log with AV noise.
    auto validateAP = [moduleBase, moduleSize, knownVTable](uintptr_t candidate, const char* source) -> bool {
        if (candidate < 0x1000000 || candidate > 0x00007FFFFFFFFFFF) return false;
        uintptr_t vtable = SEH_ReadPtr(candidate);
        if (vtable == 0) return false;

        // Primary check: exact vtable match against RTTI-discovered vtable
        if (knownVTable != 0 && vtable == knownVTable) {
            spdlog::info("squad_hooks: activePlatoon MATCHED via {} — 0x{:X} vtable=0x{:X} (RTTI match!)",
                         source, candidate, vtable);
            return true;
        }
        // Fallback: vtable in game module (works for GOG and some Steam configs)
        if (vtable >= moduleBase && vtable < moduleBase + moduleSize) {
            spdlog::info("squad_hooks: activePlatoon candidate via {} — 0x{:X} vtable=0x{:X} (in module)",
                         source, candidate, vtable);
            return true;
        }
        return false;
    };

    // ES: Recorrer el struct del personaje de 0x600 a 0x780 (cubre el 0x658 de GOG y variantes de Steam).
    // Scan character struct from 0x600 to 0x780 (covers GOG 0x658 + Steam variants)
    for (int off = 0x600; off <= 0x780; off += 8) {
        uintptr_t ap = SEH_ReadPtr(charAddr + off);
        if (validateAP(ap, ([off]{ char b[16]; sprintf_s(b, "char+0x%X", off); return std::string(b); })().c_str())) {
            if (off != 0x658) {
                spdlog::info("squad_hooks: DISCOVERED activePlatoon at char+0x{:X} (differs from GOG 0x658)", off);
            }
            return ap;
        }
    }

    // ES: Segundo intento: puntero de escuadra del personaje -> offsets 0x1B0..0x220 de la
    //     platoon, y por último el propio puntero de escuadra.
    // Try via GetSquadPtr → platoon chain
    game::CharacterAccessor accessor(primaryChar);
    uintptr_t squadPtr = accessor.GetSquadPtr();
    if (squadPtr != 0) {
        spdlog::info("squad_hooks: GetSquadPtr=0x{:X}, trying platoon chains", squadPtr);
        for (int off = 0x1B0; off <= 0x220; off += 8) {
            uintptr_t ap = SEH_ReadPtr(squadPtr + off);
            if (validateAP(ap, ([off]{ char b[16]; sprintf_s(b, "squad+0x%X", off); return std::string(b); })().c_str())) {
                return ap;
            }
        }
        if (validateAP(squadPtr, "squadPtr direct")) return squadPtr;
    }

    // ES: Volcado de diagnóstico de char+0x600..0x780 si no se encontró nada.
    // Diagnostic dump
    spdlog::warn("squad_hooks: activePlatoon NOT FOUND (knownVTable=0x{:X}) — dumping char+0x600..0x780:",
                 knownVTable);
    for (int off = 0x600; off <= 0x780; off += 8) {
        uintptr_t val = SEH_ReadPtr(charAddr + off);
        if (val > 0x1000000 && val < 0x00007FFFFFFFFFFF) {
            uintptr_t vtable = SEH_ReadPtr(val);
            spdlog::warn("  char+0x{:03X} = 0x{:X} vtable=0x{:X} {}{}",
                         off, val, vtable,
                         (vtable >= moduleBase && vtable < moduleBase + moduleSize) ? "IN_MODULE " : "",
                         (knownVTable != 0 && vtable == knownVTable) ? "** RTTI MATCH **" : "");
        }
    }

    return 0;
}

// ES: Ver .h. Pasos: comprobar que no está desactivado, elegir la función (trampolín si
//     existe, si no el puntero crudo), obtener el personaje principal del jugador local,
//     resolver su activePlatoon e inyectar con SEH. Si da violación de acceso, desactiva la
//     inyección el resto de la sesión.
// EN: See .h. Steps: check it is not disabled, pick the function (trampoline if any, else the
//     raw pointer), get the local player's primary character, resolve its activePlatoon and
//     inject under SEH. On an access violation, injection is disabled for the rest of the session.
bool AddCharacterToLocalSquad(void* character) {
    if (!character) {
        spdlog::warn("squad_hooks: AddCharacterToLocalSquad — null character");
        return false;
    }

    // ES: ¿Está desactivado SquadAddMember (falló la validación .pdata o crasheó antes)?
    // Check if SquadAddMember was disabled due to .pdata validation failure or AV on first call
    if (s_squadAddMemberDisabled) {
        spdlog::warn("squad_hooks: AddCharacterToLocalSquad — SquadAddMember disabled "
                      "(failed validation or AV on first call). Squad member tracking unavailable.");
        return false;
    }

    // ES: Elegir la función: preferir el trampolín del hook (evita reenvíos recursivos de
    //     C2S_SquadAddMember) y si no, el puntero crudo del escáner/vtable.
    // Resolve the SquadAddMember function — prefer the hook trampoline (bypasses
    // our hook to avoid recursive C2S_SquadAddMember sends), fall back to the
    // raw game function pointer from the scanner/vtable discovery.
    SquadAddMemberFn addFn = s_origSquadAddMember;
    if (!addFn) {
        auto& funcs = Core::Get().GetGameFunctions();
        addFn = reinterpret_cast<SquadAddMemberFn>(funcs.SquadAddMember);
    }
    if (!addFn) {
        spdlog::warn("squad_hooks: AddCharacterToLocalSquad — no SquadAddMember function "
                      "(hook not installed, scanner didn't find it, vtable discovery pending)");
        return false;
    }

    auto& core = Core::Get();

    // ES: Personaje principal del jugador local, para encontrar su escuadra.
    // Get the local player's primary character to find their squad
    void* primaryChar = core.GetPlayerController().GetPrimaryCharacter();
    if (!primaryChar) {
        spdlog::warn("squad_hooks: AddCharacterToLocalSquad — no primary character found");
        return false;
    }

    // ES: Resolver el activePlatoon (addMember trabaja sobre activePlatoon, no sobre platoon);
    //     según la investigación con CT: activePlatoon vtable[2] = addMember(this, personaje).
    // Resolve the activePlatoon — the addMember function operates on activePlatoon, not platoon.
    // CT research: activePlatoon vtable[2] = addMember(this=activePlatoon, character)
    uintptr_t activePlatoonPtr = ResolveActivePlatoon(primaryChar);
    if (activePlatoonPtr == 0) {
        spdlog::warn("squad_hooks: AddCharacterToLocalSquad — could not resolve activePlatoon");
        return false;
    }

    // ES: Inyectar el personaje remoto en la escuadra del jugador local.
    // Inject the remote character into the local player's squad
    void* squad = reinterpret_cast<void*>(activePlatoonPtr);
    bool ok = SEH_InjectIntoSquad(addFn, squad, character);

    if (ok) {
        spdlog::info("squad_hooks: SQUAD INJECTION SUCCESS — char 0x{:X} added to activePlatoon 0x{:X} "
                     "(fn=0x{:X}, via {})",
                     (uintptr_t)character, activePlatoonPtr, (uintptr_t)addFn,
                     (addFn == s_origSquadAddMember) ? "hook trampoline" : "raw game function");
    } else {
        spdlog::error("squad_hooks: SQUAD INJECTION FAILED (AV) — char 0x{:X}, activePlatoon 0x{:X}, fn=0x{:X}. "
                       "Disabling SquadAddMember — squad member tracking unavailable for this session.",
                       (uintptr_t)character, activePlatoonPtr, (uintptr_t)addFn);
        s_squadAddMemberDisabled = true;
    }

    return ok;
}

} // namespace kmp::squad_hooks
