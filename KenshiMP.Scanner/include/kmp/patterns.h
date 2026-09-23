// ES: patterns.h — catálogo de firmas del juego y estructura de direcciones resueltas.
//     Contiene: (1) los patrones AOB de cada función de kenshi_x64.exe que el mod hookea o
//     llama (firma de bytes con comodines '?' que se busca en .text para no depender de
//     una dirección fija), con su RVA de referencia (dirección relativa a la base del
//     módulo en 1.0.68) y el string con el que se encontró; (2) cadenas de punteros de
//     Cheat Engine; (3) strings ancla para el escáner de respaldo; (4) GameFunctions, la
//     struct que rellenan el orquestador (camino A) y ResolveGameFunctions (camino B) y que
//     usa el Core para instalar hooks y llamar funciones del juego.
//     Notación: un prólogo "48 8B C4" (mov rax, rsp) implica que el HookManager aplicará el
//     fix MovRaxRsp; "40 55 ..."/"48 89 5C 24 ..." son prólogos normales.
// EN: patterns.h — catalog of game signatures and the resolved-address struct.
//     It contains: (1) the AOB patterns of each kenshi_x64.exe function the mod hooks or
//     calls (byte signature with '?' wildcards searched in .text so we do not depend on a
//     fixed address), with its reference RVA (address relative to the module base in
//     1.0.68) and the string it was found through; (2) Cheat Engine pointer chains;
//     (3) anchor strings for the fallback scanner; (4) GameFunctions, the struct filled by
//     the orchestrator (path A) and ResolveGameFunctions (path B) and used by Core to
//     install hooks and call game functions.
//     Notation: a "48 8B C4" (mov rax, rsp) prologue means the HookManager will apply the
//     MovRaxRsp fix; "40 55 ..."/"48 89 5C 24 ..." are regular prologues.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <string>

namespace kmp {

// ES: Firmas conocidas de funciones de Kenshi en notación IDA ('?'/'??' = comodín). Los
//     comodines tapan bytes que cambian entre versiones (desplazamientos LEA, tamaños de
//     pila, destinos de call, inmediatos). Se descubrieron con límites de .pdata, xrefs
//     de strings y escaneo binario sobre la v1.0.68. Nota: el comentario inglés dice que el
//     escáner de strings es el resolvedor PRINCIPAL, pero ResolveGameFunctions prueba
//     primero el patrón y usa los strings como respaldo (ver patterns.cpp).
// Known pattern signatures for Kenshi functions.
// Patterns use IDA notation: hex bytes with '?' or '??' for wildcard.
// Wildcards cover operand bytes that change between game versions
// (LEA offsets, stack sizes, call targets, immediate values).
//
// All patterns were discovered from kenshi_x64.exe using .pdata-based
// function boundary detection, string xref analysis, and binary scanning.
// The runtime string scanner (RuntimeStringScanner) is the PRIMARY resolver
// for critical functions; patterns serve as secondary validation/fallback.
//
// Developed against v1.0.68; wildcards added for cross-version portability.

// ES: Detecta la versión del juego leyendo el recurso de versión del PE ("1.0.68.0" o "unknown").
// Detect the game version from the PE file version resource.
// Returns version string like "1.0.68.0" or "unknown" on failure.
std::string DetectGameVersion();

// ES: Espacio de nombres con las constantes de patrones, cadenas y anclas.
// EN: Namespace holding the pattern, chain and anchor constants.
namespace patterns {

// ES: IDXGISwapChain::Present (overlay D3D11) se obtiene por vtable, no por patrón.
// ── D3D11/DXGI (for overlay) ──
// IDXGISwapChain::Present is found via vtable, not pattern.

// ES: Ciclo de vida de entidades.
// ═══════════════════════════════════════════════════════════════════════════
//  ENTITY LIFECYCLE
// ═══════════════════════════════════════════════════════════════════════════

// ES: RootObjectFactory::process — crea personajes a partir de peticiones de la fábrica
//     del juego. Encontrada por el string "[RootObjectFactory::process] Character '".
//     RVA 0x581770. Prólogo mov rax,rsp (fix MovRaxRsp). Comodines en los dos disp32
//     (desplazamiento de lea rbp y tamaño de pila), que dependen del tamaño del marco.
//     El mod la hookea (entity_hooks) para detectar spawns y sincronizarlos por red. Crítica.
// RootObjectFactory::process - processes character creation
// Found via "[RootObjectFactory::process] Character '"
// RVA: 0x00581770
// Wildcards on the two disp32 operands (LEA-rbp displacement + stack-alloc size) per
// opcode-stability audit — those bytes are compiler-frame-size-dependent tier-C.
constexpr const char* CHARACTER_SPAWN = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 48 C7 45 C0";

// ES: Ruta de destrucción de nodos/personajes (NodeList). Encontrada por
//     "NodeList::destroyNodesByBuilding". RVA 0x38A720. Prólogo mov rax,rsp.
//     El mod la hookea (entity_hooks) para enterarse de personajes destruidos.
// NodeList destroy / character cleanup path
// Found via "NodeList::destroyNodesByBuilding"
// RVA: 0x0038A720
constexpr const char* CHARACTER_DESTROY = "48 8B C4 44 88 40 18 48 89 50 10 48 89 48 08 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 48 48";

// ES: RootObjectFactory::createRandomSquad — crea un escuadrón y sus miembros. Encontrada por
//     "[RootObjectFactory::createRandomSquad] Missing squad leader". RVA 0x583A10.
// RootObjectFactory::createRandomSquad - squad+character batch spawning
// Found via "[RootObjectFactory::createRandomSquad] Missing squad leader"
// RVA: 0x00583A10
constexpr const char* CREATE_RANDOM_SQUAD = "48 8B C4 55 53 56 57 48 8D A8 F8 FD FF FF 48 81 EC E8 02 00 00 48 C7 45 40 FE FF FF FF 0F 29 70";

// ES: Character::serialise — guarda/carga los datos de un personaje. Encontrada por
//     "[Character::serialise] Character '". RVA 0x6280A0. Prólogo normal (40 55).
// Character serialise (save/load character data)
// Found via "[Character::serialise] Character '"
// RVA: 0x006280A0
constexpr const char* CHARACTER_SERIALISE = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 38 FF FF FF 48 81 EC C8 01 00 00 48 C7 45 C0";

// ES: Manejador de noqueo (KO) de personaje. Encontrada por el string genérico "knockout".
//     RVA 0x345C10. Comodines en offsets de guardado en pila y en 4 bytes intermedios.
//     La usa combat_hooks para sincronizar KOs.
// Character knockout
// Found via "knockout"
// RVA: 0x00345C10
constexpr const char* CHARACTER_KO = "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B 02 48 8B E9 ? ? ? ? 8B FA";

// ES: Movimiento.
// ═══════════════════════════════════════════════════════════════════════════
//  MOVEMENT
// ═══════════════════════════════════════════════════════════════════════════

// ES: HavokCharacter::setPosition — teletransporta/coloca un personaje (física Havok).
//     Encontrada por "HavokCharacter::setPosition moved someone off the world". RVA 0x145E50.
//     El mod la usa (movement_hooks) para mover personajes remotos a su posición de red.
// HavokCharacter::setPosition
// Found via "HavokCharacter::setPosition moved someone off the world"
// RVA: 0x00145E50
constexpr const char* CHARACTER_SET_POSITION = "48 8B C4 55 57 41 54 48 8D 68 C8 48 81 EC 20 01 00 00 48 C7 44 24 30 FE FF FF FF 48 89 58 18 48";

// ES: Orden de movimiento del jugador (pathfinding). RVA 0x2EF4E3 (no alineada: no parece
//     un inicio de función). DESACTIVADO: el patrón contenía un call indirecto por la IAT
//     (FF 15 + disp32 propio de cada binario) imposible de comodinar sin perder unicidad,
//     y además el hook está desactivado (mov rax,rsp + 5 parámetros: el parámetro en pila
//     no se puede reenviar a través del detour desnudo MovRaxRsp).
// Player move command (pathfinding)
// Found via "pathfind"
// RVA: 0x002EF4E3
// DISABLED — pattern contained a RIP-relative IAT indirect call (FF 15 6C 55 F5 01) whose
// 4-byte displacement is a per-binary IAT offset, unwildcardable without losing all
// uniqueness. CharacterMoveTo is also hook-disabled at the hook layer (mov rax,rsp +
// 5 params — stack param can't be forwarded through the MovRaxRsp naked detour).
constexpr const char* CHARACTER_MOVE_TO = nullptr;

// ES: Combate.
// ═══════════════════════════════════════════════════════════════════════════
//  COMBAT
// ═══════════════════════════════════════════════════════════════════════════

// ES: Manejador del efecto de daño de un ataque. Encontrada por "Attack damage effect".
//     RVA 0x7A33A0. La usa combat_hooks para sincronizar daño.
// Attack damage effect handler
// Found via "Attack damage effect"
// RVA: 0x007A33A0
constexpr const char* APPLY_DAMAGE = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 68 FC FF FF 48 81 EC 60 04 00 00 48 C7 45 98";

// ES: Cálculo de daño cortante/contundente (StartAttack). Encontrada por "Cutting damage" /
//     "Blunt damage". RVA 0x7B2A20.
// Cut/blunt damage calculation
// Found via "Cutting damage" / "Blunt damage"
// RVA: 0x007B2A20
constexpr const char* START_ATTACK = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 08 FC FF FF 48 81 EC C0 04 00 00 48 C7 44 24";

// ES: Muerte por pérdida de sangre / hambre. Encontrada por "{1} has died from blood loss.".
//     RVA 0x7A6200. La usa combat_hooks para sincronizar muertes.
// Death from blood loss / starvation handler
// Found via "{1} has died from blood loss."
// RVA: 0x007A6200
constexpr const char* CHARACTER_DEATH = "48 8B C4 55 57 41 54 41 55 41 56 48 8D A8 28 FE FF FF 48 81 EC B0 02 00 00 48 C7 44 24 28 FE FF";

// ES: Resolución de daño del sistema de salud/sangre + probabilidad de bloqueo.
//     Encontrada por "block chance" y "damage resistance max". RVA 0x86B2B0.
// Health/blood system - combat damage resolution + block chance
// Found via "block chance" and "damage resistance max"
// RVA: 0x0086B2B0
constexpr const char* HEALTH_UPDATE = "48 8B C4 55 57 41 54 41 55 41 56 48 8D 68 A1 48 81 EC F0 00 00 00 48 C7 44 24 28 FE FF FF FF 48";

// ES: Cálculo del modificador de daño cortante. Encontrada por "cut damage mod". RVA 0x889CD0.
// Cut damage modifier calculation
// Found via "cut damage mod"
// RVA: 0x00889CD0
constexpr const char* CUT_DAMAGE_MOD = "40 55 53 56 57 41 54 41 55 41 56 48 8B EC 48 83 EC 70 48 C7 45 B0 FE FF FF FF 0F 29 74 24 60 48";

// ES: Bonificación de daño desarmado. Encontrada por "unarmed damage bonus". RVA 0x0CE2D0.
// Unarmed damage bonus
// Found via "unarmed damage bonus"
// RVA: 0x000CE2D0
constexpr const char* UNARMED_DAMAGE = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC B0 00 00 00 48 C7 45 BF FE FF FF FF";

// ES: Manejador completo de combate de artes marciales. Encontrada por "Martial Arts". RVA 0x892120.
// Martial Arts full combat handler
// Found via "Martial Arts"
// RVA: 0x00892120
constexpr const char* MARTIAL_ARTS_COMBAT = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 B8 FB FF FF 48 81 EC 10 05 00 00 48 C7 45 98";

// ── issueOrder / setJobTarget — EJECUTOR REAL de la orden del jugador ──
// ⚠ NO CONFIRMADA — 0x722EF0 NO es la orden del jugador (REFUTADO por RE, 2026-06-18).
// La doble verificación de bytes demostró que 0x722EF0 construye un MyGUI::UString y aloca
// un Ogre::AllocatedObject (imports MyGUIEngine_x64.dll / OgreMain_x64.dll): es una función
// de UI/GUI (tooltip/label/feedback de orden), NO la que encola un Job de IA. Sus callers
// (0x6714A5, 0x6719A8) pasan un manager de UI global [0x21337D0], no un Character.
// La orden REAL del jugador (clic→Task) entra por un método del vtable de Tasker/GOAPTaskMgr
// que empuja un Task_* (Task_MeleeAttack @RTTI 0x1CF7350, Task_GetUp @0x1CF7538,
// Task_GetOutOfBed @0x1CF76D8...) a la cola del char — RVA exacta SIN resolver todavía.
// Este AOB casa con esa función UI; se conserva solo como referencia histórica. NO usar
// como "IssueOrder": el hook DIAG de combate que lo usaba quedó deshabilitado (combat_hooks).
// Firma del prólogo: __fastcall(void* uiMgr /*rcx*/, void* obj /*rdx*/, bool variant /*r8b*/).
// EN: issueOrder / setJobTarget — NOT CONFIRMED. 0x722EF0 is NOT the player order (REFUTED
//     by RE, 2026-06-18): it builds a MyGUI::UString and allocates an Ogre::AllocatedObject,
//     so it is a UI function (order tooltip/label/feedback), not the one queueing an AI Job.
//     Its callers (0x6714A5, 0x6719A8) pass a global UI manager [0x21337D0], not a Character.
//     The REAL player order (click -> Task) goes through a Tasker/GOAPTaskMgr vtable method
//     that pushes a Task_* (Task_MeleeAttack RTTI 0x1CF7350, Task_GetUp 0x1CF7538,
//     Task_GetOutOfBed 0x1CF76D8...) into the character queue; exact RVA still unresolved.
//     This AOB matches that UI function and is kept only as historical reference. Do NOT use
//     it as "IssueOrder": the combat DIAG hook that used it was disabled (combat_hooks).
//     Prologue signature: __fastcall(void* uiMgr /*rcx*/, void* obj /*rdx*/, bool variant /*r8b*/).
constexpr const char* ISSUE_ORDER = "48 8B C4 57 48 81 EC B0 00 00 00 48 C7 44 24 28 FE FF FF FF 48 89 58 18 48 89 70 20";

// Character::addOrder — backend "normal/replace" VALIDADOR de órdenes (RVA 0x5D1940).
// RE 2026-07-11/12: valida la orden ANTES de encolarla. Si el chequeo falla, muestra el
// globo nativo ("My arm is broken!" .rdata 0x16232E8 / "I can't carry anyone with this
// arm." 0x16232B8) y devuelve TRUE (orden "manejada") SIN encolar nada → la orden se
// traga en silencio. Si pasa, devuelve FALSE y la orden sigue (Task → cola en 0x508380).
// Ramas: TaskType∈{4,5,0x10,0x15} → canUseArms 0x644150 (char+0x5BD/+0x5BE);
//        TaskType∈{0x69,0x44,0xD5,0xAA,0x94,0xE1} → [[AI+0x318]+0x166], AI=*(char+0x650).
// Prólogo limpio (40 55 56 57, SIN mov rax,rsp). El AOB corto de 18 bytes daba 2 matches
// (0x5D1940 y 0x7E5F80); este de 32 bytes es ÚNICO en .text (verificado estático 1.0.68).
// Firma verificada (0 usos de r9 ni args de pila en toda la función, retorno bool en al):
//   bool __fastcall(Character* me /*rcx*/, int taskType /*edx*/, void* subject /*r8*/)
// EN: Character::addOrder — "normal/replace" order VALIDATOR backend (RVA 0x5D1940).
//     RE 2026-07-11/12: validates the order BEFORE queueing it. If the check fails it shows
//     the native speech bubble ("My arm is broken!" .rdata 0x16232E8 / "I can't carry anyone
//     with this arm." 0x16232B8) and returns TRUE (order "handled") WITHOUT queueing, so the
//     order is silently swallowed. If it passes it returns FALSE and the order goes on
//     (Task -> queue at 0x508380). Branches: TaskType in {4,5,0x10,0x15} -> canUseArms
//     0x644150 (char+0x5BD/+0x5BE); TaskType in {0x69,0x44,0xD5,0xAA,0x94,0xE1} ->
//     [[AI+0x318]+0x166], AI=*(char+0x650). Clean prologue (40 55 56 57, NO mov rax,rsp).
//     The short 18-byte AOB gave 2 matches (0x5D1940 and 0x7E5F80); this 32-byte one is
//     UNIQUE in .text (statically verified on 1.0.68). Verified signature (no r9 or stack
//     args used anywhere, bool return in al):
//       bool __fastcall(Character* me /*rcx*/, int taskType /*edx*/, void* subject /*r8*/)
//     Used by combat_hooks (SafeCall_Bool_PtrIPtr in safe_hook.h).
constexpr const char* ADD_ORDER_BACKEND = "40 55 56 57 48 8D AC 24 60 FE FF FF 48 81 EC A0 02 00 00 48 C7 44 24 20 FE FF FF FF 48 89 9C 24";

// NOTA (2026-07-15): aquí había un patrón COMBAT_TARGET_RESOLVE para la RVA 0x0074A630,
// creído "selector de objetivo de combate". La RE posterior (3 strings internos de comercio
// + xref al literal "buyItem" + análisis del caller que referencia 'Sell_Item') confirmó que
// 0x74A630 es BuyItem (rutina de IA de COMERCIO). Ese AOB era un SUPERCONJUNTO del de BUY_ITEM
// que casaba en la MISMA dirección → duplicado innecesario. Eliminado. El hook de esa RVA
// (inventory_hooks.cpp: Hook_BuyItem, sync de red + guard UAF fusionados) usa el patrón BUY_ITEM
// de más abajo vía funcs.BuyItem. Ver la nota ampliada en el patrón BUY_ITEM (sección INVENTORY / ITEMS).
// EN: NOTE (2026-07-15): there used to be a COMBAT_TARGET_RESOLVE pattern here for RVA
//     0x0074A630, believed to be a "combat target selector". Later RE (3 internal trade
//     strings + xref to the "buyItem" literal + analysis of the caller referencing
//     'Sell_Item') confirmed 0x74A630 is BuyItem (AI TRADE routine). That AOB was a
//     SUPERSET of BUY_ITEM matching the SAME address, an unnecessary duplicate, so it was
//     removed. The hook on that RVA (inventory_hooks.cpp: Hook_BuyItem, network sync + UAF
//     guard merged) uses the BUY_ITEM pattern below via funcs.BuyItem.

// ES: Mundo / zonas.
// ═══════════════════════════════════════════════════════════════════════════
//  WORLD / ZONES
// ═══════════════════════════════════════════════════════════════════════════

// ES: Carga de zona (Kenshi divide el mapa en zonas). Encontrada por "zone.%d.%d.zone".
//     RVA 0x377710. Comodín en el tamaño de pila. La usa world_hooks para sincronizar zonas.
// Zone load function
// Found via "zone.%d.%d.zone"
// RVA: 0x00377710
constexpr const char* ZONE_LOAD = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 E0 FC FF FF 48 81 EC ? ? ? ? 48 C7 45 88 FE";

// ES: Descarga de zona / desmontaje del navmesh. Encontrada por "destroyed navmesh" (primer
//     xref). RVA 0x2EF1F0. La usa world_hooks.
// Zone unload / navmesh teardown
// Found via "destroyed navmesh" (first xref)
// RVA: 0x002EF1F0
constexpr const char* ZONE_UNLOAD = "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 68 88 48 81 EC 50 01 00 00 48 C7 44 24 50 FE FF FF FF";

// ES: Colocación/creación de edificios. Encontrada por
//     "[RootObjectFactory::createBuilding] Building '". RVA 0x57CC70. La usa building_hooks.
// Building placement / creation
// Found via "[RootObjectFactory::createBuilding] Building '"
// RVA: 0x0057CC70
constexpr const char* BUILDING_PLACE = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 F8 FB FF FF 48 81 EC D0 04 00 00 48 C7 45 F8";

// ES: Manejador de edificio destruido. Encontrada por "Building::setDestroyed". RVA 0x557280.
// Building destroyed handler
// Found via "Building::setDestroyed"
// RVA: 0x00557280
constexpr const char* BUILDING_DESTROYED = "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 C7 45 30 FE FF FF FF";

// ES: Sistema de navmesh. Encontrada por "navmesh". RVA 0x881950. Solo patrón (sin ancla).
// Navmesh system
// Found via "navmesh"
// RVA: 0x00881950
constexpr const char* NAVMESH = "48 89 54 24 ? 57 48 83 EC ? 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 ? 49 8B F8 48 8B DA 48 89";

// ES: Comprobación de spawn dentro de edificios. Encontrada por " tried to spawn inside walls!".
//     RVA 0x4FFAD0.
// Spawn in buildings check
// Found via " tried to spawn inside walls!"
// RVA: 0x004FFAD0
constexpr const char* SPAWN_CHECK = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 30 FF FF FF 48 81 EC D0 01 00 00 48 C7 45 C0 FE";

// ES: Bucle de juego / tiempo.
// ═══════════════════════════════════════════════════════════════════════════
//  GAME LOOP / TIME
// ═══════════════════════════════════════════════════════════════════════════

// ES: Actualización/arranque del frame de juego. Encontrada por "Kenshi 1.0.". RVA 0x123A10.
//     Ampliado a 45 bytes para que sea único (0x788100 comparte los 32 primeros).
//     La usa game_tick_hooks como tick principal del mod. Crítica.
// Game frame update / initialization
// Found via "Kenshi 1.0."
// RVA: 0x00123A10
// Extended to 45 tokens for uniqueness (false positive at 0x00788100 shares first 32 bytes)
constexpr const char* GAME_FRAME_UPDATE = "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 68 88 48 81 EC 50 01 00 00 48 C7 44 24 38 FE FF FF FF 48 89 58 10 48 89 70 18 48 89 78 20 48";

// ES: Manejador de la escala de tiempo (velocidad del juego). Encontrada por "timeScale".
//     RVA 0x214B50. Comodines en el destino del call y del salto condicional.
//     La usa time_hooks para sincronizar la velocidad. Crítica.
// Time scale handler - processes game speed changes
// Found via "timeScale"
// RVA: 0x00214B50
constexpr const char* TIME_UPDATE = "40 55 56 48 83 EC 28 48 8B F2 48 8B E9 BA 02 00 00 00 48 8B CE E8 ? ? ? ? 84 C0 0F 84 ? ?";

// ── GameWorld::setPaused — SETTER OFICIAL DE PAUSA ──
// Firma: void __fastcall(void* gameWorld /*rcx*/, bool paused /*dl*/)
// RVA: 0x00787D40 (Steam 1.0.68). Verificado byte a byte + análisis estático completo.
// Lógica: paused = paramPaused OR (gameSpeed==0). Escribe GameWorld+0x8B9, y ADEMÁS
// refresca los caches de pausa de 3 subsistemas (obj+0xB8), oculta el cartel "PAUSED"
// del HUD (updatePauseUI @0x6E20D0) y emite el evento "Resume_Game"/"Pause_Game".
//
// CRÍTICO: escribir GameWorld+0x8B9 a pelo (como hacía el mod) despausa SOLO la
// simulación pero NO refresca esos caches → los subsistemas siguen creyendo que está
// pausado → bloquean las órdenes del jugador (atacar/hablar) y el HUD muestra "PAUSED".
// Llamar a este setter en su lugar arregla la "pausa fantasma" de raíz.
//
// El byte wildcard es el disp32 RIP-relativo de la constante 0.0f (ucomiss xmm6,[rip+x]).
// 33 bytes fijos antes del wildcard → patrón ÚNICO en .text (verificado, 1 match).
// Prólogo normal (48 89 5C 24 08...), NO mov-rax-rsp → llamable directo sin el fix ASM.
// EN: GameWorld::setPaused — OFFICIAL PAUSE SETTER. Signature:
//     void __fastcall(void* gameWorld /*rcx*/, bool paused /*dl*/). RVA 0x00787D40 (Steam
//     1.0.68), verified byte by byte + full static analysis. Logic: paused = paramPaused OR
//     (gameSpeed==0). Writes GameWorld+0x8B9 and ALSO refreshes the pause caches of 3
//     subsystems (obj+0xB8), hides the HUD "PAUSED" banner (updatePauseUI 0x6E20D0) and fires
//     the "Resume_Game"/"Pause_Game" event. CRITICAL: writing GameWorld+0x8B9 directly (as
//     the mod used to) only unpauses the simulation but does NOT refresh those caches, so
//     subsystems still think it is paused, block player orders (attack/talk) and the HUD
//     shows "PAUSED". Calling this setter fixes the "ghost pause" at the root. The wildcard
//     is the RIP-relative disp32 of the 0.0f constant (ucomiss xmm6,[rip+x]); 33 fixed bytes
//     before it make the pattern UNIQUE in .text (verified, 1 match). Regular prologue
//     (48 89 5C 24 08...), NOT mov-rax-rsp, so it can be called directly without the ASM fix.
constexpr const char* SET_PAUSED = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 0F 29 74 24 40 F3 0F 10 B1 00 07 00 00 33 F6 0F 2E 35 ? ? ? ? 0F B6 DA 48 8B F9";

// ES: Guardar / cargar.
// ═══════════════════════════════════════════════════════════════════════════
//  SAVE / LOAD
// ═══════════════════════════════════════════════════════════════════════════

// ES: Guardar partida. Encontrada por "quicksave". RVA 0x7EF040. Comodín en el tamaño de pila.
// Save function
// Found via "quicksave"
// RVA: 0x007EF040
constexpr const char* SAVE_GAME = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F9 48 81 EC ? ? ? ? 48 C7 45 97 FE FF FF FF";

// ES: Cargar partida. Encontrada por "[SaveManager::loadGame] No towns loaded.". RVA 0x373F00.
// Load function
// Found via "[SaveManager::loadGame] No towns loaded."
// RVA: 0x00373F00
constexpr const char* LOAD_GAME = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 C0 FC FF FF 48 81 EC 40 04 00 00 48 C7 45 A8 FE";

// ES: Importar partida (nueva partida desde plantilla). Encontrada por
//     "[SaveManager::importGame] No towns loaded.". RVA 0x378A30.
// Import game (new game from template)
// Found via "[SaveManager::importGame] No towns loaded."
// RVA: 0x00378A30
constexpr const char* IMPORT_GAME = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 78 FC FF FF 48 81 EC 50 04 00 00 48 C7 45 C0";

// ES: Interfaz de estadísticas del personaje. Encontrada por "CharacterStats_Attributes".
//     RVA 0x8BA700. También se usa para descubrir el global PlayerBase por desensamblado.
// Character stats UI
// Found via "CharacterStats_Attributes"
// RVA: 0x008BA700
constexpr const char* CHARACTER_STATS = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 08 FF FF FF 48 81 EC C0 01 00 00 48 C7 44 24";

// ES: Entrada (teclado/ratón): no se usan patrones. Kenshi enruta la entrada por OIS/MyGUI
//     vía vtables; el mod la intercepta en el WndProc de Win32 (render_hooks.cpp).
// ═══════════════════════════════════════════════════════════════════════════
//  INPUT (handled via WndProc hook in render_hooks, patterns optional)
// ═══════════════════════════════════════════════════════════════════════════

// OIS keyboard/mouse input is handled via vtable dispatch, not hookable patterns.
// Kenshi routes input through MyGUI::InputManager::injectKeyPress/injectMouseMove.
// We intercept input at the Win32 WndProc level in render_hooks.cpp instead.
constexpr const char* INPUT_KEY_PRESSED = nullptr;
constexpr const char* INPUT_MOUSE_MOVED = nullptr;

// ES: Escuadrones / platoons.
// ═══════════════════════════════════════════════════════════════════════════
//  SQUAD / PLATOON MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

// ES: Reset de posiciones de escuadrón / gestión de escuadrón. Encontrada por
//     "Reset squad positions". RVA 0x480B50. La usa squad_hooks.
// Squad position reset / squad management entry
// Found via "Reset squad positions"
// RVA: 0x00480B50
constexpr const char* SQUAD_CREATE = "48 8B C4 55 41 54 41 55 48 8D A8 28 FF FF FF 48 81 EC C0 01 00 00 48 C7 44 24 48 FE FF FF FF 48";

// ES: Añadir miembro a escuadrón ("delayedSpawningChecks"). RVA 0x928423 (no alineada a 16).
//     DESACTIVADO: en Steam el patrón caía a mitad de función y el string llevaba a otra
//     función. El orquestador la resuelve por RTTI: clase "ActivePlatoon|Platoon|Squad",
//     slot 2 de la vtable. Dejar un patrón real arriesgaba que la autocorrección de
//     tryPattern pisara el resultado RTTI.
// Delayed spawning checks - processes new squad member additions
// Found via "delayedSpawningChecks"
// RVA: 0x00928423
// DISABLED — pattern matches mid-function on Steam (not at a .pdata function entry) and
// finds a different function via alternate string xref. Orchestrator resolves this via
// RTTI vtable lookup (vtableClass="ActivePlatoon|Platoon|Squad", vtableSlot=2). Leaving
// a real byte string risks the auto-correct path in tryPattern overriding the RTTI result.
constexpr const char* SQUAD_ADD_MEMBER = nullptr;

// ES: Inventario / objetos.
// ═══════════════════════════════════════════════════════════════════════════
//  INVENTORY / ITEMS
// ═══════════════════════════════════════════════════════════════════════════

// ES: Inventory addItem — añade un objeto al inventario de un personaje/contenedor.
//     Encontrada por "addItem" (genérico). RVA 0x74C8B0. La usa inventory_hooks.
// Inventory addItem - adds item to character/container inventory
// Found via "addItem"
// RVA: 0x0074C8B0
constexpr const char* ITEM_PICKUP = "48 8B C4 44 89 40 18 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 40 48 C7 45 E0 FE FF FF FF 48";

// ES: Inventory removeItem — quita un objeto del inventario. Encontrada por "removeItem".
//     RVA 0x745DE0. Comodín en un lea RIP-relativo. La usa inventory_hooks.
// Inventory removeItem - removes item from inventory
// Found via "removeItem"
// RVA: 0x00745DE0
constexpr const char* ITEM_DROP = "40 53 48 83 EC 20 48 8B 01 45 33 C9 FF 50 28 48 8B D8 48 85 C0 74 19 4C 8D 0D ? ? ? ? 48 8D";

// Buy item from shop — rutina de IA de COMERCIO (NPC comprando/vendiendo items)
// Found via "buyItem" (+ xref a 'Sell_Item' desde el caller)
// RVA: 0x0074A630
// Firma real (RE 1.0.68): void* __fastcall(void* buyer, void* item, void* seller). El retorno es
// un PUNTERO al item comprado (o nullptr si falla), NO `char` — declararlo `char` truncaba el
// puntero de 64 bits a 1 byte y causaba el crash game+0x9A18DA (confirmado por RE del epílogo y
// del caller nativo game+0x9A18B8, que desreferencia el resultado como objeto con vtable).
// UN SOLO hook sobre esta RVA (2026-07-14): inventory_hooks::Hook_BuyItem hace a la vez la
// sincronización de red de la compra (C2S_TradeRequest) Y el guard del use-after-free del
// retorno (validación de la vtable del puntero devuelto contra el rango de código del juego).
// Antes eran DOS hooks sobre la misma dirección (el guard vivía en combat_hooks.cpp), lo que
// MinHook rechazaba en silencio por deduplicar por dirección; se fusionaron en uno. El hook se
// instala sobre funcs.BuyItem (esta misma RVA); no hace falta un segundo patrón.
// EN: Buy item from shop — AI TRADE routine (NPC buying/selling items). Found via "buyItem"
//     (+ xref to 'Sell_Item' from the caller). RVA 0x0074A630. Real signature (RE 1.0.68):
//     void* __fastcall(void* buyer, void* item, void* seller). The return is a POINTER to the
//     bought item (or nullptr on failure), NOT `char`; declaring it `char` truncated the
//     64-bit pointer to 1 byte and caused the game+0x9A18DA crash (confirmed by RE of the
//     epilogue and of the native caller game+0x9A18B8, which dereferences the result as an
//     object with a vtable). ONE single hook on this RVA (2026-07-14):
//     inventory_hooks::Hook_BuyItem does both the network sync of the purchase
//     (C2S_TradeRequest) AND the use-after-free guard on the return (validates the returned
//     pointer's vtable against the game code range). There used to be TWO hooks on the same
//     address (the guard lived in combat_hooks.cpp), which MinHook silently rejected because
//     it deduplicates by address; they were merged. No second pattern needed.
constexpr const char* BUY_ITEM = "40 55 56 57 41 54 41 55 48 81 EC 00 01 00 00 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 48 01 00 00";

// ES: Facción / diplomacia.
// ═══════════════════════════════════════════════════════════════════════════
//  FACTION / DIPLOMACY
// ═══════════════════════════════════════════════════════════════════════════

// ES: Cambios de relación entre facciones. Encontrada por "faction relation". RVA 0x872E00.
// Faction relation handler - processes faction relation changes
// Found via "faction relation"
// RVA: 0x00872E00
constexpr const char* FACTION_RELATION = "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 58 FD FF FF 48 81 EC 80 03 00 00 48 C7 85 90 00 00";

// ES: Sistema de IA.
// ═══════════════════════════════════════════════════════════════════════════
//  AI SYSTEM
// ═══════════════════════════════════════════════════════════════════════════

// ES: AI::create — crea el controlador de IA de un personaje. Encontrada por
//     "[AI::create] No faction for". RVA 0x622110. Ampliado a 41 tokens para ser único
//     (0x0AF870 comparte los 29 primeros bytes fijos). Comodín en el lea de la security
//     cookie. La usa ai_hooks. Crítica.
// AI::create - creates AI controller for a character
// Found via "[AI::create] No faction for"
// RVA: 0x00622110
// Extended to 41 tokens for uniqueness (false positive at 0x000AF870 shares first 29 fixed bytes)
constexpr const char* AI_CREATE = "40 57 48 81 EC 90 00 00 00 48 C7 44 24 28 FE FF FF FF 48 89 9C 24 B0 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 80";

// ES: Cargador de paquetes de IA (comportamientos). Encontrada por "AI packages". RVA 0x271620.
//     La usa ai_hooks.
// AI packages loader - loads and assigns AI behavior packages
// Found via "AI packages"
// RVA: 0x00271620
constexpr const char* AI_PACKAGES = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC 00 01 00 00 48 C7 45 9F FE FF FF FF";

// ES: Torretas / combate a distancia.
// ═══════════════════════════════════════════════════════════════════════════
//  TURRET / RANGED COMBAT
// ═══════════════════════════════════════════════════════════════════════════

// ES: Manejador de uso de torreta. Encontrada por "gun turret". RVA 0x43B690.
// Gun turret operation handler
// Found via "gun turret"
// RVA: 0x0043B690
constexpr const char* GUN_TURRET = "48 8B C4 55 41 54 41 55 48 8D 68 A1 48 81 EC C0 00 00 00 48 C7 45 E7 FE FF FF FF 48 89 58 10 48";

// ES: Apuntado/disparo de torreta. Encontrada por "gun turret" (segunda función). RVA 0x43CDB0.
// Gun turret targeting/fire handler
// Found via "gun turret" (second function)
// RVA: 0x0043CDB0
constexpr const char* GUN_TURRET_FIRE = "40 55 53 56 57 41 54 41 55 48 8D AC 24 F8 FE FF FF 48 81 EC 08 02 00 00 48 C7 44 24 58 FE FF FF";


// ES: Gestión de edificios (desmontar, construir, reparar); las usa building_hooks.
// ═══════════════════════════════════════════════════════════════════════════
//  BUILDING MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

// ES: Desmontar edificio. Encontrada por "dismantle". RVA 0x2A2860.
// Building dismantle handler
// Found via "dismantle"
// RVA: 0x002A2860
constexpr const char* BUILDING_DISMANTLE = "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 38 FF FF FF 48 81 EC A0 01 00 00 48 C7 45 B0 FE FF";

// ES: Progreso de construcción. Encontrada por "construction progress". RVA 0x5547F0.
// Building construction progress handler
// Found via "construction progress"
// RVA: 0x005547F0
constexpr const char* BUILDING_CONSTRUCT = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 C7 44 24 58 FE";

// ES: Reparación de edificio. Encontrada por "construction progress" (segunda función,
//     ruta de reparación). RVA 0x555650.
// Building repair handler
// Found via "construction progress" (second function - repair path)
// RVA: 0x00555650
constexpr const char* BUILDING_REPAIR = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC 00 01 00 00 48 C7 45 87 FE FF FF";

// ES: Cadenas de punteros conocidas (de la comunidad de Cheat Engine). La base cambia con la
//     versión; los offsets de la cadena son estables. Nota: KNOWN_CHAINS no se consume en
//     ningún sitio (ver comentario de la Oleada A más abajo).
// ═══════════════════════════════════════════════════════════════════════════
//  KNOWN POINTER CHAINS (from Cheat Engine community)
// ═══════════════════════════════════════════════════════════════════════════
// Base offsets change per version; chain offsets are stable.

// ES: Una cadena: nombre, offset base desde kenshi_x64.exe, offsets terminados en -1 y descripción.
// EN: One chain: name, base offset from kenshi_x64.exe, -1 terminated offsets and description.
struct PointerChain {
    const char* name;
    uint32_t    baseOffset;    // From kenshi_x64.exe base
    int         offsets[8];    // -1 terminated chain
    const char* description;
};

// ES: Offsets base de la v1.0.68 (PlayerBase = RVA 0x01AC8A90).
// v1.0.68 base offsets
constexpr PointerChain KNOWN_CHAINS[] = {
    {"PlayerBase",  0x01AC8A90, {-1},                          "Player data base pointer"},
    // MUERTAS/INVÁLIDAS (Oleada A 2026-07-12): la cadena {0x2B8, 0x5F8, ...} tiene un deref de
    // más — char+0x2B8 es CharacterMemory* (_myMemory, una copia), NO donde el motor escribe
    // salud. La ruta canónica es char+0x458 (MedicalSystem inline) → +0x1A0 → part+0x40
    // (ver game_types.h healthPartArray/healthBase). Estas entradas ya no las usa nada
    // (KNOWN_CHAINS no se consume en ningún sitio); se comentan como muertas, no se borran.
    // EN: DEAD/INVALID (Wave A 2026-07-12): the {0x2B8, 0x5F8, ...} chain has one dereference too
    //     many — char+0x2B8 is CharacterMemory* (_myMemory, a copy), NOT where the engine writes
    //     health. The canonical path is char+0x458 (inline MedicalSystem) -> +0x1A0 -> part+0x40
    //     (see game_types.h healthPartArray/healthBase). Nothing uses these entries any more
    //     (KNOWN_CHAINS is not consumed anywhere); they are commented out as dead, not deleted.
    // {"Health",      0x01AC8A90, {0x2B8, 0x5F8, 0x40, -1},     "Character health (float) — INVÁLIDA, ver nota"},
    // {"StunDamage",  0x01AC8A90, {0x2B8, 0x5F8, 0x44, -1},     "Character stun damage (float) — INVÁLIDA, ver nota"},
    {"Money",       0x01AC8A90, {0x298, 0x78, 0x88, -1},       "Player money (int)"},
    {"CharList",    0x01AC8A90, {0x0, -1},                      "First character, +8 per next"},
};

// ES: Número de cadenas.
// EN: Number of chains.
constexpr size_t NUM_KNOWN_CHAINS = sizeof(KNOWN_CHAINS) / sizeof(KNOWN_CHAINS[0]);

// ES: Strings ancla para el escáner de respaldo en runtime: si el patrón falla se busca el
//     string en .rdata, luego el LEA que lo referencia y la función que lo contiene.
//     Actualizados a los strings reales de la v1.0.68. Nota: algunos son genéricos
//     ("knockout", "pathfind", "addItem") y pueden llevar a la función equivocada;
//     "delayedSpawningChecks" sigue aquí aunque SquadAddMember se resuelve por vtable.
// ═══════════════════════════════════════════════════════════════════════════
//  STRING ANCHORS FOR RUNTIME FALLBACK SCANNER
// ═══════════════════════════════════════════════════════════════════════════
// These unique strings are searched at runtime if patterns fail.
// Updated to match actual strings found in kenshi_x64.exe v1.0.68.

// ES: Un ancla: id de la función, texto a buscar y su longitud.
// EN: One anchor: function id, text to search and its length.
struct StringAnchor {
    const char* label;
    const char* searchString;
    int         searchStringLen;
};

constexpr StringAnchor STRING_ANCHORS[] = {
    // Entity lifecycle
    {"CharacterSpawn",       "[RootObjectFactory::process] Character",              38},
    {"CharacterDestroy",     "NodeList::destroyNodesByBuilding",                    32},
    {"CreateRandomSquad",    "[RootObjectFactory::createRandomSquad] Missing squad leader", 59},
    {"CharacterSerialise",   "[Character::serialise] Character '",                  34},
    {"CharacterKO",          "knockout",                                             8},
    // Movement
    {"CharacterSetPosition", "HavokCharacter::setPosition moved someone off the world", 55},
    {"CharacterMoveTo",      "pathfind",                                             8},
    // Combat
    {"ApplyDamage",          "Attack damage effect",                                20},
    {"StartAttack",          "Cutting damage",                                      14},
    {"CharacterDeath",       "{1} has died from blood loss.",                        29},
    {"HealthUpdate",         "block chance",                                         12},
    {"CutDamageMod",         "cut damage mod",                                      14},
    {"UnarmedDamage",        "unarmed damage bonus",                                20},
    {"MartialArtsCombat",    "Martial Arts",                                        12},
    // World / Zones
    {"ZoneLoad",             "zone.%d.%d.zone",                                     15},
    {"ZoneUnload",           "destroyed navmesh",                                   17},
    {"BuildingPlace",        "[RootObjectFactory::createBuilding] Building",         44},
    {"BuildingDestroyed",    "Building::setDestroyed",                              22},
    {"SpawnCheck",           " tried to spawn inside walls!",                        29},
    // Game loop / Time
    {"GameFrameUpdate",      "Kenshi 1.0.",                                         11},
    {"TimeUpdate",           "timeScale",                                            9},
    // Save/Load
    {"SaveGame",             "quicksave",                                             9},
    {"LoadGame",             "[SaveManager::loadGame] No towns loaded.",             40},
    {"ImportGame",           "[SaveManager::importGame] No towns loaded.",           42},
    {"CharacterStats",       "CharacterStats_Attributes",                            25},
    // Squad / Platoon
    {"SquadCreate",          "Reset squad positions",                                21},
    {"SquadAddMember",       "delayedSpawningChecks",                               21},
    // Inventory / Items
    {"ItemPickup",           "addItem",                                               7},
    {"ItemDrop",             "removeItem",                                            10},
    {"BuyItem",              "buyItem",                                                7},
    // Faction
    {"FactionRelation",      "faction relation",                                     16},
    // AI
    {"AICreate",             "[AI::create] No faction for",                          27},
    {"AIPackages",           "AI packages",                                          11},
    // Turret
    {"GunTurret",            "gun turret",                                           10},
};

// ES: Número de anclas.
// EN: Number of anchors.
constexpr size_t NUM_STRING_ANCHORS = sizeof(STRING_ANCHORS) / sizeof(STRING_ANCHORS[0]);

} // namespace patterns

// ES: Punteros a funciones resueltos: los rellena el escáner en runtime. Cada campo es la
//     dirección real (base + RVA) de una función del juego, o nullptr si no se resolvió
//     (en ese caso el hook correspondiente no se instala).
// ═══════════════════════════════════════════════════════════════════════════
//  RESOLVED FUNCTION POINTERS - filled at runtime by the scanner
// ═══════════════════════════════════════════════════════════════════════════

// ES: Direcciones resueltas de todas las funciones del juego que usa el mod.
// EN: Resolved addresses of every game function the mod uses.
struct GameFunctions {
    // ES: Ciclo de vida de entidades. FactoryCreate (0x583400) y CreateRandomChar (0x5836E0)
    //     no tienen patrón ni ancla: no se registran para resolverse (según docs/03-scanner.md).
    // ── Entity Lifecycle ──
    void*  CharacterSpawn       = nullptr;  // RootObjectFactory::process (0x581770)
    void*  FactoryCreate        = nullptr;  // RootObjectFactory::create (0x583400) — dispatcher, builds request struct internally
    void*  CreateRandomChar     = nullptr;  // RootObjectFactory::createRandomChar (0x5836E0) — creates random NPC
    void*  CharacterDestroy     = nullptr;  // NodeList destroy path
    void*  CreateRandomSquad    = nullptr;  // RootObjectFactory::createRandomSquad
    void*  CharacterSerialise   = nullptr;  // Character save/load serialization
    void*  CharacterKO          = nullptr;  // Knockout handler

    // ES: Movimiento.
    // ── Movement ──
    void*  CharacterSetPosition = nullptr;  // HavokCharacter::setPosition
    void*  CharacterMoveTo      = nullptr;  // Pathfinding move command

    // ES: Combate. Ojo: el comentario de IssueOrder la llama "orden real del jugador", pero el
    //     comentario del patrón ISSUE_ORDER dice que 0x722EF0 es una función de UI (REFUTADO).
    // ── Combat ──
    void*  ApplyDamage          = nullptr;  // Attack damage effect
    void*  StartAttack          = nullptr;  // Cut/blunt damage calculation
    void*  CharacterDeath       = nullptr;  // Death from blood loss
    void*  HealthUpdate         = nullptr;  // Health/damage resolution
    void*  CutDamageMod         = nullptr;  // Cut damage modifier
    void*  UnarmedDamage        = nullptr;  // Unarmed damage bonus
    void*  MartialArtsCombat    = nullptr;  // Martial arts combat handler
    // EN: IssueOrder (0x722EF0): see the ISSUE_ORDER pattern note; it is really a UI function.
    void*  IssueOrder           = nullptr;  // issueOrder/setJobTarget (0x722EF0) — orden real
                                            // del jugador (clic objetivo → Job de IA).
                                            // void(__fastcall*)(void* char, void* target, bool now)
    // EN: Tasker::pushOrder (0x674300) — INSERTS the order into the platoon Tasker map
    //     (this = *(char+0x658 ActivePlatoon +0x98)). It is where the attack order ENTERS the
    //     queue the AI tick ends up consuming (via AI+0x20 -> selector -> CharBody+0x68).
    void*  PushOrder            = nullptr;  // Tasker::pushOrder (0x674300) — INSERTA la orden en
                                            // el map del Tasker del platoon (this = *(char+0x658
                                            // ActivePlatoon +0x98)). Es el punto donde la orden de
                                            // ataque ENTRA en la cola que el AI tick acaba
                                            // consumiendo (vía AI+0x20 → selector → CharBody+0x68).
                                            // void(__fastcall*)(Tasker* this, RootObject* subject, int mode)
    // EN: Character::attackTarget (0x5CB0A0) — direct attack order: writes AI+0x78 (target)
    //     and queues into the platoon Tasker via enqueueCombatOrder(mode=4).
    void*  AttackTarget         = nullptr;  // Character::attackTarget (0x5CB0A0) — orden de ataque
                                            // directa: escribe AI+0x78 (target) y encola en el
                                            // Tasker del platoon vía enqueueCombatOrder(mode=4).
                                            // void(__fastcall*)(Character* me, Character* who)
    // EN: Character::addOrder "normal/replace" backend (0x5D1940, thunk 0x274A26, called by
    //     addOrder 0x5D20D0). VALIDATOR: true = order ABORTED (swallowed with the native bubble,
    //     not queued), false = order goes on.
    void*  AddOrderBackend      = nullptr;  // Character::addOrder backend "normal/replace"
                                            // (0x5D1940, thunk 0x274A26, llamado por addOrder
                                            // 0x5D20D0). VALIDADOR: true=orden ABORTADA (tragada
                                            // con globo nativo, sin encolar), false=orden sigue.
                                            // bool(__fastcall*)(Character* me, int taskType, void* subject)
    // EN: Character::setActivePlatoon (0x6213F0) — CONFIRMED in bytes (Steam 1.0.68). It does
    //     the AI<->platoon REGISTRATION that the mod's clone spawn SKIPS:
    //       1) [char+0x658] = platoon (ActivePlatoon*)  2) rcx = [char+0x650] (AI*)
    //       3) rdx = platoon ? [platoon+0x78] : 0 (platoon->me = Platoon*)
    //       4) call 0x506CC0 -> mov [AI+0x10], rdx => AI+0x10 = Platoon* (the skipped link)
    //       5) if platoon != 0: [char+0x418] = flags(r8d)
    //     Without step 4 the AI tick cannot find the platoon and its Tasker
    //     (ActivePlatoon+0x98) stays ORPHANED (never consumed).
    void*  SetActivePlatoon     = nullptr;  // Character::setActivePlatoon (0x6213F0) — CONFIRMADO
                                            // en bytes (Steam 1.0.68). Hace el REGISTRO AI<->platoon
                                            // que el spawn del clon del mod OMITE:
                                            //   1) [char+0x658] = platoon        (ActivePlatoon*)
                                            //   2) rcx = [char+0x650]            (AI*)
                                            //   3) rdx = platoon ? [platoon+0x78] : 0   (platoon->me = Platoon*)
                                            //   4) call 0x506CC0  ->  mov [AI+0x10], rdx
                                            //      => AI+0x10 = Platoon*  (ESTE es el enlace omitido)
                                            //   5) si platoon!=0: [char+0x418] = flags(r8d)
                                            // Sin el paso 4 el AI tick no encuentra el platoon y su
                                            // Tasker (ActivePlatoon+0x98) queda HUERFANO (no consumido).
                                            // void(__fastcall*)(Character* me, void* activePlatoon, int flags)
    // EN: CombatClass::update(float) (0x60D650) — DIAG-COMBATSEED. Mangled
    //     ?update@CombatClass@@UEAAXM@Z (virtual, void, 1 float). It is the CombatClass combat AI
    //     tick: consumes the perception array (CombatClass+0x208, counter +0x200) to produce
    //     currentTarget (CombatClass+0x290) and trigger Task_MeleeAttack. CLEAN prologue
    //     (40 53 48 83 EC 20 48 8B D9, no mov rax,rsp), hookable without the MovRaxRsp fix.
    //     `this` (rcx) is the CombatClass; dt goes in xmm1.
    void*  CombatClassUpdate    = nullptr;  // CombatClass::update(float) (0x60D650) — DIAG-COMBATSEED.
                                            // Mangled ?update@CombatClass@@UEAAXM@Z (virtual, void, 1 float).
                                            // Es el AI tick de combate del CombatClass: consume el array
                                            // de percepciones (CombatClass+0x208, contador +0x200) para
                                            // producir currentTarget (CombatClass+0x290) y disparar
                                            // Task_MeleeAttack. Prólogo LIMPIO (40 53 48 83 EC 20 48 8B D9,
                                            // sin mov rax,rsp) → hookeable sin el fix MovRaxRsp. El this
                                            // (rcx) es el CombatClass; el dt va en xmm1.
                                            // void(__fastcall*)(CombatClass* this, float dt)
    // NOTA (2026-07-15): se eliminó el campo duplicado 'CombatTargetResolve' (0x74A630). Esa RVA
    // es BuyItem (ver campo BuyItem más abajo). El hook de esa RVA (inventory_hooks.cpp:
    // Hook_BuyItem, sync de red + guard UAF fusionados) usa funcs.BuyItem directamente — no
    // necesita un puntero propio.
    // EN: NOTE (2026-07-15): the duplicate 'CombatTargetResolve' field (0x74A630) was removed.
    //     That RVA is BuyItem (see the BuyItem field below). The hook on that RVA
    //     (inventory_hooks.cpp: Hook_BuyItem) uses funcs.BuyItem directly.

    // ES: Mundo / zonas. SquadSpawnBypass, SquadSpawnCall y CharAnimUpdate son direcciones
    //     dentro de funciones (no inicios) tomadas de un mod de investigación sobre GOG.
    // ── World / Zones ──
    void*  ZoneLoad             = nullptr;  // Zone loading
    void*  ZoneUnload           = nullptr;  // Zone unloading / navmesh teardown
    void*  BuildingPlace        = nullptr;  // Building placement
    void*  BuildingDestroyed    = nullptr;  // Building destruction
    void*  Navmesh              = nullptr;  // Navmesh system
    void*  SpawnCheck           = nullptr;  // Spawn collision check
    void*  SquadSpawnBypass    = nullptr;  // Squad spawn check bypass (research mod: GOG 0x4FF47C)
    void*  SquadSpawnCall      = nullptr;  // Squad spawn function call site (research mod: GOG 0x4FFA88)
    void*  CharAnimUpdate      = nullptr;  // Character animation update callback (research mod: GOG 0x65F6C7)

    // ES: Bucle de juego / tiempo. SetPaused = GameWorld::setPaused (ver patrón SET_PAUSED).
    // ── Game Loop / Time ──
    void*  GameFrameUpdate      = nullptr;  // Main game frame tick
    void*  TimeUpdate           = nullptr;  // Time scale handler
    void*  SetPaused            = nullptr;  // GameWorld::setPaused (0x787D40) — setter OFICIAL
                                            // de pausa: refresca caches de subsistemas + HUD.
                                            // void(__fastcall*)(void* gameWorld, bool paused)

    // ES: Guardar / cargar.
    // ── Save / Load ──
    void*  SaveGame             = nullptr;  // Save game
    void*  LoadGame             = nullptr;  // Load game
    void*  ImportGame           = nullptr;  // Import game (new from template)
    void*  CharacterStats       = nullptr;  // Character stats UI

    // ES: Entrada (opcional: se usa WndProc).
    // ── Input ──
    void*  InputKeyPressed      = nullptr;  // OIS key input (optional, WndProc used)
    void*  InputMouseMoved      = nullptr;  // OIS mouse input (optional, WndProc used)

    // ES: Escuadrón / platoon.
    // ── Squad / Platoon ──
    void*  SquadCreate          = nullptr;  // Squad position reset / management
    void*  SquadAddMember       = nullptr;  // Delayed spawning / member additions

    // ES: Inventario / objetos.
    // ── Inventory / Items ──
    void*  ItemPickup           = nullptr;  // Inventory addItem
    void*  ItemDrop             = nullptr;  // Inventory removeItem
    void*  BuyItem              = nullptr;  // Shop purchase

    // ES: Facción / diplomacia.
    // ── Faction / Diplomacy ──
    void*  FactionRelation      = nullptr;  // Faction relation changes

    // ES: Sistema de IA.
    // ── AI System ──
    void*  AICreate             = nullptr;  // AI controller creation
    void*  AIPackages           = nullptr;  // AI behavior package loader

    // ES: Torretas / distancia.
    // ── Turret / Ranged ──
    void*  GunTurret            = nullptr;  // Turret operation
    void*  GunTurretFire        = nullptr;  // Turret targeting/fire

    // ES: Gestión de edificios.
    // ── Building Management ──
    void*  BuildingDismantle    = nullptr;  // Dismantle building
    void*  BuildingConstruct    = nullptr;  // Construction progress
    void*  BuildingRepair       = nullptr;  // Building repair

    // ES: Punteros base conocidos: PlayerBase (datos del jugador) y GameWorldSingleton (en 1.0.68
    //     es la instancia estática de GameWorld en .data, RVA 0x2134110, no un puntero; ver
    //     resolveGwObject en patterns.cpp).
    // EN: Known base pointers: PlayerBase (player data) and GameWorldSingleton (on 1.0.68 it is
    //     the static GameWorld instance in .data, RVA 0x2134110, not a pointer; see
    //     resolveGwObject in patterns.cpp).
    // ── Known Base Pointers ──
    uintptr_t PlayerBase         = 0;
    uintptr_t GameWorldSingleton = 0;

    // ES: ¿Hay lo mínimo para arrancar? Sí si hay PlayerBase, o si hay CharacterSpawn y además
    //     GameFrameUpdate o TimeUpdate (en Steam los singletons se descubren más tarde).
    // EN: Is the minimum there to start? Yes if PlayerBase exists, or if CharacterSpawn plus
    //     GameFrameUpdate or TimeUpdate exist (on Steam singletons are discovered later).
    bool IsMinimallyResolved() const {
        // ES: En Steam PlayerBase puede no encontrarse al iniciar; se reintenta al cargar partida.
        // On Steam, PlayerBase may not be found during init (singletons are
        // discovered later when the game loads and the values become non-null).
        // Consider resolved if we have either PlayerBase OR the critical hooks.
        if (PlayerBase != 0) return true;
        // Fallback: at least CharacterSpawn + GameFrameUpdate/TimeUpdate resolved
        // means string xref worked and hooks can install — singletons can be retried later.
        return (CharacterSpawn != nullptr) &&
               (GameFrameUpdate != nullptr || TimeUpdate != nullptr);
    }

    // ES: Cuenta cuántas direcciones están resueltas (43 punteros de la lista + 2 globales).
    //     Nota: TotalFunctions() devuelve 41, que no cuadra con esos 45 posibles, y la lista
    //     no incluye AddOrderBackend, CombatClassUpdate, SetPaused, FactoryCreate, etc.
    // EN: Counts how many addresses are resolved (43 listed pointers + 2 globals).
    //     Note: TotalFunctions() returns 41, which does not match those 45 possible, and the
    //     list leaves out AddOrderBackend, CombatClassUpdate, SetPaused, FactoryCreate, etc.
    int CountResolved() const {
        int count = 0;
        const void* const* ptrs[] = {
            &CharacterSpawn, &CharacterDestroy, &CreateRandomSquad,
            &CharacterSerialise, &CharacterKO,
            &CharacterSetPosition, &CharacterMoveTo,
            &ApplyDamage, &StartAttack, &CharacterDeath,
            &HealthUpdate, &CutDamageMod, &UnarmedDamage, &MartialArtsCombat,
            &IssueOrder, &PushOrder, &AttackTarget, &SetActivePlatoon,
            &ZoneLoad, &ZoneUnload, &BuildingPlace, &BuildingDestroyed,
            &Navmesh, &SpawnCheck,
            &GameFrameUpdate, &TimeUpdate,
            &SaveGame, &LoadGame, &ImportGame, &CharacterStats,
            &SquadCreate, &SquadAddMember,
            &ItemPickup, &ItemDrop, &BuyItem,
            &FactionRelation,
            &AICreate, &AIPackages,
            &GunTurret, &GunTurretFire,
            &BuildingDismantle, &BuildingConstruct, &BuildingRepair,
        };
        for (auto* p : ptrs) {
            if (*p != nullptr) count++;
        }
        if (PlayerBase) count++;
        if (GameWorldSingleton) count++;
        return count;
    }

    // ES: Total "nominal" de funciones para los informes (ver nota de CountResolved).
    // EN: Nominal function total for reports (see the CountResolved note).
    static constexpr int TotalFunctions() { return 41; }
};

// ES: Declaración adelantada de PatternScanner (definido en scanner.h).
// Forward declaration for PatternScanner (defined in scanner.h)
class PatternScanner;

// ES: Camino B: resuelve GameFunctions con patrones AOB + respaldo por strings en runtime
//     + descubrimiento de globales por desensamblado. Devuelve si hay lo mínimo resuelto.
// Resolve game function pointers using patterns + runtime string fallback
bool ResolveGameFunctions(const PatternScanner& scanner, GameFunctions& funcs);

// ES: Reintenta el descubrimiento de globales (PlayerBase/GameWorld) cuando la partida ya
//     cargó y los valores dejaron de ser nulos.
// Re-run global pointer discovery after game has loaded.
bool RetryGlobalDiscovery(const PatternScanner& scanner, GameFunctions& funcs);

} // namespace kmp
