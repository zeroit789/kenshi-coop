// ES: orchestrator.cpp — implementación del PatternOrchestrator (camino A, ver
//     orchestrator.h y docs/architecture/03-scanner.md): registro de todas las entradas
//     de Kenshi (patrón AOB, string ancla, RVA fija de referencia, slot de vtable),
//     métodos de resolución y el pipeline de 8 fases que escribe las direcciones en
//     GameFunctions.
// EN: orchestrator.cpp — PatternOrchestrator implementation (path A, see orchestrator.h
//     and docs/architecture/03-scanner.md): registry of every Kenshi entry (AOB pattern,
//     anchor string, reference fixed RVA, vtable slot), resolution methods and the
//     8-phase pipeline that writes the addresses into GameFunctions.
#include "kmp/orchestrator.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <algorithm>
#include <cctype>

namespace kmp {

// ES: Nombres de los métodos de resolución.
// ═══════════════════════════════════════════════════════════════════════════
//  RESOLUTION METHOD NAMES
// ═══════════════════════════════════════════════════════════════════════════

// ES: Texto corto de cada método (para logs e informes).
// EN: Short text for each method (for logs and reports).
const char* ResolutionMethodName(ResolutionMethod method) {
    switch (method) {
        case ResolutionMethod::None:            return "None";
        case ResolutionMethod::PatternScan:     return "PatternScan";
        case ResolutionMethod::StringXref:      return "StringXref";
        case ResolutionMethod::VTableSlot:      return "VTableSlot";
        case ResolutionMethod::CallGraphTrace:  return "CallGraph";
        case ResolutionMethod::HardcodedOffset: return "Hardcoded";
        case ResolutionMethod::PDataLookup:     return "PData";
        case ResolutionMethod::ComplexPattern:  return "Complex";
        case ResolutionMethod::Manual:          return "Manual";
        default: return "Unknown";
    }
}

// ES: Inicialización.
// ═══════════════════════════════════════════════════════════════════════════
//  INITIALIZATION
// ═══════════════════════════════════════════════════════════════════════════

// ES: Inicializa el motor de escaneo del módulo y los analizadores (.pdata, strings,
//     vtables, grafo). No ejecuta ninguna fase todavía.
// EN: Initializes the module scan engine and the analyzers (.pdata, strings, vtables,
//     graph). Does not run any phase yet.
bool PatternOrchestrator::Init(const char* moduleName, const OrchestratorConfig& config) {
    m_config = config;

    if (!m_scanner.Init(moduleName)) {
        spdlog::error("Orchestrator: Failed to initialize scanner engine");
        return false;
    }

    uintptr_t base = m_scanner.GetBase();
    size_t size = m_scanner.GetSize();

    m_pdata.Init(base, size);
    m_strings.Init(base, size, &m_pdata);
    m_vtables.Init(base, size, &m_pdata);
    m_callGraph.Init(base, size, &m_pdata);

    m_initialized = true;
    spdlog::info("Orchestrator: Initialized for module at 0x{:X}, size 0x{:X}", base, size);
    return true;
}

// ES: Registro de patrones.
// ═══════════════════════════════════════════════════════════════════════════
//  PATTERN REGISTRY
// ═══════════════════════════════════════════════════════════════════════════

// ES: Añade una entrada; si ya existe una con el mismo id, la sustituye.
// EN: Adds an entry; if one with the same id exists, it is replaced.
void PatternOrchestrator::Register(PatternEntry entry) {
    // Check for duplicate
    auto it = m_entryIndex.find(entry.id);
    if (it != m_entryIndex.end()) {
        // Update existing
        m_entries[it->second] = std::move(entry);
        return;
    }

    m_entryIndex[entry.id] = m_entries.size();
    m_entries.push_back(std::move(entry));
}

// ES: Quita una entrada y reconstruye el índice id -> posición.
// EN: Removes an entry and rebuilds the id -> index map.
void PatternOrchestrator::Unregister(const std::string& id) {
    auto it = m_entryIndex.find(id);
    if (it == m_entryIndex.end()) return;

    size_t idx = it->second;
    m_entries.erase(m_entries.begin() + idx);
    m_entryIndex.erase(it);

    // Rebuild index
    m_entryIndex.clear();
    for (size_t i = 0; i < m_entries.size(); i++) {
        m_entryIndex[m_entries[i].id] = i;
    }
}

// ES: Consultas de entradas por id (const y mutable) y por categoría.
// EN: Entry lookups by id (const and mutable) and by category.
const PatternEntry* PatternOrchestrator::GetEntry(const std::string& id) const {
    auto it = m_entryIndex.find(id);
    return it != m_entryIndex.end() ? &m_entries[it->second] : nullptr;
}

PatternEntry* PatternOrchestrator::GetMutableEntry(const std::string& id) {
    auto it = m_entryIndex.find(id);
    return it != m_entryIndex.end() ? &m_entries[it->second] : nullptr;
}

std::vector<const PatternEntry*> PatternOrchestrator::GetByCategory(
    const std::string& category) const {
    std::vector<const PatternEntry*> result;
    for (const auto& entry : m_entries) {
        if (entry.category == category) result.push_back(&entry);
    }
    return result;
}

// ES: Registro de todas las entradas de Kenshi.
// ═══════════════════════════════════════════════════════════════════════════
//  REGISTER BUILT-IN PATTERNS
// ═══════════════════════════════════════════════════════════════════════════

// ES: Registra todas las funciones y globales de Kenshi que usa el mod, enlazando cada
//     entrada al campo de GameFunctions donde se escribirá la dirección. Para cada una:
//     id, categoría, descripción, patrón AOB (patterns.h), string ancla + longitud, RVA
//     de referencia (v1.0.68) y si es crítica (si no se resuelve, su hook no se instala).
//     Ojo: algunas longitudes de ancla no coinciden con patterns.cpp (CharacterDestroy 31
//     vs 32, CharacterDeath 28 vs 29, BuildingPlace 45 vs 44). Con 31/28 solo se recorta el
//     texto, pero con 45 el std::string incluye el '\0' final y probablemente la búsqueda
//     por subcadena de BuildingPlace nunca casa (sin verificar en runtime).
// EN: Registers every Kenshi function and global the mod uses, binding each entry to the
//     GameFunctions field that will receive the address. For each one: id, category,
//     description, AOB pattern (patterns.h), anchor string + length, reference RVA
//     (v1.0.68) and whether it is critical (if unresolved, its hook is not installed).
//     Note: some anchor lengths differ from patterns.cpp (CharacterDestroy 31 vs 32,
//     CharacterDeath 28 vs 29, BuildingPlace 45 vs 44). 31/28 just trim the text, but with
//     45 the std::string includes the trailing '\0' and the BuildingPlace substring search
//     probably never matches (not verified at runtime).
void PatternOrchestrator::RegisterBuiltinPatterns(GameFunctions& funcs) {
    // ES: Helper para registrar una función (escribe en un void* de GameFunctions).
    // EN: Helper to register a function (writes into a GameFunctions void*).
    auto reg = [&](const char* id, const char* cat, const char* desc,
                   const char* pat, const char* str, int strLen,
                   uint32_t rva, void** target, bool crit = false) {
        PatternEntry e;
        e.id = id;
        e.category = cat;
        e.description = desc;
        e.pattern = pat;
        e.stringAnchor = str;
        e.stringAnchorLen = strLen;
        e.hardcodedRVA = rva;
        e.targetPtr = target;
        e.critical = crit;
        Register(std::move(e));
    };

    // ES: Helper para registrar un puntero global (escribe en un uintptr_t de GameFunctions).
    //     Nota: no se usa; los globales se registran a mano más abajo.
    // EN: Helper to register a global pointer (writes into a GameFunctions uintptr_t).
    //     Note: unused; the globals are registered by hand further below.
    auto regGlobal = [&](const char* id, const char* cat, const char* desc,
                         uint32_t rva, uintptr_t* target) {
        PatternEntry e;
        e.id = id;
        e.category = cat;
        e.description = desc;
        e.hardcodedRVA = rva;
        e.isGlobalPointer = true;
        e.targetUintptr = target;
        Register(std::move(e));
    };

    // ES: Ciclo de vida de entidades (CharacterSpawn/Destroy son CRÍTICAS para el multijugador).
    // ── Entity Lifecycle ── (CharacterSpawn/Destroy are CRITICAL for multiplayer)
    reg("CharacterSpawn", "entity", "RootObjectFactory::process",
        patterns::CHARACTER_SPAWN, "[RootObjectFactory::process] Character", 38,
        0x00581770, &funcs.CharacterSpawn, true);
    reg("CharacterDestroy", "entity", "NodeList destroy / character cleanup",
        patterns::CHARACTER_DESTROY, "NodeList::destroyNodesByBuilding", 31,
        0x0038A720, &funcs.CharacterDestroy, true);
    reg("CreateRandomSquad", "entity", "RootObjectFactory::createRandomSquad",
        patterns::CREATE_RANDOM_SQUAD,
        "[RootObjectFactory::createRandomSquad] Missing squad leader", 59,
        0x00583A10, &funcs.CreateRandomSquad);
    reg("CharacterSerialise", "entity", "Character save/load serialization",
        patterns::CHARACTER_SERIALISE, "[Character::serialise] Character '", 33,
        0x006280A0, &funcs.CharacterSerialise);
    reg("CharacterKO", "entity", "Character knockout handler",
        patterns::CHARACTER_KO, "knockout", 8,
        0x00345C10, &funcs.CharacterKO);

    // ES: Movimiento. CharacterMoveTo no tiene patrón (nullptr) y su ancla "pathfind" es
    //     genérica; su RVA 0x2EF4E3 no está alineada, así que ResolveEntry la rechaza.
    // ── Movement ──
    reg("CharacterSetPosition", "movement", "HavokCharacter::setPosition",
        patterns::CHARACTER_SET_POSITION,
        "HavokCharacter::setPosition moved someone off the world", 55,
        0x00145E50, &funcs.CharacterSetPosition);
    reg("CharacterMoveTo", "movement", "Pathfinding move command",
        patterns::CHARACTER_MOVE_TO, "pathfind", 8,
        0x002EF4E3, &funcs.CharacterMoveTo);

    // ES: Combate (daño, ataque, muerte, salud, modificadores, artes marciales).
    // ── Combat ──
    reg("ApplyDamage", "combat", "Attack damage effect handler",
        patterns::APPLY_DAMAGE, "Attack damage effect", 20,
        0x007A33A0, &funcs.ApplyDamage);
    reg("StartAttack", "combat", "Cut/blunt damage calculation",
        patterns::START_ATTACK, "Cutting damage", 14,
        0x007B2A20, &funcs.StartAttack);
    reg("CharacterDeath", "combat", "Death from blood loss handler",
        patterns::CHARACTER_DEATH, "{1} has died from blood loss.", 28,
        0x007A6200, &funcs.CharacterDeath);
    reg("HealthUpdate", "combat", "Health/blood system + block chance",
        patterns::HEALTH_UPDATE, "block chance", 12,
        0x0086B2B0, &funcs.HealthUpdate);
    reg("CutDamageMod", "combat", "Cut damage modifier calculation",
        patterns::CUT_DAMAGE_MOD, "cut damage mod", 14,
        0x00889CD0, &funcs.CutDamageMod);
    reg("UnarmedDamage", "combat", "Unarmed damage bonus",
        patterns::UNARMED_DAMAGE, "unarmed damage bonus", 20,
        0x000CE2D0, &funcs.UnarmedDamage);
    reg("MartialArtsCombat", "combat", "Martial arts full combat handler",
        patterns::MARTIAL_ARTS_COMBAT, "Martial Arts", 12,
        0x00892120, &funcs.MartialArtsCombat);
    // ⚠ IssueOrder NO CONFIRMADA — 0x722EF0 es UI/MyGUI (UString), NO la orden del jugador
    // (REFUTADO por RE de bytes 2026-06-18). Se mantiene el registro para que funcs.IssueOrder
    // resuelva al AOB, PERO el hook DIAG de combate que lo usaba está deshabilitado
    // (combat_hooks.cpp) para no hookear una función UI por error. La orden real entra por
    // Tasker/GOAPTaskMgr (RVA sin resolver). NO tratar como dispatcher de órdenes.
    // EN: IssueOrder NOT CONFIRMED — 0x722EF0 is UI/MyGUI (UString), NOT the player order
    //     (REFUTED by byte-level RE 2026-06-18). The registration is kept so funcs.IssueOrder
    //     resolves to the AOB, BUT the combat DIAG hook that used it is disabled
    //     (combat_hooks.cpp) so a UI function is not hooked by mistake. The real order goes
    //     through Tasker/GOAPTaskMgr (RVA unresolved). Do NOT treat it as an order dispatcher.
    reg("IssueOrder", "combat", "UI string builder (NO es IssueOrder — refutado)",
        patterns::ISSUE_ORDER, nullptr, 0,
        0x00722EF0, &funcs.IssueOrder);
    // ── Tasker::pushOrder (0x674300) — DIAG-PUSHORDER (Fase 4) ──
    // RE 2026-06-19 (byte a byte): es donde la orden de ataque ENTRA en el map del Tasker del
    // platoon (this = *(char+0x658 ActivePlatoon +0x98)). Cadena confirmada:
    //   attackTarget 0x5CB0A0 → enqueueCombatOrder 0x6744A0 (mode=4, tras 2 gates isAlly)
    //   → getTasker 0x791B10 (devuelve char+0x658) → [+0x98] → pushOrder 0x674300 → inserta nodo.
    // Prólogo limpio (48 85 D2 0F 84 ...), NO mov rax,rsp → hookeable sin el fix MovRaxRsp.
    // AOB único en .text (1 match). El hook DIAG solo registra (no muta) para confirmar en
    // runtime si la orden del HOST entra y a QUÉ Tasker.
    // EN: Tasker::pushOrder (0x674300) — DIAG-PUSHORDER (Phase 4). RE 2026-06-19 (byte level):
    //     where the attack order ENTERS the platoon Tasker map (this = *(char+0x658
    //     ActivePlatoon +0x98)). Confirmed chain: attackTarget 0x5CB0A0 -> enqueueCombatOrder
    //     0x6744A0 (mode=4, after 2 isAlly gates) -> getTasker 0x791B10 (returns char+0x658)
    //     -> [+0x98] -> pushOrder 0x674300 -> inserts node. Clean prologue (48 85 D2 0F 84 ...),
    //     NOT mov rax,rsp. Unique AOB in .text (1 match). The DIAG hook only logs (no mutation)
    //     to confirm at runtime whether the HOST order enters and into WHICH Tasker.
    //     Note: no pattern is registered here (nullptr), so it resolves via the fixed RVA
    //     validated by .pdata (TryHardcodedOffset).
    reg("PushOrder", "combat", "Tasker::pushOrder — inserta orden en el map del Tasker del platoon",
        nullptr, nullptr, 0,
        0x00674300, &funcs.PushOrder);
    // ── Character::addOrder backend "normal/replace" (0x5D1940) — DIAG-ADDORDER-BACKEND ──
    // RE 2026-07-11/12: VALIDADOR de órdenes. Si el chequeo de brazos falla devuelve true
    // (orden "manejada") SIN encolar → la orden de atacar/comer/cargar se traga en silencio
    // con globo nativo ("My arm is broken!" / "I can't carry anyone with this arm.").
    // Si pasa devuelve false y la orden sigue su camino normal (Task → cola 0x508380).
    // AOB de 32 bytes ÚNICO en .text (el prólogo corto de 18 daba 2 matches). Prólogo limpio
    // sin mov rax,rsp → hookeable sin el fix MovRaxRsp. Solo usa rcx/edx/r8 (verificado:
    // 0 usos de r9/args de pila en los 0x783 bytes de la función).
    // EN: Character::addOrder "normal/replace" backend (0x5D1940) — DIAG-ADDORDER-BACKEND.
    //     RE 2026-07-11/12: order VALIDATOR. If the arm check fails it returns true (order
    //     "handled") WITHOUT queueing, so the attack/eat/carry order is silently swallowed with
    //     the native bubble ("My arm is broken!" / "I can't carry anyone with this arm.").
    //     If it passes it returns false and the order continues (Task -> queue 0x508380).
    //     32-byte AOB UNIQUE in .text (the short 18-byte prologue gave 2 matches). Clean
    //     prologue without mov rax,rsp. Only uses rcx/edx/r8 (verified: 0 uses of r9/stack
    //     args in the function's 0x783 bytes).
    reg("AddOrderBackend", "combat",
        "Character::addOrder backend normal/replace — validador que traga órdenes (bool en al)",
        patterns::ADD_ORDER_BACKEND, nullptr, 0,
        0x005D1940, &funcs.AddOrderBackend);
    // ── Character::attackTarget (0x5CB0A0) — ya usada por el [AUTOTEST] vía RVA directa ──
    // Se registra también en GameFunctions para que el resto del mod la tenga resuelta.
    // EN: Character::attackTarget (0x5CB0A0) — already used by [AUTOTEST] via direct RVA.
    //     Also registered in GameFunctions so the rest of the mod has it resolved (fixed RVA).
    reg("AttackTarget", "combat", "Character::attackTarget — orden de ataque directa (encola mode=4)",
        nullptr, nullptr, 0,
        0x005CB0A0, &funcs.AttackTarget);
    // ── Character::setActivePlatoon (0x6213F0) — FIX-PLATOON (Fase 4) ──
    // RE 2026-06-19 (byte a byte, Steam 1.0.68): hace el REGISTRO AI<->platoon que el spawn del
    // clon del mod OMITE. Escribe [char+0x658]=platoon, carga AI=[char+0x650], y llama 0x506CC0
    // que pone [AI+0x10]=platoon->me (=[platoon+0x78]). Sin ese paso el AI tick no encuentra el
    // platoon y su Tasker (ActivePlatoon+0x98) queda huérfano (orden encolada pero NO consumida).
    // Prólogo estándar (48 89 5C 24 08 57) → hookeable/llamable sin el fix MovRaxRsp.
    // AOB único en .text (1 match): 48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 89 91 58 06 00 00
    // EN: Character::setActivePlatoon (0x6213F0) — FIX-PLATOON (Phase 4). RE 2026-06-19 (byte
    //     level, Steam 1.0.68): does the AI<->platoon REGISTRATION the mod's clone spawn SKIPS.
    //     Writes [char+0x658]=platoon, loads AI=[char+0x650] and calls 0x506CC0, which sets
    //     [AI+0x10]=platoon->me (=[platoon+0x78]). Without that the AI tick cannot find the
    //     platoon and its Tasker (ActivePlatoon+0x98) is orphaned (order queued but NOT
    //     consumed). Standard prologue (48 89 5C 24 08 57), hookable/callable without the fix.
    //     The unique AOB is written in the comment but not registered (pattern nullptr).
    reg("SetActivePlatoon", "combat", "Character::setActivePlatoon — registro AI<->platoon (FIX-PLATOON)",
        nullptr, nullptr, 0,
        0x006213F0, &funcs.SetActivePlatoon);
    // ── CombatClass::update(float) (0x60D650) — DIAG-COMBATSEED (solo diagnóstico) ──
    // Mangled ?update@CombatClass@@UEAAXM@Z: virtual, void, 1 float (dt en xmm1, this en rcx).
    // Es el AI tick del CombatClass: consume el array de percepciones (CombatClass+0x208, contador
    // +0x200) para producir currentTarget (CombatClass+0x290) y disparar Task_MeleeAttack. El hook
    // DIAG-COMBATSEED (combat_hooks.cpp) lo instrumenta SOLO para el CombatClass del HOST y loguea
    // el estado (contador de percepciones, currentTarget, AI+0x28) para localizar qué campo cambia
    // entre "host en frío" y "host tras golpear el muñeco de entrenamiento". SOLO lee/loguea, no muta.
    // Prólogo LIMPIO (40 53 48 83 EC 20 48 8B D9, sin mov rax,rsp) → hookeable sin el fix MovRaxRsp.
    // EN: CombatClass::update(float) (0x60D650) — DIAG-COMBATSEED (diagnostics only). Mangled
    //     ?update@CombatClass@@UEAAXM@Z: virtual, void, 1 float (dt in xmm1, this in rcx). It is
    //     the CombatClass AI tick: consumes the perception array (CombatClass+0x208, counter
    //     +0x200) to produce currentTarget (CombatClass+0x290) and trigger Task_MeleeAttack.
    //     The DIAG-COMBATSEED hook (combat_hooks.cpp) instruments it ONLY for the HOST
    //     CombatClass and logs the state to find which field changes between "cold host" and
    //     "host after hitting the training dummy". Read/log only. CLEAN prologue.
    reg("CombatClassUpdate", "combat", "CombatClass::update(float) — AI tick de combate (DIAG-COMBATSEED)",
        nullptr, nullptr, 0,
        0x0060D650, &funcs.CombatClassUpdate);
    // NOTA (2026-07-15): aquí había un registro "CombatTargetResolve" para la RVA 0x0074A630
    // que era un DUPLICADO de la misma dirección ya registrada como "BuyItem" (ver más abajo,
    // categoría "inventory", string xref "buyItem"). La RE posterior confirmó que 0x74A630 es
    // BuyItem (rutina de IA de COMERCIO), no un selector de objetivo de combate. El hook de esa
    // RVA (inventory_hooks.cpp: Hook_BuyItem, sync de red + guard UAF fusionados) se instala sobre
    // funcs.BuyItem — no hace falta un segundo registro de la misma RVA bajo otro nombre. Registro
    // eliminado para quitar la doble etiqueta; funcs.BuyItem sigue resolviendo 0x74A630 igual que antes.
    // EN: NOTE (2026-07-15): there used to be a "CombatTargetResolve" registration here for
    //     RVA 0x0074A630, a DUPLICATE of the same address already registered as "BuyItem"
    //     (below, "inventory" category, "buyItem" xref). Later RE confirmed 0x74A630 is BuyItem
    //     (AI TRADE routine), not a combat target selector. The hook on that RVA
    //     (inventory_hooks.cpp: Hook_BuyItem) is installed on funcs.BuyItem, so the duplicate
    //     registration was removed; funcs.BuyItem still resolves 0x74A630 as before.

    // ES: Mundo / zonas. Navmesh no tiene ancla (solo patrón o RVA).
    // ── World / Zones ──
    reg("ZoneLoad", "world", "Zone loading",
        patterns::ZONE_LOAD, "zone.%d.%d.zone", 15,
        0x00377710, &funcs.ZoneLoad);
    reg("ZoneUnload", "world", "Zone unload / navmesh teardown",
        patterns::ZONE_UNLOAD, "destroyed navmesh", 17,
        0x002EF1F0, &funcs.ZoneUnload);
    reg("BuildingPlace", "world", "Building placement / creation",
        patterns::BUILDING_PLACE,
        "[RootObjectFactory::createBuilding] Building", 45,
        0x0057CC70, &funcs.BuildingPlace);
    reg("BuildingDestroyed", "world", "Building destroyed handler",
        patterns::BUILDING_DESTROYED, "Building::setDestroyed", 22,
        0x00557280, &funcs.BuildingDestroyed);
    reg("Navmesh", "world", "Navmesh system",
        patterns::NAVMESH, nullptr, 0,
        0x00881950, &funcs.Navmesh);
    reg("SpawnCheck", "world", "Spawn collision check",
        patterns::SPAWN_CHECK, " tried to spawn inside walls!", 29,
        0x004FFAD0, &funcs.SpawnCheck);

    // ES: Bypass de spawn de escuadrones (sacado de la RE de un mod de investigación): el
    //     pipeline de spawn comprueba si debe generar escuadrones cerca del jugador; hookearlo
    //     permitiría inyectar personajes de jugadores remotos por el pipeline natural del juego.
    //     OJO: es una dirección a mitad de función (0x4FF47C, no alineada), así que
    //     ResolveEntry la rechaza por patrón/RVA; y el patrón en línea tiene un "FF" de más
    //     respecto a SPAWN_CHECK ("... 30 FF FF FF FF 48 81 EC"), probablemente una errata.
    // ── Squad Spawn Bypass (from research mod RE) ──
    // The squad spawning pipeline checks whether to spawn squads near the player.
    // Hooking this allows injecting remote player characters through the game's
    // natural spawn pipeline — fully initialized with faction, AI, squad, animations.
    reg("SquadSpawnBypass", "entity", "Squad spawn check bypass",
        "48 8D AC 24 30 FF FF FF FF 48 81 EC D0 01 00 00",
        " tried to spawn inside walls!", 29,
        0x004FF47C, &funcs.SquadSpawnBypass);

    // ES: Actualización de animación de personaje: se ejecuta para CADA personaje en cada
    //     frame; el mod de investigación la usa para seguir a todos los personajes por nombre.
    //     Patrón de GOG: mov rcx,[rbx+320]; mov [rbx+37C],sil. También es una dirección a
    //     mitad de función (0x65F6C7, no alineada), así que el filtro de alineación de
    //     ResolveEntry la rechaza por patrón/RVA.
    // Character animation update — fires for EVERY character each frame.
    // Research mod uses this to track all characters by name in real time.
    // Pattern from GOG: mov rcx,[rbx+320]; mov [rbx+37C],sil
    reg("CharAnimUpdate", "entity", "Character animation update tick",
        "48 8B 8B 20 03 00 00 40 88 B3 7C 03 00 00",
        nullptr, 0,
        0x0065F6C7, &funcs.CharAnimUpdate);

    // ES: Bucle de juego / tiempo (CRÍTICAS para el tick del multijugador).
    // ── Game Loop / Time ── (CRITICAL for multiplayer tick)
    reg("GameFrameUpdate", "core", "Main game frame tick",
        patterns::GAME_FRAME_UPDATE, "Kenshi 1.0.", 11,
        0x00123A10, &funcs.GameFrameUpdate, true);
    reg("TimeUpdate", "core", "Time scale handler",
        patterns::TIME_UPDATE, "timeScale", 9,
        0x00214B50, &funcs.TimeUpdate, true);

    // ES: Guardar / cargar / estadísticas.
    // ── Save / Load ──
    reg("SaveGame", "save", "Save game function",
        patterns::SAVE_GAME, "quicksave", 9,
        0x007EF040, &funcs.SaveGame);
    reg("LoadGame", "save", "Load game function",
        patterns::LOAD_GAME, "[SaveManager::loadGame] No towns loaded.", 40,
        0x00373F00, &funcs.LoadGame);
    reg("ImportGame", "save", "Import game (new from template)",
        patterns::IMPORT_GAME, "[SaveManager::importGame] No towns loaded.", 42,
        0x00378A30, &funcs.ImportGame);
    reg("CharacterStats", "ui", "Character stats UI",
        patterns::CHARACTER_STATS, "CharacterStats_Attributes", 25,
        0x008BA700, &funcs.CharacterStats);

    // ES: Escuadrón / platoon.
    // ── Squad / Platoon ──
    reg("SquadCreate", "squad", "Squad position reset / management",
        patterns::SQUAD_CREATE, "Reset squad positions", 21,
        0x00480B50, &funcs.SquadCreate);

    // ES: SquadAddMember se resuelve por RTTI: función del slot 2 (+0x10) de la vtable de
    //     ActivePlatoon. El ancla "delayedSpawningChecks" se quitó porque en Steam cae en otra
    //     función. vtableClass admite varios candidatos separados por "|". La RVA 0x928423 no
    //     está alineada, por lo que como RVA fija se rechazaría.
    // SquadAddMember: resolved via RTTI vtable scan.
    // The addMember function is at vtable slot 2 (offset +0x10) of the ActivePlatoon class.
    // String anchor "delayedSpawningChecks" is REMOVED — finds wrong function on Steam.
    // vtableClass uses "|"-separated candidates for fuzzy RTTI name matching.
    {
        PatternEntry e;
        e.id = "SquadAddMember";
        e.category = "squad";
        e.description = "ActivePlatoon::addMember (vtable slot 2)";
        e.pattern = patterns::SQUAD_ADD_MEMBER;
        e.stringAnchor = nullptr;
        e.stringAnchorLen = 0;
        e.hardcodedRVA = 0x00928423;
        e.targetPtr = &funcs.SquadAddMember;
        e.vtableClass = "ActivePlatoon|Platoon|Squad";
        e.vtableSlot = 2;
        Register(std::move(e));
    }

    // ES: Inventario / objetos.
    // ── Inventory / Items ──
    reg("ItemPickup", "inventory", "Inventory addItem",
        patterns::ITEM_PICKUP, "addItem", 7,
        0x0074C8B0, &funcs.ItemPickup);
    reg("ItemDrop", "inventory", "Inventory removeItem",
        patterns::ITEM_DROP, "removeItem", 10,
        0x00745DE0, &funcs.ItemDrop);
    reg("BuyItem", "inventory", "Shop purchase",
        patterns::BUY_ITEM, "buyItem", 7,
        0x0074A630, &funcs.BuyItem);

    // ES: Facción / diplomacia.
    // ── Faction / Diplomacy ──
    reg("FactionRelation", "faction", "Faction relation handler",
        patterns::FACTION_RELATION, "faction relation", 16,
        0x00872E00, &funcs.FactionRelation);

    // ES: Sistema de IA (AICreate es CRÍTICA: hace falta para suprimir la IA de los remotos).
    // ── AI System ── (AICreate is CRITICAL — needed to suppress remote AI)
    reg("AICreate", "ai", "AI controller creation",
        patterns::AI_CREATE, "[AI::create] No faction for", 27,
        0x00622110, &funcs.AICreate, true);
    reg("AIPackages", "ai", "AI behavior package loader",
        patterns::AI_PACKAGES, "AI packages", 11,
        0x00271620, &funcs.AIPackages);

    // ES: Torretas / distancia.
    // ── Turret / Ranged ──
    reg("GunTurret", "combat", "Turret operation handler",
        patterns::GUN_TURRET, "gun turret", 10,
        0x0043B690, &funcs.GunTurret);
    reg("GunTurretFire", "combat", "Turret targeting/fire handler",
        patterns::GUN_TURRET_FIRE, nullptr, 0,
        0x0043CDB0, &funcs.GunTurretFire);

    // ES: Gestión de edificios.
    // ── Building Management ──
    reg("BuildingDismantle", "building", "Building dismantle handler",
        patterns::BUILDING_DISMANTLE, "dismantle", 9,
        0x002A2860, &funcs.BuildingDismantle);
    reg("BuildingConstruct", "building", "Building construction progress",
        patterns::BUILDING_CONSTRUCT, "construction progress", 21,
        0x005547F0, &funcs.BuildingConstruct);
    reg("BuildingRepair", "building", "Building repair handler",
        patterns::BUILDING_REPAIR, nullptr, 0,
        0x00555650, &funcs.BuildingRepair);

    // ES: Punteros globales (PlayerBase es CRÍTICO para recorrer entidades). Tienen string
    //     ancla para descubrirlos con FindGlobalNearString en GOG y Steam; las RVAs fijas son
    //     respaldo. Posible problema (según el código, sin verificar en runtime): la fase 5
    //     ejecuta TryStringXref también sobre globales, y como "CharacterStats_Attributes"
    //     existe, PlayerBase podría quedar resuelto con la dirección de la FUNCIÓN que usa el
    //     string (el filtro de alineación no se aplica a globales). RetryGlobalDiscovery lo
    //     corregiría después al validar el valor.
    // EN: Global pointers (PlayerBase is CRITICAL for entity iteration). They have anchor
    //     strings to discover them via FindGlobalNearString on GOG and Steam; fixed RVAs are a
    //     fallback. Possible issue (from reading the code, not verified at runtime): phase 5
    //     runs TryStringXref on globals too, and since "CharacterStats_Attributes" exists,
    //     PlayerBase could be resolved to the address of the FUNCTION using the string (the
    //     alignment filter does not apply to globals). RetryGlobalDiscovery would fix it later
    //     when validating the value.
    // ── Global Pointers ── (PlayerBase is CRITICAL for entity iteration)
    // NOTE: These have string anchors so the orchestrator can discover them
    // via FindGlobalNearString on BOTH GOG and Steam versions.
    // The hardcoded RVAs are GOG-only fallbacks.
    {
        PatternEntry e;
        e.id = "PlayerBase"; e.category = "global"; e.description = "Player data base pointer";
        e.hardcodedRVA = 0x01AC8A90; e.isGlobalPointer = true; e.critical = true;
        e.targetUintptr = &funcs.PlayerBase;
        // PlayerBase is loaded by functions referencing these unique debug strings.
        // The global is accessed via MOV/LEA reg,[RIP+disp32] in the same function.
        // Multiple anchors for robustness (first match wins).
        e.stringAnchor = "CharacterStats_Attributes";
        e.stringAnchorLen = 25;
        Register(std::move(e));
    }
    {
        PatternEntry e;
        e.id = "GameWorldSingleton"; e.category = "global";
        e.description = "GameWorld embedded instance (NO puntero)";
        // RVA de la INSTANCIA estatica de GameWorld embebida en .data para Kenshi Steam 1.0.68.
        // Verificado por RTTI + ctor/dtor + 513 xrefs del binario: en 1.0.68 GameWorld NO es un
        // puntero global (GameWorld* ou), es la INSTANCIA misma. Por tanto base+0x2134110 ES
        // directamente el objeto GameWorld (su primer qword es la vtable en .text 0x1722608),
        // NO hay que dereferenciar.
        // Historial de RVAs: 0x02133040 (erroneo) -> 0x02131020 (era de 1.0.65, daba NULL) ->
        // 0x02134110 (CORRECTO 1.0.68, verificado RTTI/xref).
        // EN: RVA of the static GameWorld INSTANCE embedded in .data for Kenshi Steam 1.0.68.
        //     Verified via RTTI + ctor/dtor + 513 binary xrefs: on 1.0.68 GameWorld is NOT a global
        //     pointer (GameWorld* ou), it is the INSTANCE itself. So base+0x2134110 IS the GameWorld
        //     object (its first qword is the .text vtable 0x1722608), no dereference needed.
        //     RVA history: 0x02133040 (wrong) -> 0x02131020 (1.0.65 era, gave NULL) -> 0x02134110
        //     (CORRECT 1.0.68, verified RTTI/xref).
        e.hardcodedRVA = 0x02134110; e.isGlobalPointer = true; e.critical = true;
        e.targetUintptr = &funcs.GameWorldSingleton;
        // ES: Ancla "dayTime": según docs/03-scanner.md NO existe en la v1.0.68, así que el
        //     descubrimiento por string de GameWorld no funciona en esa versión.
        // GameWorld is loaded by functions referencing time/speed/zone strings.
        e.stringAnchor = "dayTime";
        e.stringAnchorLen = 7;
        Register(std::move(e));
    }

    // ES: Patrones ampliados: entradas "aspiracionales" de otros sistemas del juego registradas
    //     solo con un string ancla (sin patrón, sin RVA y sin destino en GameFunctions). Con
    //     anclas tan genéricas ("heal", "camp", "price"...) no resuelven nada útil hoy; son
    //     ganchos para RE futuro y solo aparecen en el informe.
    // EN: Expanded patterns: "aspirational" entries for other game systems registered with
    //     only an anchor string (no pattern, no RVA and no GameFunctions target). With such
    //     generic anchors ("heal", "camp", "price"...) they resolve nothing useful today; they
    //     are hooks for future RE and only show up in the report.
    // ═══════════════════════════════════════════════════════════════════════
    //  EXPANDED PATTERNS — New game systems discovered via RE
    // ═══════════════════════════════════════════════════════════════════════

    // ── Research System ──
    reg("ResearchComplete", "research", "Research item completion",
        nullptr, "Research complete", 17, 0, nullptr);
    reg("ResearchProgress", "research", "Research progress update",
        nullptr, "research progress", 17, 0, nullptr);
    reg("TechUnlock", "research", "Technology unlock handler",
        nullptr, "tech unlock", 11, 0, nullptr);

    // ── Crafting System ──
    reg("CraftItem", "crafting", "Item crafting handler",
        nullptr, "crafting item", 13, 0, nullptr);
    reg("CraftingQueue", "crafting", "Crafting queue management",
        nullptr, "crafting queue", 14, 0, nullptr);
    reg("AutoCraft", "crafting", "Automatic crafting handler",
        nullptr, "auto-craft", 10, 0, nullptr);

    // ── Dialogue System ──
    reg("DialogueStart", "dialogue", "Dialogue initiation",
        nullptr, "StartDialogue", 13, 0, nullptr);
    reg("DialogueChoice", "dialogue", "Dialogue choice handler",
        nullptr, "dialogue choice", 15, 0, nullptr);
    reg("DialogueLine", "dialogue", "Dialogue line display",
        nullptr, "DialogueLine", 12, 0, nullptr);

    // ── Weather System ──
    reg("WeatherUpdate", "weather", "Weather state update",
        nullptr, "weather", 7, 0, nullptr);
    reg("WeatherChange", "weather", "Weather transition handler",
        nullptr, "weather change", 14, 0, nullptr);
    reg("DustStorm", "weather", "Dust storm event handler",
        nullptr, "dust storm", 10, 0, nullptr);
    reg("AcidRain", "weather", "Acid rain damage handler",
        nullptr, "acid rain", 9, 0, nullptr);

    // ── Terrain / World Generation ──
    reg("TerrainLoad", "terrain", "Terrain chunk loading",
        nullptr, "terrain", 7, 0, nullptr);
    reg("WorldGenerate", "terrain", "World generation",
        nullptr, "world generation", 16, 0, nullptr);
    reg("BiomeLoad", "terrain", "Biome data loading",
        nullptr, "biome", 5, 0, nullptr);

    // ── Animation System ──
    reg("AnimationPlay", "animation", "Animation playback",
        nullptr, "playAnimation", 13, 0, nullptr);
    reg("AnimationBlend", "animation", "Animation blend/transition",
        nullptr, "animation blend", 15, 0, nullptr);
    reg("RagdollActivate", "animation", "Ragdoll physics activation",
        nullptr, "ragdoll", 7, 0, nullptr);

    // ── Pathfinding Internals ──
    reg("PathfindRequest", "pathfinding", "Pathfinding request",
        nullptr, "pathfind request", 16, 0, nullptr);
    reg("PathfindComplete", "pathfinding", "Pathfinding completion",
        nullptr, "pathfind complete", 17, 0, nullptr);
    reg("NavmeshBuild", "pathfinding", "Navmesh construction",
        nullptr, "build navmesh", 13, 0, nullptr);

    // ── Audio System ──
    reg("SoundPlay", "audio", "Sound playback",
        nullptr, "playSound", 9, 0, nullptr);
    reg("MusicChange", "audio", "Music track change",
        nullptr, "music", 5, 0, nullptr);

    // ── Mod / Plugin Loading ──
    reg("ModLoad", "modding", "Mod loading handler",
        nullptr, "loadMod", 7, 0, nullptr);
    reg("PluginInit", "modding", "Plugin initialization",
        nullptr, "Plugin_x64", 10, 0, nullptr);

    // ── UI / MyGUI ──
    reg("MyGUICreateWidget", "ui", "MyGUI widget creation",
        nullptr, "createWidget", 12, 0, nullptr);
    reg("MyGUILayoutLoad", "ui", "MyGUI layout loading",
        nullptr, "loadLayout", 10, 0, nullptr);

    // ── Ogre3D / Rendering ──
    reg("OgreRenderFrame", "render", "Ogre render frame",
        nullptr, "renderOneFrame", 14, 0, nullptr);
    reg("OgreLoadMesh", "render", "Ogre mesh loading",
        nullptr, "loadMesh", 8, 0, nullptr);
    reg("OgreLoadMaterial", "render", "Ogre material loading",
        nullptr, "loadMaterial", 12, 0, nullptr);

    // ── Havok Physics ──
    reg("HavokStep", "physics", "Havok physics step",
        nullptr, "stepDeltaTime", 13, 0, nullptr);
    reg("HavokAddBody", "physics", "Havok add rigid body",
        nullptr, "addEntity", 9, 0, nullptr);
    reg("HavokCollision", "physics", "Havok collision callback",
        nullptr, "contactPointCallback", 20, 0, nullptr);

    // ── Economy / Trade ──
    reg("TradeOpen", "economy", "Trade window open",
        nullptr, "trade window", 12, 0, nullptr);
    reg("PriceCalculate", "economy", "Item price calculation",
        nullptr, "price", 5, 0, nullptr);
    reg("MoneyTransfer", "economy", "Money transfer handler",
        nullptr, "money transfer", 14, 0, nullptr);

    // ── Stealth / Detection ──
    reg("StealthCheck", "stealth", "Stealth detection check",
        nullptr, "stealth", 7, 0, nullptr);
    reg("DetectionUpdate", "stealth", "Detection meter update",
        nullptr, "detection", 9, 0, nullptr);
    reg("TheftAttempt", "stealth", "Theft attempt handler",
        nullptr, "steal", 5, 0, nullptr);
    reg("LockpickAttempt", "stealth", "Lockpicking handler",
        nullptr, "lockpick", 8, 0, nullptr);

    // ── Medical / Healing ──
    reg("HealWound", "medical", "Wound healing handler",
        nullptr, "heal wound", 10, 0, nullptr);
    reg("BandageApply", "medical", "Bandage application",
        nullptr, "bandage", 7, 0, nullptr);
    reg("RobotRepair", "medical", "Robot repair handler",
        nullptr, "robot repair", 12, 0, nullptr);

    // ── Slavery System ──
    reg("SlaveryCapture", "slavery", "Slave capture handler",
        nullptr, "enslave", 7, 0, nullptr);
    reg("SlaveryEscape", "slavery", "Slave escape handler",
        nullptr, "escape", 6, 0, nullptr);
    reg("BountyUpdate", "slavery", "Bounty system update",
        nullptr, "bounty", 6, 0, nullptr);

    // ── Camp / Outpost ──
    reg("CampSetup", "camp", "Camp setup handler",
        nullptr, "camp", 4, 0, nullptr);
    reg("WallBuild", "camp", "Defensive wall building",
        nullptr, "wall", 4, 0, nullptr);
    reg("GateToggle", "camp", "Gate open/close handler",
        nullptr, "gate", 4, 0, nullptr);

    // ── Limb System ──
    reg("LimbLoss", "limbs", "Limb loss handler",
        nullptr, "limb loss", 9, 0, nullptr);
    reg("ProstheticAttach", "limbs", "Prosthetic limb attachment",
        nullptr, "prosthetic", 10, 0, nullptr);

    // ── Hunger / Needs ──
    reg("HungerUpdate", "needs", "Hunger update handler",
        nullptr, "hunger", 6, 0, nullptr);
    reg("FeedCharacter", "needs", "Character feeding handler",
        nullptr, "feed", 4, 0, nullptr);

    // ── World State / Events ──
    reg("WorldStateChange", "worldstate", "World state change event",
        nullptr, "world state", 11, 0, nullptr);
    reg("FactionEvent", "worldstate", "Faction event trigger",
        nullptr, "faction event", 13, 0, nullptr);
    reg("RaidTrigger", "worldstate", "Raid trigger handler",
        nullptr, "raid", 4, 0, nullptr);

    spdlog::info("Orchestrator: Registered {} pattern entries", m_entries.size());
}

// ES: Métodos internos de resolución.
// ═══════════════════════════════════════════════════════════════════════════
//  INTERNAL RESOLUTION METHODS
// ═══════════════════════════════════════════════════════════════════════════

// ES: Marca una entrada como resuelta y escribe la dirección en su destino. Filtro de
//     alineación: una FUNCIÓN resuelta por patrón, string o RVA fija que no esté alineada a
//     16 se rechaza (probable acierto a mitad de función) y se deja sin resolver para que lo
//     intenten fases posteriores. No aplica a globales ni a vtable/complex/callgraph.
// EN: Marks an entry as resolved and writes the address into its target. Alignment
//     filter: a FUNCTION resolved by pattern, string or fixed RVA that is not 16-byte
//     aligned is rejected (likely mid-function hit) and left unresolved so later phases can
//     try. Does not apply to globals nor to vtable/complex/callgraph.
void PatternOrchestrator::ResolveEntry(PatternEntry& entry, uintptr_t address,
                                        ResolutionMethod method, float confidence) {
    // MSVC x64 functions are 16-byte aligned. A non-aligned address from pattern scan
    // or string xref is a mid-function hit (SEH handler block etc.) — reject it so
    // later phases (vtable resolution, call graph) can still try.
    if (!entry.isGlobalPointer && (address & 0xF) != 0 &&
        (method == ResolutionMethod::PatternScan || method == ResolutionMethod::StringXref ||
         method == ResolutionMethod::HardcodedOffset)) {
        spdlog::warn("  Rejecting '{}' = 0x{:X} via {} — NOT 16-byte aligned (0x{:X}), "
                     "likely mid-function hit",
                     entry.id, address, ResolutionMethodName(method), address & 0xF);
        return; // Don't mark resolved — let later phases try
    }

    entry.resolvedAddress = address;
    entry.resolvedMethod = method;
    entry.confidence = confidence;
    entry.isResolved = true;

    // Write to target pointers
    if (entry.targetPtr) {
        *entry.targetPtr = reinterpret_cast<void*>(address);
    }
    if (entry.targetUintptr) {
        *entry.targetUintptr = address;
    }

    spdlog::info("  Resolved '{}' = 0x{:X} via {} (confidence {:.0f}%)",
                 entry.id, address, ResolutionMethodName(method), confidence * 100);
}

// ES: Resolución por patrón AOB (primera coincidencia, confianza 1.0). Nota: devuelve true
//     aunque ResolveEntry haya rechazado la dirección por alineación.
// EN: AOB pattern resolution (first match, confidence 1.0). Note: returns true even if
//     ResolveEntry rejected the address because of alignment.
bool PatternOrchestrator::TryPatternScan(PatternEntry& entry) {
    if (!entry.pattern) return false;
    auto result = m_scanner.Find(entry.pattern);
    if (!result) return false;
    ResolveEntry(entry, result.address, ResolutionMethod::PatternScan, 1.0f);
    return true;
}

// ES: Resolución por string: estrategia 1 = funciones que referencian el string según la
//     base de xrefs del StringAnalyzer (toma la primera, 0.85); estrategia 2 = buscar el
//     string y usar la función de su primer xref (0.8) o, si .pdata no la mapea,
//     retroceder hasta el prólogo (0.7). La búsqueda es por subcadena.
// EN: String resolution: strategy 1 = functions referencing the string according to the
//     StringAnalyzer xref database (takes the first, 0.85); strategy 2 = find the string
//     and use its first xref's function (0.8) or, if .pdata cannot map it, walk back to
//     the prologue (0.7). The search is by substring.
bool PatternOrchestrator::TryStringXref(PatternEntry& entry) {
    if (!entry.stringAnchor || entry.stringAnchorLen <= 0) return false;

    std::string searchStr(entry.stringAnchor, entry.stringAnchorLen);

    // Strategy 1: Use the string analyzer's pre-built xref database
    auto funcs = m_strings.FindFunctionsReferencingString(searchStr);
    if (!funcs.empty()) {
        spdlog::info("  StringXref '{}': found {} referencing functions via analyzer (using 0x{:X})",
                     entry.id, funcs.size(), funcs[0]);
        ResolveEntry(entry, funcs[0], ResolutionMethod::StringXref, 0.85f);
        return true;
    }

    // Strategy 2: Direct string lookup + xref check
    auto matchingStrings = m_strings.FindStrings(searchStr);
    if (!matchingStrings.empty()) {
        spdlog::info("  StringXref '{}': found {} matching strings, first has {} xrefs",
                     entry.id, matchingStrings.size(),
                     matchingStrings[0]->xrefs.size());
        if (!matchingStrings[0]->xrefs.empty()) {
            uintptr_t funcAddr = matchingStrings[0]->xrefs[0].funcAddress;
            if (funcAddr) {
                ResolveEntry(entry, funcAddr, ResolutionMethod::StringXref, 0.8f);
                return true;
            }
            // .pdata couldn't map this xref to a function — try prologue walk-back
            uintptr_t codeAddr = matchingStrings[0]->xrefs[0].codeAddress;
            if (codeAddr) {
                uintptr_t funcStart = WalkBackToPrologue(codeAddr);
                if (funcStart) {
                    spdlog::info("  StringXref '{}': prologue walk-back from 0x{:X} → 0x{:X}",
                                 entry.id, codeAddr, funcStart);
                    ResolveEntry(entry, funcStart, ResolutionMethod::StringXref, 0.7f);
                    return true;
                }
            }
            spdlog::warn("  StringXref '{}': xref exists but funcAddress=0 and walk-back failed", entry.id);
        } else {
            spdlog::warn("  StringXref '{}': string found at 0x{:X} but no code xrefs "
                         "(no LEA referencing it)", entry.id, matchingStrings[0]->address);
        }
    } else {
        spdlog::warn("  StringXref '{}': string '{}' NOT FOUND in binary",
                     entry.id, searchStr);
    }

    return false;
}

// ES: Resolución por vtable: prueba cada clase candidata ("A|B|C") y toma la función del
//     slot indicado en la primera clase que exista (confianza 0.9).
// EN: Vtable resolution: tries each candidate class ("A|B|C") and takes the function in
//     the given slot of the first class that exists (confidence 0.9).
bool PatternOrchestrator::TryVTableSlot(PatternEntry& entry) {
    if (entry.vtableClass.empty() || entry.vtableSlot < 0) return false;

    // Support "|"-separated candidate class names (e.g. "ActivePlatoon|Platoon|Squad")
    // Try each candidate in order; first match wins.
    std::string candidates = entry.vtableClass;
    size_t pos = 0;
    while (pos < candidates.size()) {
        size_t delim = candidates.find('|', pos);
        std::string name = candidates.substr(pos, delim == std::string::npos ? std::string::npos : delim - pos);
        pos = (delim == std::string::npos) ? candidates.size() : delim + 1;

        if (name.empty()) continue;

        uintptr_t func = m_vtables.GetVirtualFunction(name, entry.vtableSlot);
        if (func) {
            spdlog::info("  VTableSlot '{}': resolved via RTTI class '{}' slot {} -> 0x{:X}",
                         entry.id, name, entry.vtableSlot, func);
            ResolveEntry(entry, func, ResolutionMethod::VTableSlot, 0.9f);
            return true;
        }
    }

    return false;
}

// ES: Resolución por RVA fija (base + hardcodedRVA). Globales: se aceptan si el valor es 0
//     (partida sin cargar, 0.5) o un puntero legible con doble desreferencia (0.95).
//     Funciones: SEGURIDAD — si la entrada tiene patrón y el patrón falló, el binario es
//     otro (p.ej. Steam vs GOG) y la RVA apuntaría a otra función -> NO se acepta. Sin
//     patrón, solo se acepta si .pdata confirma que es inicio de función (0.6).
// EN: Fixed RVA resolution (base + hardcodedRVA). Globals: accepted if the value is 0
//     (game not loaded, 0.5) or a readable pointer with double dereference (0.95).
//     Functions: SAFETY — if the entry has a pattern and the pattern failed, the binary is
//     different (e.g. Steam vs GOG) and the RVA would point to another function -> NOT
//     accepted. Without a pattern, only accepted if .pdata confirms a function start (0.6).
bool PatternOrchestrator::TryHardcodedOffset(PatternEntry& entry) {
    if (entry.hardcodedRVA == 0) return false;
    uintptr_t addr = m_scanner.GetBase() + entry.hardcodedRVA;

    if (entry.isGlobalPointer) {
        // Validate that we can read through the pointer
        uintptr_t val = 0;
        if (Memory::Read(addr, val)) {
            // Accept if value is 0 (game not loaded yet) or a valid heap/stack pointer.
            // Additional checks: a valid Kenshi object pointer should be page-aligned-ish
            // and readable (not just a random number in user-mode range).
            if (val == 0) {
                ResolveEntry(entry, addr, ResolutionMethod::HardcodedOffset, 0.5f);
                return true;
            }
            if (val > 0x10000 && val < 0x00007FFFFFFFFFFF &&
                val != 0xFFFFFFFFFFFFFFFF && val != 0xCCCCCCCCCCCCCCCC &&
                val != 0xCDCDCDCDCDCDCDCD) {
                // Double-dereference check: a real object pointer should itself be readable
                uintptr_t vtable = 0;
                if (Memory::Read(val, vtable) && vtable > 0x10000 && vtable < 0x00007FFFFFFFFFFF) {
                    ResolveEntry(entry, addr, ResolutionMethod::HardcodedOffset, 0.95f);
                    return true;
                }
                spdlog::debug("  Hardcoded global 0x{:X} for '{}' reads 0x{:X} but deref failed — rejecting",
                              addr, entry.id, val);
            }
        }
        return false;
    }

    // ES: No fiarse a ciegas de RVAs fijas (ver explicación en inglés abajo).
    // ═══ SAFETY: Don't blindly trust GOG hardcoded RVAs on Steam ═══
    // Hardcoded RVAs are version-specific (GOG v1.0.68). On Steam, these addresses
    // point to DIFFERENT functions. If we have a pattern for this function and the
    // pattern scan already failed (Phase 4), the bytes at this address don't match
    // our expectations — meaning this is a different binary. Hooking a random function
    // at this address would cause a CRASH.
    //
    // Only accept hardcoded RVAs when:
    //   1. No pattern exists (can't validate byte-level) — still risky, so skip too
    //   2. OR pattern scan succeeded (handled in Phase 4, won't reach here)
    if (entry.pattern) {
        // Pattern exists but scan failed → wrong binary version → skip hardcoded RVA
        spdlog::debug("  Skipping hardcoded RVA 0x{:X} for '{}' — pattern didn't match "
                      "(binary version mismatch, likely Steam vs GOG)", addr, entry.id);
        return false;
    }

    // ES: Sin patrón: validación más débil con .pdata.
    // No pattern available — validate via .pdata as a weaker check
    auto* func = m_pdata.FindFunction(addr);
    if (func) {
        spdlog::warn("  Accepting hardcoded RVA 0x{:X} for '{}' via .pdata (no pattern to validate — "
                     "may be wrong on Steam)", addr, entry.id);
        ResolveEntry(entry, addr, ResolutionMethod::HardcodedOffset, 0.6f);
        return true;
    }

    return false;
}

// ES: Resolución por patrón compuesto (ancla + componentes) con el motor de escaneo.
// EN: Composite pattern resolution (anchor + components) via the scan engine.
bool PatternOrchestrator::TryComplexPattern(PatternEntry& entry) {
    if (entry.complexPattern.components.empty()) return false;
    auto result = m_scanner.FindComplex(entry.complexPattern);
    if (!result) return false;
    ResolveEntry(entry, result.address, ResolutionMethod::ComplexPattern, result.confidence);
    return true;
}

// ES: "Grafo de llamadas": en realidad solo busca una función etiquetada por strings cuyo
//     nombre coincida con el id de la entrada (confianza 0.6); no recorre el grafo.
// EN: "Call graph": it actually just looks for a string-labeled function whose name
//     matches the entry id (confidence 0.6); it does not walk the graph.
bool PatternOrchestrator::TryCallGraphTrace(PatternEntry& entry) {
    if (!entry.stringAnchor) return false;

    // Try to find via call graph: find a nearby labeled function,
    // then trace calls to find the target
    auto* labeled = m_strings.FindLabeledFunction(entry.id);
    if (labeled) {
        ResolveEntry(entry, labeled->address, ResolutionMethod::CallGraphTrace, 0.6f);
        return true;
    }

    return false;
}

// ES: Retroceso hasta el prólogo.
// ═══════════════════════════════════════════════════════════════════════════
//  PROLOGUE WALK-BACK
// ═══════════════════════════════════════════════════════════════════════════

// ES: Retrocede desde una dirección de código hasta el inicio de la función. Estrategia 1:
//     buscar padding CC y comprobar que lo siguiente parece un prólogo MSVC conocido.
//     Estrategia 2: probar direcciones alineadas a 16 con 'mov rax,rsp' o 'push REX'.
//     Nota: usa la base del módulo como límite inferior, no el inicio de .text.
// EN: Walks back from a code address to the function start. Strategy 1: look for CC
//     padding and check that what follows looks like a known MSVC prologue. Strategy 2:
//     try 16-byte aligned addresses with 'mov rax,rsp' or 'push REX'. Note: it uses the
//     module base as lower bound, not the start of .text.
uintptr_t PatternOrchestrator::WalkBackToPrologue(uintptr_t codeAddr, int maxDistance) const {
    // Walk backwards from a code address looking for common x64 function prologues.
    // Kenshi (MSVC-compiled) uses these consistently:
    //   48 8B C4       mov rax, rsp
    //   40 55          push rbp (with REX)
    //   40 53          push rbx (with REX)
    //   40 56          push rsi (with REX)
    //   40 57          push rdi (with REX)
    //   48 89 5C 24    mov [rsp+xx], rbx
    //   48 83 EC       sub rsp, xx
    //   CC             int3 padding (function boundary marker)
    //
    // Strategy: walk back byte-by-byte. When we hit CC padding (int3),
    // the next non-CC byte is the function start. Also check for known
    // prologue patterns at each position.

    uintptr_t textBase = m_scanner.GetBase();
    uintptr_t textEnd = textBase + m_scanner.GetSize();
    uintptr_t searchStart = codeAddr - maxDistance;
    if (searchStart < textBase) searchStart = textBase;

    // ES: Estrategia 1: padding CC -> inicio de función con prólogo reconocido.
    // Strategy 1: Walk back looking for CC padding → function start
    for (uintptr_t addr = codeAddr - 1; addr >= searchStart; addr--) {
        uint8_t byte = 0;
        if (!Memory::Read(addr, byte)) break;

        if (byte == 0xCC) {
            // Found int3 padding — function starts at next byte
            uintptr_t funcStart = addr + 1;

            // Validate: check the bytes at funcStart look like a prologue
            uint32_t dword = 0;
            if (Memory::Read(funcStart, dword)) {
                uint8_t p0 = dword & 0xFF, p1 = (dword >> 8) & 0xFF, p2 = (dword >> 16) & 0xFF;
                // Common MSVC prologues
                if (p0 == 0x48 && p1 == 0x8B && p2 == 0xC4) return funcStart;
                if (p0 == 0x48 && p1 == 0x89 && p2 == 0x5C) return funcStart;
                if (p0 == 0x48 && p1 == 0x83 && p2 == 0xEC) return funcStart;
                if (p0 == 0x40 && (p1 >= 0x53 && p1 <= 0x57)) return funcStart;
                if (p0 == 0x55 || p0 == 0x53) return funcStart; // push rbp/rbx
                if (p0 == 0x41 && (p1 >= 0x54 && p1 <= 0x57)) return funcStart;
                if (p0 == 0x48 && p1 == 0x81 && p2 == 0xEC) return funcStart;
            }
            // CC but no recognized prologue — keep going (might be data)
        }
    }

    // ES: Estrategia 2: direcciones alineadas a 16 con prólogo típico.
    // Strategy 2: Walk back checking for prologue patterns at aligned addresses
    // Functions are often (but not always) 16-byte aligned
    for (uintptr_t addr = codeAddr & ~0xF; addr >= searchStart; addr -= 16) {
        uint32_t dw = 0;
        if (!Memory::Read(addr, dw)) break;
        uint8_t p0 = dw & 0xFF, p1 = (dw >> 8) & 0xFF, p2 = (dw >> 16) & 0xFF;

        if (p0 == 0x48 && p1 == 0x8B && p2 == 0xC4) return addr;
        if (p0 == 0x40 && (p1 >= 0x53 && p1 <= 0x57)) return addr;
    }

    return 0;
}

// ES: Búsqueda directa de string (respaldo de emergencia).
// ═══════════════════════════════════════════════════════════════════════════
//  DIRECT STRING SEARCH (emergency fallback)
// ═══════════════════════════════════════════════════════════════════════════

// ES: Emergencia (fase 8): busca el string byte a byte en .rdata, luego un LEA
//     RIP-relativo (48 8D / 4C 8D) en .text que lo referencie y retrocede hasta el prólogo.
//     Como alternativa usa .pdata, pero con FindFunction (inicio exacto) sobre la dirección
//     del LEA, que casi nunca coincidirá; FindContaining sería lo esperable.
// EN: Emergency (phase 8): searches the string byte by byte in .rdata, then a RIP-relative
//     LEA (48 8D / 4C 8D) in .text referencing it, and walks back to the prologue. As an
//     alternative it uses .pdata, but with FindFunction (exact start) on the LEA address,
//     which will almost never match; FindContaining would be the expected call.
bool PatternOrchestrator::TryDirectStringSearch(PatternEntry& entry) {
    if (!entry.stringAnchor || entry.stringAnchorLen <= 0) return false;

    uintptr_t moduleBase = m_scanner.GetBase();
    size_t moduleSize = m_scanner.GetSize();

    // ES: Paso 1: localizar el string en .rdata.
    // Step 1: Find the string in .rdata by scanning for its bytes
    const auto* rdata = m_scanner.FindSection(".rdata");
    if (!rdata) return false;

    std::string needle(entry.stringAnchor, entry.stringAnchorLen);
    uintptr_t stringAddr = 0;

    for (uintptr_t addr = rdata->base; addr < rdata->base + rdata->size - needle.size(); addr++) {
        bool match = true;
        for (size_t i = 0; i < needle.size(); i++) {
            uint8_t byte = 0;
            if (!Memory::Read(addr + i, byte) || byte != static_cast<uint8_t>(needle[i])) {
                match = false;
                break;
            }
        }
        if (match) {
            stringAddr = addr;
            break;
        }
    }

    if (!stringAddr) {
        spdlog::debug("  DirectStringSearch '{}': string not found in .rdata", entry.id);
        return false;
    }

    spdlog::info("  DirectStringSearch '{}': string found at 0x{:X}", entry.id, stringAddr);

    // ES: Paso 2: buscar en .text un LEA que lo referencie.
    // Step 2: Scan .text for LEA instructions that reference this string
    // LEA reg, [rip+disp32] = 48 8D xx yy yy yy yy (7 bytes)
    // or without REX: 8D xx yy yy yy yy (6 bytes)
    const auto* text = m_scanner.FindSection(".text");
    if (!text) return false;

    for (uintptr_t addr = text->base; addr < text->base + text->size - 7; addr++) {
        uint8_t b0 = 0, b1 = 0, b2 = 0;
        Memory::Read(addr, b0);
        Memory::Read(addr + 1, b1);
        Memory::Read(addr + 2, b2);

        // Check for LEA with REX.W prefix: 48 8D xx [disp32]
        // ModRM byte: xx where mod=00, rm=101 (RIP-relative) → (b2 & 0xC7) == 0x05
        if (b0 == 0x48 && b1 == 0x8D && (b2 & 0xC7) == 0x05) {
            int32_t disp = 0;
            if (!Memory::Read(addr + 3, disp)) continue;

            uintptr_t target = addr + 7 + disp; // RIP-relative: next instruction + displacement
            if (target == stringAddr) {
                spdlog::info("  DirectStringSearch '{}': LEA xref at 0x{:X}", entry.id, addr);

                // Step 3: Walk back from LEA to find function start
                uintptr_t funcStart = WalkBackToPrologue(addr);
                if (funcStart) {
                    spdlog::info("  DirectStringSearch '{}': prologue at 0x{:X}", entry.id, funcStart);
                    ResolveEntry(entry, funcStart, ResolutionMethod::StringXref, 0.75f);
                    return true;
                }

                // Try .pdata as alternative
                auto* func = m_pdata.FindFunction(addr);
                if (func) {
                    spdlog::info("  DirectStringSearch '{}': .pdata func at 0x{:X}", entry.id, func->startVA);
                    ResolveEntry(entry, func->startVA, ResolutionMethod::StringXref, 0.8f);
                    return true;
                }
            }
        }

        // Also check 4C 8D (LEA r8-r15, [rip+disp32])
        if (b0 == 0x4C && b1 == 0x8D && (b2 & 0xC7) == 0x05) {
            int32_t disp = 0;
            if (!Memory::Read(addr + 3, disp)) continue;

            uintptr_t target = addr + 7 + disp;
            if (target == stringAddr) {
                spdlog::info("  DirectStringSearch '{}': LEA r8+ xref at 0x{:X}", entry.id, addr);
                uintptr_t funcStart = WalkBackToPrologue(addr);
                if (funcStart) {
                    ResolveEntry(entry, funcStart, ResolutionMethod::StringXref, 0.75f);
                    return true;
                }
                auto* func = m_pdata.FindFunction(addr);
                if (func) {
                    ResolveEntry(entry, func->startVA, ResolutionMethod::StringXref, 0.8f);
                    return true;
                }
            }
        }
    }

    spdlog::warn("  DirectStringSearch '{}': string at 0x{:X} but no LEA xref found", entry.id, stringAddr);
    return false;
}

// ES: RVA validada por prólogo (respaldo de emergencia para funciones críticas).
// ═══════════════════════════════════════════════════════════════════════════
//  PROLOGUE-VALIDATED RVA (emergency fallback for critical functions)
// ═══════════════════════════════════════════════════════════════════════════

// ES: Emergencia (fase 8): acepta la RVA fija si los 4 primeros bytes parecen un prólogo
//     (confianza 0.55). Ojo: a diferencia de TryHardcodedOffset, NO comprueba si existe un
//     patrón que haya fallado, así que en un binario distinto podría aceptar una función
//     equivocada que empiece por un prólogo típico.
// EN: Emergency (phase 8): accepts the fixed RVA if the first 4 bytes look like a
//     prologue (confidence 0.55). Note: unlike TryHardcodedOffset it does NOT check whether
//     a pattern exists and failed, so on a different binary it could accept a wrong
//     function that starts with a typical prologue.
bool PatternOrchestrator::TryPrologueValidatedRVA(PatternEntry& entry) {
    if (entry.hardcodedRVA == 0) return false;

    uintptr_t addr = m_scanner.GetBase() + entry.hardcodedRVA;

    // Read 4 bytes at the target and check for a valid function prologue
    uint32_t dword = 0;
    if (!Memory::Read(addr, dword)) return false;
    uint8_t p0 = dword & 0xFF, p1 = (dword >> 8) & 0xFF;
    uint8_t p2 = (dword >> 16) & 0xFF, p3 = (dword >> 24) & 0xFF;

    bool validPrologue =
        (p0 == 0x48 && p1 == 0x8B && p2 == 0xC4) ||  // mov rax, rsp
        (p0 == 0x48 && p1 == 0x89 && p2 == 0x5C) ||  // mov [rsp+xx], rbx
        (p0 == 0x48 && p1 == 0x83 && p2 == 0xEC) ||  // sub rsp, xx
        (p0 == 0x48 && p1 == 0x81 && p2 == 0xEC) ||  // sub rsp, large
        (p0 == 0x40 && p1 >= 0x53 && p1 <= 0x57) ||  // push r with REX
        (p0 == 0x41 && p1 >= 0x54 && p1 <= 0x57) ||  // push r8-r15
        (p0 == 0x55) ||  // push rbp
        (p0 == 0x53) ||  // push rbx
        (p0 == 0x56) ||  // push rsi
        (p0 == 0x57);    // push rdi

    if (!validPrologue) {
        spdlog::debug("  PrologueRVA '{}': 0x{:X} has bytes {:02X} {:02X} {:02X} {:02X} — not a prologue",
                       entry.id, addr, p0, p1, p2, p3);
        return false;
    }

    spdlog::warn("  PrologueRVA '{}': EMERGENCY accepting 0x{:X} (prologue {:02X} {:02X} {:02X} {:02X})",
                 entry.id, addr, p0, p1, p2, p3);
    ResolveEntry(entry, addr, ResolutionMethod::HardcodedOffset, 0.55f);
    return true;
}

// ES: Fases del pipeline.
// ═══════════════════════════════════════════════════════════════════════════
//  PIPELINE PHASES
// ═══════════════════════════════════════════════════════════════════════════

// ES: Milisegundos transcurridos desde 'start'.
// EN: Milliseconds elapsed since 'start'.
double PatternOrchestrator::ElapsedMs(TimePoint start) const {
    auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// ES: Fase 1: enumerar .pdata (límites de función autoritativos).
// EN: Phase 1: enumerate .pdata (authoritative function boundaries).
void PatternOrchestrator::RunPhase1_PData() {
    if (!m_config.enablePData) return;
    auto start = Clock::now();

    spdlog::info("Orchestrator Phase 1: .pdata enumeration");
    m_pdata.Enumerate();

    m_lastReport.pdataFunctions = m_pdata.GetFunctionCount();
    m_lastReport.timing.pdata = ElapsedMs(start);
    spdlog::info("  Phase 1 complete: {} functions in {:.1f}ms",
                 m_lastReport.pdataFunctions, m_lastReport.timing.pdata);
}

// ES: Fase 2: descubrir strings, resolver sus xrefs y etiquetar funciones.
// EN: Phase 2: discover strings, resolve their xrefs and label functions.
void PatternOrchestrator::RunPhase2_Strings() {
    if (!m_config.enableStrings) return;
    auto start = Clock::now();

    spdlog::info("Orchestrator Phase 2: String discovery + xref analysis");

    // Re-init strings with .pdata now available
    m_strings.Init(m_scanner.GetBase(), m_scanner.GetSize(), &m_pdata);
    m_strings.ScanStrings(m_config.stringMinLength);
    if (m_config.scanWideStrings) {
        m_strings.ScanWideStrings(m_config.stringMinLength);
    }
    m_strings.ResolveXrefs();
    m_strings.LabelFunctions();

    m_lastReport.stringsFound = m_strings.GetStringCount();
    m_lastReport.xrefsResolved = m_strings.GetXrefCount();
    m_lastReport.labeledFunctions = m_strings.GetLabeledFunctionCount();
    m_lastReport.timing.strings = ElapsedMs(start);
    spdlog::info("  Phase 2 complete: {} strings, {} xrefs, {} labeled in {:.1f}ms",
                 m_lastReport.stringsFound, m_lastReport.xrefsResolved,
                 m_lastReport.labeledFunctions, m_lastReport.timing.strings);
}

// ES: Fase 3: escanear vtables/RTTI y volcar al log las clases de platoon/squad/active
//     (diagnóstico para localizar SquadAddMember).
// EN: Phase 3: scan vtables/RTTI and log platoon/squad/active classes (diagnostics to
//     locate SquadAddMember).
void PatternOrchestrator::RunPhase3_VTables() {
    if (!m_config.enableVTables) return;
    auto start = Clock::now();

    spdlog::info("Orchestrator Phase 3: VTable scanning + RTTI");
    m_vtables.Init(m_scanner.GetBase(), m_scanner.GetSize(), &m_pdata);
    m_vtables.ScanVTables();

    // Diagnostic: dump all RTTI classes related to squad/platoon for SquadAddMember discovery
    m_vtables.ForEach([](const VTableInfo& vt) {
        // Case-insensitive substring check for diagnostic keywords
        std::string lower = vt.className;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lower.find("platoon") != std::string::npos ||
            lower.find("squad") != std::string::npos ||
            lower.find("active") != std::string::npos) {
            spdlog::info("  RTTI class '{}' (mangled: {}) — {} slots, vtable@0x{:X}",
                         vt.className, vt.mangledName, vt.slotCount, vt.address);
            // Log first 5 slots for identification
            for (size_t i = 0; i < vt.slots.size() && i < 5; i++) {
                spdlog::info("    slot[{}] = 0x{:X} {}", i, vt.slots[i].funcAddress,
                             vt.slots[i].funcLabel.empty() ? "" : vt.slots[i].funcLabel);
            }
        }
    });

    m_lastReport.vtablesFound = m_vtables.GetVTableCount();
    m_lastReport.timing.vtables = ElapsedMs(start);
    spdlog::info("  Phase 3 complete: {} vtables in {:.1f}ms",
                 m_lastReport.vtablesFound, m_lastReport.timing.vtables);
}

// ES: Fase 4: escaneo por lotes (una sola pasada por .text) de todos los patrones
//     pendientes. Nota: el contador resolvedByPattern sube aunque ResolveEntry rechace la
//     dirección por alineación.
// EN: Phase 4: batch scan (single pass over .text) of all pending patterns. Note: the
//     resolvedByPattern counter increases even if ResolveEntry rejects the address.
void PatternOrchestrator::RunPhase4_PatternScan() {
    if (!m_config.enableBatchScan) return;
    auto start = Clock::now();

    spdlog::info("Orchestrator Phase 4: SIMD batch pattern scan");

    // Build batch entries for all patterns
    std::vector<BatchEntry> batchEntries;
    std::vector<size_t> entryIndices; // Maps batch index → entry index

    for (size_t i = 0; i < m_entries.size(); i++) {
        auto& entry = m_entries[i];
        if (entry.isResolved) continue;
        if (!entry.pattern) continue;

        auto parsed = ScannerEngine::Parse(entry.pattern);
        if (!parsed) continue;

        BatchEntry be;
        be.id = entry.id;
        be.pattern = *parsed;
        batchEntries.push_back(std::move(be));
        entryIndices.push_back(i);
    }

    if (!batchEntries.empty()) {
        m_scanner.BatchScan(batchEntries);

        // Apply results
        for (size_t i = 0; i < batchEntries.size(); i++) {
            if (batchEntries[i].result.valid) {
                auto& entry = m_entries[entryIndices[i]];
                ResolveEntry(entry, batchEntries[i].result.address,
                             ResolutionMethod::PatternScan, 1.0f);
                m_lastReport.resolvedByPattern++;
            }
        }
    }

    m_lastReport.timing.patternScan = ElapsedMs(start);
    spdlog::info("  Phase 4 complete: {} resolved via pattern in {:.1f}ms",
                 m_lastReport.resolvedByPattern, m_lastReport.timing.patternScan);
}

// ES: Fase 5: para lo que siga sin resolver, prueba en orden string, vtable, RVA fija y
//     patrón compuesto. Nota: si un método devuelve true pero ResolveEntry rechazó la
//     dirección, se pasa a la siguiente entrada sin probar los demás métodos.
// EN: Phase 5: for anything still unresolved, tries string, vtable, fixed RVA and
//     composite pattern in order. Note: if a method returns true but ResolveEntry rejected
//     the address, it moves on to the next entry without trying the remaining methods.
void PatternOrchestrator::RunPhase5_StringFallback() {
    auto start = Clock::now();

    spdlog::info("Orchestrator Phase 5: String xref fallback");

    for (auto& entry : m_entries) {
        if (entry.isResolved) continue;

        // Try string xref
        if (TryStringXref(entry)) {
            m_lastReport.resolvedByString++;
            continue;
        }

        // Try vtable slot
        if (TryVTableSlot(entry)) {
            m_lastReport.resolvedByVTable++;
            continue;
        }

        // Try hardcoded offset
        if (TryHardcodedOffset(entry)) {
            m_lastReport.resolvedByHardcoded++;
            continue;
        }

        // Try complex pattern
        if (TryComplexPattern(entry)) {
            m_lastReport.resolvedByComplex++;
            continue;
        }
    }

    m_lastReport.timing.stringFallback = ElapsedMs(start);
    spdlog::info("  Phase 5 complete: string={}, vtable={}, hardcoded={}, complex={} in {:.1f}ms",
                 m_lastReport.resolvedByString, m_lastReport.resolvedByVTable,
                 m_lastReport.resolvedByHardcoded, m_lastReport.resolvedByComplex,
                 m_lastReport.timing.stringFallback);
}

// ES: Fase 6: construye el grafo (completo o solo de lo resuelto), propaga etiquetas y
//     prueba TryCallGraphTrace con lo que quede.
// EN: Phase 6: builds the graph (full or only resolved functions), propagates labels
//     and tries TryCallGraphTrace on what remains.
void PatternOrchestrator::RunPhase6_CallGraph() {
    if (!m_config.enableCallGraph) return;
    auto start = Clock::now();

    spdlog::info("Orchestrator Phase 6: Call graph analysis");

    if (m_config.fullCallGraph) {
        m_callGraph.BuildFullGraph();
    } else {
        // Build graph only for resolved functions
        std::vector<uintptr_t> knownAddrs;
        for (const auto& entry : m_entries) {
            if (entry.isResolved) knownAddrs.push_back(entry.resolvedAddress);
        }
        m_callGraph.BuildGraphFor(knownAddrs);
    }

    // Propagate labels
    if (m_config.enableLabelPropagation) {
        std::unordered_map<uintptr_t, std::string> labels;
        for (const auto& entry : m_entries) {
            if (entry.isResolved) {
                labels[entry.resolvedAddress] = entry.id;
            }
        }
        m_callGraph.PropagateLabels(labels, m_config.callGraphDepth);
    }

    // Try to resolve remaining entries via call graph
    for (auto& entry : m_entries) {
        if (entry.isResolved) continue;
        if (TryCallGraphTrace(entry)) {
            m_lastReport.resolvedByCallGraph++;
        }
    }

    m_lastReport.callGraphNodes = m_callGraph.GetNodeCount();
    m_lastReport.callGraphEdges = m_callGraph.GetEdgeCount();
    m_lastReport.timing.callGraph = ElapsedMs(start);
    spdlog::info("  Phase 6 complete: {} nodes, {} edges, {} resolved in {:.1f}ms",
                 m_lastReport.callGraphNodes, m_lastReport.callGraphEdges,
                 m_lastReport.resolvedByCallGraph, m_lastReport.timing.callGraph);
}

// ES: Escáner con SEH para descubrir punteros globales (separado de RunPhase7 por el error
//     C2712). Busca en el código de una función MOV/LEA reg,[RIP+disp32] hacia .data (o
//     cualquier zona del módulo que no sea .text ni .rdata) y acepta el primer destino cuyo
//     valor sea 0 (partida sin cargar) o un puntero de heap con vtable dentro del módulo.
// SEH-safe code scanner for global pointer discovery (extracted from RunPhase7 to avoid C2712).
// Scans function code bytes for RIP-relative MOV/LEA instructions targeting .data section.
// Uses raw pointer validation instead of std::function to allow __try.
// Returns true if a valid global pointer candidate was found (result in outTarget/outInstr).
static bool SEH_ScanCodeForGlobalPtr(
    const uint8_t* code, size_t codeLen, uintptr_t funcAddr,
    uintptr_t dataSectionBase, size_t dataSectionSize,
    uintptr_t textSectionBase, size_t textSectionSize,
    uintptr_t rdataSectionBase, size_t rdataSectionSize,
    uintptr_t moduleBase, size_t moduleSize,
    uintptr_t& outTarget, uintptr_t& outInstr)
{
    __try {
        for (size_t i = 0; i + 7 < codeLen; i++) {
            bool hasRex = (code[i] == 0x48 || code[i] == 0x4C);
            bool isMemOp = (code[i + 1] == 0x8B || code[i + 1] == 0x8D);
            if (!hasRex || !isMemOp) continue;

            uint8_t modrm = code[i + 2];
            if ((modrm & 0xC7) != 0x05) continue;

            int32_t disp;
            std::memcpy(&disp, &code[i + 3], 4);
            uintptr_t instrAddr = funcAddr + i;
            uintptr_t target = instrAddr + 7 + disp;

            // Check if target is in .data section (writable globals)
            bool inData = (dataSectionBase != 0) &&
                          target >= dataSectionBase &&
                          target < dataSectionBase + dataSectionSize;
            // Also accept any in-module address OUTSIDE .text and .rdata
            if (!inData && target >= moduleBase && target < moduleBase + moduleSize) {
                bool inText = (textSectionBase != 0) &&
                              target >= textSectionBase &&
                              target < textSectionBase + textSectionSize;
                bool inRdata = (rdataSectionBase != 0) &&
                               target >= rdataSectionBase &&
                               target < rdataSectionBase + rdataSectionSize;
                inData = !inText && !inRdata;
            }
            if (!inData) continue;

            // Quick validation: read the pointer value
            uintptr_t val = 0;
            if (!Memory::Read(target, val)) continue;
            // Accept null (game not loaded) or heap pointer (outside module)
            if (val == 0) {
                outTarget = target;
                outInstr = instrAddr;
                return true;
            }
            if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) continue;
            if (val == 0xFFFFFFFFFFFFFFFF || val == 0xCCCCCCCCCCCCCCCC ||
                val == 0xCDCDCDCDCDCDCDCD) continue;
            if (val >= moduleBase && val < moduleBase + moduleSize) continue;
            // Double-deref: check vtable points into module
            uintptr_t vtable = 0;
            if (!Memory::Read(val, vtable)) continue;
            if (vtable >= moduleBase && vtable < moduleBase + moduleSize) {
                outTarget = target;
                outInstr = instrAddr;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Code read faulted — skip this function
    }
    return false;
}

// ES: Fase 7: (7a) para globales sin resolver con string ancla, busca las funciones que
//     referencian el string y escanea su código buscando el global; (7b) valida los
//     globales ya resueltos (heap + vtable). Nota: con la instancia embebida de GameWorld
//     (1.0.68) el valor leído es la vtable (dentro del módulo), así que 7b la marca como
//     "sospechosa" (confianza 0.2) aunque sea correcta.
// EN: Phase 7: (7a) for unresolved globals with an anchor string, finds the functions
//     referencing the string and scans their code for the global; (7b) validates already
//     resolved globals (heap + vtable). Note: with the embedded GameWorld instance (1.0.68)
//     the value read is the vtable (inside the module), so 7b flags it as "suspicious"
//     (confidence 0.2) even though it is correct.
void PatternOrchestrator::RunPhase7_GlobalPointers() {
    auto start = Clock::now();

    spdlog::info("Orchestrator Phase 7: Global pointer discovery + validation");

    uintptr_t moduleBase = m_scanner.GetBase();
    size_t moduleSize = m_scanner.GetSize();

    // ES: ¿Parece válido el global? (valor 0 aceptado solo si allowNull; si no, puntero de
    //     heap cuya vtable apunte al módulo). Nota: esta lambda no se usa en la función.
    // Helper: check if a .data address looks like a valid global singleton pointer.
    // During init the value may be 0 (game not loaded) — accept 0 as tentative.
    // After game load, the value should be a heap-allocated object with a vtable.
    auto isValidGlobalPtr = [moduleBase, moduleSize](uintptr_t addr, bool allowNull) -> bool {
        if (addr == 0) return false;
        uintptr_t val = 0;
        if (!Memory::Read(addr, val)) return false;
        if (val == 0) return allowNull; // Null = game not loaded; accept if caller allows
        if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) return false;
        if (val == 0xFFFFFFFFFFFFFFFF || val == 0xCCCCCCCCCCCCCCCC ||
            val == 0xCDCDCDCDCDCDCDCD) return false;
        // Must be outside module image (heap-allocated object, not a function pointer)
        if (val >= moduleBase && val < moduleBase + moduleSize) return false;
        // Double-deref: a real singleton should point to a readable object (vtable)
        uintptr_t vtable = 0;
        if (!Memory::Read(val, vtable)) return false;
        // vtable should point into the module (likely .rdata vtable area or .text)
        if (vtable < moduleBase || vtable >= moduleBase + moduleSize) return false;
        return true;
    };

    // ES: Fase 7a: descubrimiento de globales vía xref de strings.
    // ── Phase 7a: Discover unresolved global pointers via string xref ──
    // For global pointer entries that have a stringAnchor but weren't resolved
    // by hardcoded RVA (Phase 5), find the function referencing the string,
    // then scan its code for MOV/LEA reg,[RIP+disp32] pointing to .data.
    for (auto& entry : m_entries) {
        if (!entry.isGlobalPointer || entry.isResolved) continue;
        if (!entry.stringAnchor || entry.stringAnchorLen <= 0) continue;

        spdlog::info("  Global '{}': attempting string-xref discovery via '{}'",
                     entry.id, std::string(entry.stringAnchor, entry.stringAnchorLen));

        // Find functions referencing this string
        std::string searchStr(entry.stringAnchor, entry.stringAnchorLen);
        auto funcsReferencing = m_strings.FindFunctionsReferencingString(searchStr);
        if (funcsReferencing.empty()) {
            spdlog::warn("  Global '{}': no functions reference anchor string", entry.id);
            continue;
        }

        // For each referencing function, scan its code for data section loads
        bool found = false;
        for (uintptr_t funcAddr : funcsReferencing) {
            if (found) break;

            // Get function end from .pdata
            uintptr_t funcEnd = funcAddr + 4096; // Default scan 4KB
            auto* pdataEntry = m_pdata.FindFunction(funcAddr);
            if (pdataEntry) {
                funcEnd = pdataEntry->endVA;
                // But cap at reasonable max to avoid scanning too far
                if (funcEnd - funcAddr > 16384) funcEnd = funcAddr + 16384;
            }

            // Scan for RIP-relative MOV/LEA into .data sections
            const uint8_t* code = reinterpret_cast<const uint8_t*>(funcAddr);
            size_t codeLen = funcEnd - funcAddr;
            const auto* dataSection = m_scanner.FindSection(".data");

            // SEH scan is done in a separate plain function (SEH_ScanCodeForGlobalPtr)
            // to avoid MSVC C2712 (can't use __try with object unwinding in this function)
            uintptr_t discoveredTarget = 0;
            uintptr_t discoveredInstr = 0;
            uintptr_t dsBase = dataSection ? dataSection->base : 0;
            size_t dsSize = dataSection ? dataSection->size : 0;
            const auto* textSection = m_scanner.FindSection(".text");
            const auto* rdataSection = m_scanner.FindSection(".rdata");
            uintptr_t tsBase = textSection ? textSection->base : 0;
            size_t tsSize = textSection ? textSection->size : 0;
            uintptr_t rsBase = rdataSection ? rdataSection->base : 0;
            size_t rsSize = rdataSection ? rdataSection->size : 0;
            if (SEH_ScanCodeForGlobalPtr(code, codeLen, funcAddr, dsBase, dsSize,
                                          tsBase, tsSize, rsBase, rsSize,
                                          moduleBase, moduleSize,
                                          discoveredTarget, discoveredInstr)) {
                uintptr_t val = 0;
                Memory::Read(discoveredTarget, val);
                spdlog::info("  Global '{}': DISCOVERED at 0x{:X} via func 0x{:X} "
                             "(instr 0x{:X}, val=0x{:X})",
                             entry.id, discoveredTarget, funcAddr, discoveredInstr, val);
                ResolveEntry(entry, discoveredTarget, ResolutionMethod::StringXref,
                             val == 0 ? 0.6f : 0.9f);
                found = true;
            }
        }

        if (!found) {
            spdlog::warn("  Global '{}': string-xref discovery failed ({} candidate functions checked)",
                         entry.id, funcsReferencing.size());
        }
    }

    // ES: Fase 7b: validación de los globales ya resueltos (ajusta la confianza).
    // ── Phase 7b: Validate existing global pointers ──
    for (auto& entry : m_entries) {
        if (!entry.isGlobalPointer || !entry.isResolved) continue;

        uintptr_t val = 0;
        if (Memory::Read(entry.resolvedAddress, val)) {
            if (val > 0x10000 && val < 0x00007FFFFFFFFFFF &&
                !(val >= moduleBase && val < moduleBase + moduleSize)) {
                uintptr_t vtable = 0;
                if (Memory::Read(val, vtable) && vtable >= moduleBase &&
                    vtable < moduleBase + moduleSize) {
                    entry.confidence = 1.0f;
                    spdlog::info("  Global '{}' validated: 0x{:X} -> 0x{:X} (vtable 0x{:X})",
                                 entry.id, entry.resolvedAddress, val, vtable);
                } else {
                    entry.confidence = 0.7f;
                    spdlog::info("  Global '{}' at 0x{:X} -> 0x{:X} (vtable check inconclusive)",
                                 entry.id, entry.resolvedAddress, val);
                }
            } else if (val == 0) {
                entry.confidence = 0.5f;
                spdlog::warn("  Global '{}' at 0x{:X} is null (game not loaded yet)",
                             entry.id, entry.resolvedAddress);
            } else {
                spdlog::warn("  Global '{}' at 0x{:X} reads 0x{:X} — suspicious (in-module or invalid range)",
                             entry.id, entry.resolvedAddress, val);
                entry.confidence = 0.2f;
            }
        }
    }

    m_lastReport.timing.globalPtrs = ElapsedMs(start);
}

// ES: Fase 8: emergencia solo para entradas CRÍTICAS aún sin resolver: búsqueda directa
//     del string y, si falla, RVA fija validada por prólogo. Si tampoco, su hook no se instala.
// EN: Phase 8: emergency only for CRITICAL entries still unresolved: direct string search
//     and, if that fails, prologue-validated fixed RVA. Otherwise its hook is not installed.
void PatternOrchestrator::RunPhase8_EmergencyCritical() {
    // Emergency resolution for critical patterns that survived all 7 phases unresolved.
    // Uses aggressive fallbacks that are slower but more portable:
    //   1. Direct string search in .rdata + LEA xref scan in .text
    //   2. Prologue-validated hardcoded RVA acceptance

    int unresolvedCritical = 0;
    for (const auto& entry : m_entries) {
        if (!entry.isResolved && entry.critical) unresolvedCritical++;
    }

    if (unresolvedCritical == 0) return;

    spdlog::warn("Orchestrator Phase 8: EMERGENCY — {} critical entries unresolved", unresolvedCritical);

    int resolved = 0;
    for (auto& entry : m_entries) {
        if (entry.isResolved || !entry.critical) continue;

        spdlog::warn("  Emergency resolution for CRITICAL entry '{}'", entry.id);

        // Attempt 1: Direct brute-force string search + LEA scan
        if (TryDirectStringSearch(entry)) {
            resolved++;
            continue;
        }

        // Attempt 2: Prologue-validated hardcoded RVA
        if (TryPrologueValidatedRVA(entry)) {
            resolved++;
            continue;
        }

        spdlog::error("  CRITICAL ENTRY '{}' UNRESOLVED — hook will NOT be installed", entry.id);
    }

    if (resolved > 0) {
        spdlog::info("  Phase 8 emergency resolved {} critical entries", resolved);
    }
}

// ES: Pipeline completo.
// ═══════════════════════════════════════════════════════════════════════════
//  FULL PIPELINE
// ═══════════════════════════════════════════════════════════════════════════

// ES: Ejecuta las 8 fases en orden, calcula el informe final (resueltas/fallidas) y lo loguea.
// EN: Runs the 8 phases in order, builds the final report (resolved/failed) and logs it.
OrchestratorReport PatternOrchestrator::Run() {
    if (!m_initialized) {
        spdlog::error("Orchestrator: Not initialized — call Init() first");
        return {};
    }

    auto totalStart = Clock::now();
    m_lastReport = {};
    m_lastReport.totalEntries = static_cast<int>(m_entries.size());

    spdlog::info("════════════════════════════════════════════════════════════");
    spdlog::info("  Pattern Orchestrator — Starting {} entries", m_entries.size());
    spdlog::info("════════════════════════════════════════════════════════════");

    RunPhase1_PData();
    RunPhase2_Strings();
    RunPhase3_VTables();
    RunPhase4_PatternScan();
    RunPhase5_StringFallback();
    RunPhase6_CallGraph();
    RunPhase7_GlobalPointers();
    RunPhase8_EmergencyCritical();

    // Compile final stats
    m_lastReport.totalResolved = CountResolved();
    m_lastReport.totalFailed = m_lastReport.totalEntries - m_lastReport.totalResolved;

    for (const auto& entry : m_entries) {
        if (!entry.isResolved) {
            m_lastReport.failedEntries.push_back(entry.id);
        }
    }

    m_lastReport.timing.total = ElapsedMs(totalStart);

    LogReport(m_lastReport);
    return m_lastReport;
}

// ES: Reintentos.
// ═══════════════════════════════════════════════════════════════════════════
//  RETRY
// ═══════════════════════════════════════════════════════════════════════════

// ES: Reintenta todas las entradas sin resolver (hasta maxRetries por entrada) con todos
//     los métodos. Útil tras cargar partida.
// EN: Retries every unresolved entry (up to maxRetries each) with all methods. Useful
//     after a game load.
int PatternOrchestrator::RetryFailed() {
    int resolved = 0;
    for (auto& entry : m_entries) {
        if (entry.isResolved) continue;
        if (entry.retryCount >= m_config.maxRetries) continue;
        entry.retryCount++;

        if (TryPatternScan(entry) || TryStringXref(entry) ||
            TryVTableSlot(entry) || TryHardcodedOffset(entry) ||
            TryComplexPattern(entry) || TryCallGraphTrace(entry)) {
            resolved++;
        }
    }
    spdlog::info("Orchestrator: Retry resolved {} additional entries", resolved);
    return resolved;
}

// ES: Reintenta una entrada concreta por id.
// EN: Retries a single entry by id.
bool PatternOrchestrator::RetryEntry(const std::string& id) {
    auto* entry = GetMutableEntry(id);
    if (!entry || entry->isResolved) return false;

    entry->retryCount++;
    return TryPatternScan(*entry) || TryStringXref(*entry) ||
           TryVTableSlot(*entry) || TryHardcodedOffset(*entry) ||
           TryComplexPattern(*entry) || TryCallGraphTrace(*entry);
}

// ES: API de consultas.
// ═══════════════════════════════════════════════════════════════════════════
//  QUERY API
// ═══════════════════════════════════════════════════════════════════════════

// ES: Dirección, estado, método y confianza por id; número de entradas resueltas.
// EN: Address, state, method and confidence by id; number of resolved entries.
uintptr_t PatternOrchestrator::GetAddress(const std::string& id) const {
    auto* entry = GetEntry(id);
    return entry ? entry->resolvedAddress : 0;
}

bool PatternOrchestrator::IsResolved(const std::string& id) const {
    auto* entry = GetEntry(id);
    return entry && entry->isResolved;
}

ResolutionMethod PatternOrchestrator::GetMethod(const std::string& id) const {
    auto* entry = GetEntry(id);
    return entry ? entry->resolvedMethod : ResolutionMethod::None;
}

float PatternOrchestrator::GetConfidence(const std::string& id) const {
    auto* entry = GetEntry(id);
    return entry ? entry->confidence : 0.0f;
}

int PatternOrchestrator::CountResolved() const {
    int count = 0;
    for (const auto& entry : m_entries) {
        if (entry.isResolved) count++;
    }
    return count;
}

// ES: Funciones etiquetadas por el análisis de strings.
// EN: Functions labeled by the string analysis.
const std::vector<LabeledFunction>& PatternOrchestrator::GetDiscoveredFunctions() const {
    return m_strings.GetLabeledFunctions();
}

// ES: Busca una función por nombre: entradas registradas, luego etiquetas de strings y
//     por último el slot 0 de la vtable de una clase con ese nombre.
// EN: Finds a function by name: registered entries, then string labels and finally slot 0
//     of the vtable of a class with that name.
uintptr_t PatternOrchestrator::FindFunction(const std::string& name) const {
    // Check registered entries first
    auto* entry = GetEntry(name);
    if (entry && entry->isResolved) return entry->resolvedAddress;

    // Check string analyzer
    auto* labeled = m_strings.FindLabeledFunction(name);
    if (labeled) return labeled->address;

    // Check vtables
    auto* vt = m_vtables.FindByClassName(name);
    if (vt && !vt->slots.empty()) return vt->slots[0].funcAddress;

    return 0;
}

// ES: Strings que referencia una función.
// EN: Strings referenced by a function.
std::vector<std::string> PatternOrchestrator::GetFunctionStrings(uintptr_t addr) const {
    return m_strings.GetFunctionStrings(addr);
}

// ES: Informes.
// ═══════════════════════════════════════════════════════════════════════════
//  REPORTING
// ═══════════════════════════════════════════════════════════════════════════

// ES: Último informe generado / volcado del informe al log.
// EN: Last generated report / report dump to the log.
OrchestratorReport PatternOrchestrator::GenerateReport() const {
    return m_lastReport;
}

void PatternOrchestrator::LogReport(const OrchestratorReport& report) const {
    spdlog::info("════════════════════════════════════════════════════════════");
    spdlog::info("  Pattern Orchestrator — Final Report");
    spdlog::info("════════════════════════════════════════════════════════════");
    spdlog::info("  Resolution: {}/{} ({:.1f}%)",
                 report.totalResolved, report.totalEntries,
                 report.totalEntries > 0
                     ? 100.0f * report.totalResolved / report.totalEntries
                     : 0.0f);
    spdlog::info("    Pattern scan:  {}", report.resolvedByPattern);
    spdlog::info("    String xref:   {}", report.resolvedByString);
    spdlog::info("    VTable slot:   {}", report.resolvedByVTable);
    spdlog::info("    Call graph:    {}", report.resolvedByCallGraph);
    spdlog::info("    Hardcoded RVA: {}", report.resolvedByHardcoded);
    spdlog::info("    Complex:       {}", report.resolvedByComplex);
    spdlog::info("  Discovery:");
    spdlog::info("    .pdata funcs:  {}", report.pdataFunctions);
    spdlog::info("    Strings:       {}", report.stringsFound);
    spdlog::info("    Xrefs:         {}", report.xrefsResolved);
    spdlog::info("    VTables:       {}", report.vtablesFound);
    spdlog::info("    Call graph:    {} nodes, {} edges", report.callGraphNodes, report.callGraphEdges);
    spdlog::info("    Labeled funcs: {}", report.labeledFunctions);
    spdlog::info("  Timing:");
    spdlog::info("    .pdata:        {:.1f}ms", report.timing.pdata);
    spdlog::info("    Strings:       {:.1f}ms", report.timing.strings);
    spdlog::info("    VTables:       {:.1f}ms", report.timing.vtables);
    spdlog::info("    Pattern scan:  {:.1f}ms", report.timing.patternScan);
    spdlog::info("    String fb:     {:.1f}ms", report.timing.stringFallback);
    spdlog::info("    Call graph:    {:.1f}ms", report.timing.callGraph);
    spdlog::info("    Global ptrs:   {:.1f}ms", report.timing.globalPtrs);
    spdlog::info("    TOTAL:         {:.1f}ms", report.timing.total);

    if (!report.failedEntries.empty()) {
        spdlog::warn("  Failed ({}):", report.failedEntries.size());
        for (const auto& id : report.failedEntries) {
            spdlog::warn("    - {}", id);
        }
    }

    spdlog::info("════════════════════════════════════════════════════════════");
}

} // namespace kmp
