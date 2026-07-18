#pragma once
#include <cstdint>
#include <unordered_map>
#include <string>

namespace kmp {

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

// Detect the game version from the PE file version resource.
// Returns version string like "1.0.68.0" or "unknown" on failure.
std::string DetectGameVersion();

namespace patterns {

// ── D3D11/DXGI (for overlay) ──
// IDXGISwapChain::Present is found via vtable, not pattern.

// ═══════════════════════════════════════════════════════════════════════════
//  ENTITY LIFECYCLE
// ═══════════════════════════════════════════════════════════════════════════

// RootObjectFactory::process - processes character creation
// Found via "[RootObjectFactory::process] Character '"
// RVA: 0x00581770
// Wildcards on the two disp32 operands (LEA-rbp displacement + stack-alloc size) per
// opcode-stability audit — those bytes are compiler-frame-size-dependent tier-C.
constexpr const char* CHARACTER_SPAWN = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 48 C7 45 C0";

// NodeList destroy / character cleanup path
// Found via "NodeList::destroyNodesByBuilding"
// RVA: 0x0038A720
constexpr const char* CHARACTER_DESTROY = "48 8B C4 44 88 40 18 48 89 50 10 48 89 48 08 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 48 48";

// RootObjectFactory::createRandomSquad - squad+character batch spawning
// Found via "[RootObjectFactory::createRandomSquad] Missing squad leader"
// RVA: 0x00583A10
constexpr const char* CREATE_RANDOM_SQUAD = "48 8B C4 55 53 56 57 48 8D A8 F8 FD FF FF 48 81 EC E8 02 00 00 48 C7 45 40 FE FF FF FF 0F 29 70";

// Character serialise (save/load character data)
// Found via "[Character::serialise] Character '"
// RVA: 0x006280A0
constexpr const char* CHARACTER_SERIALISE = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 38 FF FF FF 48 81 EC C8 01 00 00 48 C7 45 C0";

// Character knockout
// Found via "knockout"
// RVA: 0x00345C10
constexpr const char* CHARACTER_KO = "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B 02 48 8B E9 ? ? ? ? 8B FA";

// ═══════════════════════════════════════════════════════════════════════════
//  MOVEMENT
// ═══════════════════════════════════════════════════════════════════════════

// HavokCharacter::setPosition
// Found via "HavokCharacter::setPosition moved someone off the world"
// RVA: 0x00145E50
constexpr const char* CHARACTER_SET_POSITION = "48 8B C4 55 57 41 54 48 8D 68 C8 48 81 EC 20 01 00 00 48 C7 44 24 30 FE FF FF FF 48 89 58 18 48";

// Player move command (pathfinding)
// Found via "pathfind"
// RVA: 0x002EF4E3
// DISABLED — pattern contained a RIP-relative IAT indirect call (FF 15 6C 55 F5 01) whose
// 4-byte displacement is a per-binary IAT offset, unwildcardable without losing all
// uniqueness. CharacterMoveTo is also hook-disabled at the hook layer (mov rax,rsp +
// 5 params — stack param can't be forwarded through the MovRaxRsp naked detour).
constexpr const char* CHARACTER_MOVE_TO = nullptr;

// ═══════════════════════════════════════════════════════════════════════════
//  COMBAT
// ═══════════════════════════════════════════════════════════════════════════

// Attack damage effect handler
// Found via "Attack damage effect"
// RVA: 0x007A33A0
constexpr const char* APPLY_DAMAGE = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 68 FC FF FF 48 81 EC 60 04 00 00 48 C7 45 98";

// Cut/blunt damage calculation
// Found via "Cutting damage" / "Blunt damage"
// RVA: 0x007B2A20
constexpr const char* START_ATTACK = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 08 FC FF FF 48 81 EC C0 04 00 00 48 C7 44 24";

// Death from blood loss / starvation handler
// Found via "{1} has died from blood loss."
// RVA: 0x007A6200
constexpr const char* CHARACTER_DEATH = "48 8B C4 55 57 41 54 41 55 41 56 48 8D A8 28 FE FF FF 48 81 EC B0 02 00 00 48 C7 44 24 28 FE FF";

// Health/blood system - combat damage resolution + block chance
// Found via "block chance" and "damage resistance max"
// RVA: 0x0086B2B0
constexpr const char* HEALTH_UPDATE = "48 8B C4 55 57 41 54 41 55 41 56 48 8D 68 A1 48 81 EC F0 00 00 00 48 C7 44 24 28 FE FF FF FF 48";

// Cut damage modifier calculation
// Found via "cut damage mod"
// RVA: 0x00889CD0
constexpr const char* CUT_DAMAGE_MOD = "40 55 53 56 57 41 54 41 55 41 56 48 8B EC 48 83 EC 70 48 C7 45 B0 FE FF FF FF 0F 29 74 24 60 48";

// Unarmed damage bonus
// Found via "unarmed damage bonus"
// RVA: 0x000CE2D0
constexpr const char* UNARMED_DAMAGE = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC B0 00 00 00 48 C7 45 BF FE FF FF FF";

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
constexpr const char* ADD_ORDER_BACKEND = "40 55 56 57 48 8D AC 24 60 FE FF FF 48 81 EC A0 02 00 00 48 C7 44 24 20 FE FF FF FF 48 89 9C 24";

// NOTA (2026-07-15): aquí había un patrón COMBAT_TARGET_RESOLVE para la RVA 0x0074A630,
// creído "selector de objetivo de combate". La RE posterior (3 strings internos de comercio
// + xref al literal "buyItem" + análisis del caller que referencia 'Sell_Item') confirmó que
// 0x74A630 es BuyItem (rutina de IA de COMERCIO). Ese AOB era un SUPERCONJUNTO del de BUY_ITEM
// que casaba en la MISMA dirección → duplicado innecesario. Eliminado. El hook de esa RVA
// (inventory_hooks.cpp: Hook_BuyItem, sync de red + guard UAF fusionados) usa el patrón BUY_ITEM
// de más abajo vía funcs.BuyItem. Ver la nota ampliada en el patrón BUY_ITEM (sección INVENTORY / ITEMS).

// ═══════════════════════════════════════════════════════════════════════════
//  WORLD / ZONES
// ═══════════════════════════════════════════════════════════════════════════

// Zone load function
// Found via "zone.%d.%d.zone"
// RVA: 0x00377710
constexpr const char* ZONE_LOAD = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 E0 FC FF FF 48 81 EC ? ? ? ? 48 C7 45 88 FE";

// Zone unload / navmesh teardown
// Found via "destroyed navmesh" (first xref)
// RVA: 0x002EF1F0
constexpr const char* ZONE_UNLOAD = "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 68 88 48 81 EC 50 01 00 00 48 C7 44 24 50 FE FF FF FF";

// Building placement / creation
// Found via "[RootObjectFactory::createBuilding] Building '"
// RVA: 0x0057CC70
constexpr const char* BUILDING_PLACE = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 F8 FB FF FF 48 81 EC D0 04 00 00 48 C7 45 F8";

// Building destroyed handler
// Found via "Building::setDestroyed"
// RVA: 0x00557280
constexpr const char* BUILDING_DESTROYED = "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 C7 45 30 FE FF FF FF";

// Navmesh system
// Found via "navmesh"
// RVA: 0x00881950
constexpr const char* NAVMESH = "48 89 54 24 ? 57 48 83 EC ? 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 ? 49 8B F8 48 8B DA 48 89";

// Spawn in buildings check
// Found via " tried to spawn inside walls!"
// RVA: 0x004FFAD0
constexpr const char* SPAWN_CHECK = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 30 FF FF FF 48 81 EC D0 01 00 00 48 C7 45 C0 FE";

// ═══════════════════════════════════════════════════════════════════════════
//  GAME LOOP / TIME
// ═══════════════════════════════════════════════════════════════════════════

// Game frame update / initialization
// Found via "Kenshi 1.0."
// RVA: 0x00123A10
// Extended to 45 tokens for uniqueness (false positive at 0x00788100 shares first 32 bytes)
constexpr const char* GAME_FRAME_UPDATE = "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 68 88 48 81 EC 50 01 00 00 48 C7 44 24 38 FE FF FF FF 48 89 58 10 48 89 70 18 48 89 78 20 48";

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
constexpr const char* SET_PAUSED = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 0F 29 74 24 40 F3 0F 10 B1 00 07 00 00 33 F6 0F 2E 35 ? ? ? ? 0F B6 DA 48 8B F9";

// ═══════════════════════════════════════════════════════════════════════════
//  SAVE / LOAD
// ═══════════════════════════════════════════════════════════════════════════

// Save function
// Found via "quicksave"
// RVA: 0x007EF040
constexpr const char* SAVE_GAME = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F9 48 81 EC ? ? ? ? 48 C7 45 97 FE FF FF FF";

// Load function
// Found via "[SaveManager::loadGame] No towns loaded."
// RVA: 0x00373F00
constexpr const char* LOAD_GAME = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 C0 FC FF FF 48 81 EC 40 04 00 00 48 C7 45 A8 FE";

// Import game (new game from template)
// Found via "[SaveManager::importGame] No towns loaded."
// RVA: 0x00378A30
constexpr const char* IMPORT_GAME = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 78 FC FF FF 48 81 EC 50 04 00 00 48 C7 45 C0";

// Character stats UI
// Found via "CharacterStats_Attributes"
// RVA: 0x008BA700
constexpr const char* CHARACTER_STATS = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 08 FF FF FF 48 81 EC C0 01 00 00 48 C7 44 24";

// ═══════════════════════════════════════════════════════════════════════════
//  INPUT (handled via WndProc hook in render_hooks, patterns optional)
// ═══════════════════════════════════════════════════════════════════════════

// OIS keyboard/mouse input is handled via vtable dispatch, not hookable patterns.
// Kenshi routes input through MyGUI::InputManager::injectKeyPress/injectMouseMove.
// We intercept input at the Win32 WndProc level in render_hooks.cpp instead.
constexpr const char* INPUT_KEY_PRESSED = nullptr;
constexpr const char* INPUT_MOUSE_MOVED = nullptr;

// ═══════════════════════════════════════════════════════════════════════════
//  SQUAD / PLATOON MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

// Squad position reset / squad management entry
// Found via "Reset squad positions"
// RVA: 0x00480B50
constexpr const char* SQUAD_CREATE = "48 8B C4 55 41 54 41 55 48 8D A8 28 FF FF FF 48 81 EC C0 01 00 00 48 C7 44 24 48 FE FF FF FF 48";

// Delayed spawning checks - processes new squad member additions
// Found via "delayedSpawningChecks"
// RVA: 0x00928423
// DISABLED — pattern matches mid-function on Steam (not at a .pdata function entry) and
// finds a different function via alternate string xref. Orchestrator resolves this via
// RTTI vtable lookup (vtableClass="ActivePlatoon|Platoon|Squad", vtableSlot=2). Leaving
// a real byte string risks the auto-correct path in tryPattern overriding the RTTI result.
constexpr const char* SQUAD_ADD_MEMBER = nullptr;

// ═══════════════════════════════════════════════════════════════════════════
//  INVENTORY / ITEMS
// ═══════════════════════════════════════════════════════════════════════════

// Inventory addItem - adds item to character/container inventory
// Found via "addItem"
// RVA: 0x0074C8B0
constexpr const char* ITEM_PICKUP = "48 8B C4 44 89 40 18 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 40 48 C7 45 E0 FE FF FF FF 48";

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
constexpr const char* BUY_ITEM = "40 55 56 57 41 54 41 55 48 81 EC 00 01 00 00 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 48 01 00 00";

// ═══════════════════════════════════════════════════════════════════════════
//  FACTION / DIPLOMACY
// ═══════════════════════════════════════════════════════════════════════════

// Faction relation handler - processes faction relation changes
// Found via "faction relation"
// RVA: 0x00872E00
constexpr const char* FACTION_RELATION = "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 58 FD FF FF 48 81 EC 80 03 00 00 48 C7 85 90 00 00";

// ═══════════════════════════════════════════════════════════════════════════
//  AI SYSTEM
// ═══════════════════════════════════════════════════════════════════════════

// AI::create - creates AI controller for a character
// Found via "[AI::create] No faction for"
// RVA: 0x00622110
// Extended to 41 tokens for uniqueness (false positive at 0x000AF870 shares first 29 fixed bytes)
constexpr const char* AI_CREATE = "40 57 48 81 EC 90 00 00 00 48 C7 44 24 28 FE FF FF FF 48 89 9C 24 B0 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 80";

// AI packages loader - loads and assigns AI behavior packages
// Found via "AI packages"
// RVA: 0x00271620
constexpr const char* AI_PACKAGES = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC 00 01 00 00 48 C7 45 9F FE FF FF FF";

// ═══════════════════════════════════════════════════════════════════════════
//  TURRET / RANGED COMBAT
// ═══════════════════════════════════════════════════════════════════════════

// Gun turret operation handler
// Found via "gun turret"
// RVA: 0x0043B690
constexpr const char* GUN_TURRET = "48 8B C4 55 41 54 41 55 48 8D 68 A1 48 81 EC C0 00 00 00 48 C7 45 E7 FE FF FF FF 48 89 58 10 48";

// Gun turret targeting/fire handler
// Found via "gun turret" (second function)
// RVA: 0x0043CDB0
constexpr const char* GUN_TURRET_FIRE = "40 55 53 56 57 41 54 41 55 48 8D AC 24 F8 FE FF FF 48 81 EC 08 02 00 00 48 C7 44 24 58 FE FF FF";


// ═══════════════════════════════════════════════════════════════════════════
//  BUILDING MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

// Building dismantle handler
// Found via "dismantle"
// RVA: 0x002A2860
constexpr const char* BUILDING_DISMANTLE = "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 38 FF FF FF 48 81 EC A0 01 00 00 48 C7 45 B0 FE FF";

// Building construction progress handler
// Found via "construction progress"
// RVA: 0x005547F0
constexpr const char* BUILDING_CONSTRUCT = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 C7 44 24 58 FE";

// Building repair handler
// Found via "construction progress" (second function - repair path)
// RVA: 0x00555650
constexpr const char* BUILDING_REPAIR = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC 00 01 00 00 48 C7 45 87 FE FF FF";

// ═══════════════════════════════════════════════════════════════════════════
//  KNOWN POINTER CHAINS (from Cheat Engine community)
// ═══════════════════════════════════════════════════════════════════════════
// Base offsets change per version; chain offsets are stable.

struct PointerChain {
    const char* name;
    uint32_t    baseOffset;    // From kenshi_x64.exe base
    int         offsets[8];    // -1 terminated chain
    const char* description;
};

// v1.0.68 base offsets
constexpr PointerChain KNOWN_CHAINS[] = {
    {"PlayerBase",  0x01AC8A90, {-1},                          "Player data base pointer"},
    // MUERTAS/INVÁLIDAS (Oleada A 2026-07-12): la cadena {0x2B8, 0x5F8, ...} tiene un deref de
    // más — char+0x2B8 es CharacterMemory* (_myMemory, una copia), NO donde el motor escribe
    // salud. La ruta canónica es char+0x458 (MedicalSystem inline) → +0x1A0 → part+0x40
    // (ver game_types.h healthPartArray/healthBase). Estas entradas ya no las usa nada
    // (KNOWN_CHAINS no se consume en ningún sitio); se comentan como muertas, no se borran.
    // {"Health",      0x01AC8A90, {0x2B8, 0x5F8, 0x40, -1},     "Character health (float) — INVÁLIDA, ver nota"},
    // {"StunDamage",  0x01AC8A90, {0x2B8, 0x5F8, 0x44, -1},     "Character stun damage (float) — INVÁLIDA, ver nota"},
    {"Money",       0x01AC8A90, {0x298, 0x78, 0x88, -1},       "Player money (int)"},
    {"CharList",    0x01AC8A90, {0x0, -1},                      "First character, +8 per next"},
};

constexpr size_t NUM_KNOWN_CHAINS = sizeof(KNOWN_CHAINS) / sizeof(KNOWN_CHAINS[0]);

// ═══════════════════════════════════════════════════════════════════════════
//  STRING ANCHORS FOR RUNTIME FALLBACK SCANNER
// ═══════════════════════════════════════════════════════════════════════════
// These unique strings are searched at runtime if patterns fail.
// Updated to match actual strings found in kenshi_x64.exe v1.0.68.

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

constexpr size_t NUM_STRING_ANCHORS = sizeof(STRING_ANCHORS) / sizeof(STRING_ANCHORS[0]);

} // namespace patterns

// ═══════════════════════════════════════════════════════════════════════════
//  RESOLVED FUNCTION POINTERS - filled at runtime by the scanner
// ═══════════════════════════════════════════════════════════════════════════

struct GameFunctions {
    // ── Entity Lifecycle ──
    void*  CharacterSpawn       = nullptr;  // RootObjectFactory::process (0x581770)
    void*  FactoryCreate        = nullptr;  // RootObjectFactory::create (0x583400) — dispatcher, builds request struct internally
    void*  CreateRandomChar     = nullptr;  // RootObjectFactory::createRandomChar (0x5836E0) — creates random NPC
    void*  CharacterDestroy     = nullptr;  // NodeList destroy path
    void*  CreateRandomSquad    = nullptr;  // RootObjectFactory::createRandomSquad
    void*  CharacterSerialise   = nullptr;  // Character save/load serialization
    void*  CharacterKO          = nullptr;  // Knockout handler

    // ── Movement ──
    void*  CharacterSetPosition = nullptr;  // HavokCharacter::setPosition
    void*  CharacterMoveTo      = nullptr;  // Pathfinding move command

    // ── Combat ──
    void*  ApplyDamage          = nullptr;  // Attack damage effect
    void*  StartAttack          = nullptr;  // Cut/blunt damage calculation
    void*  CharacterDeath       = nullptr;  // Death from blood loss
    void*  HealthUpdate         = nullptr;  // Health/damage resolution
    void*  CutDamageMod         = nullptr;  // Cut damage modifier
    void*  UnarmedDamage        = nullptr;  // Unarmed damage bonus
    void*  MartialArtsCombat    = nullptr;  // Martial arts combat handler
    void*  IssueOrder           = nullptr;  // issueOrder/setJobTarget (0x722EF0) — orden real
                                            // del jugador (clic objetivo → Job de IA).
                                            // void(__fastcall*)(void* char, void* target, bool now)
    void*  PushOrder            = nullptr;  // Tasker::pushOrder (0x674300) — INSERTA la orden en
                                            // el map del Tasker del platoon (this = *(char+0x658
                                            // ActivePlatoon +0x98)). Es el punto donde la orden de
                                            // ataque ENTRA en la cola que el AI tick acaba
                                            // consumiendo (vía AI+0x20 → selector → CharBody+0x68).
                                            // void(__fastcall*)(Tasker* this, RootObject* subject, int mode)
    void*  AttackTarget         = nullptr;  // Character::attackTarget (0x5CB0A0) — orden de ataque
                                            // directa: escribe AI+0x78 (target) y encola en el
                                            // Tasker del platoon vía enqueueCombatOrder(mode=4).
                                            // void(__fastcall*)(Character* me, Character* who)
    void*  AddOrderBackend      = nullptr;  // Character::addOrder backend "normal/replace"
                                            // (0x5D1940, thunk 0x274A26, llamado por addOrder
                                            // 0x5D20D0). VALIDADOR: true=orden ABORTADA (tragada
                                            // con globo nativo, sin encolar), false=orden sigue.
                                            // bool(__fastcall*)(Character* me, int taskType, void* subject)
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

    // ── Game Loop / Time ──
    void*  GameFrameUpdate      = nullptr;  // Main game frame tick
    void*  TimeUpdate           = nullptr;  // Time scale handler
    void*  SetPaused            = nullptr;  // GameWorld::setPaused (0x787D40) — setter OFICIAL
                                            // de pausa: refresca caches de subsistemas + HUD.
                                            // void(__fastcall*)(void* gameWorld, bool paused)

    // ── Save / Load ──
    void*  SaveGame             = nullptr;  // Save game
    void*  LoadGame             = nullptr;  // Load game
    void*  ImportGame           = nullptr;  // Import game (new from template)
    void*  CharacterStats       = nullptr;  // Character stats UI

    // ── Input ──
    void*  InputKeyPressed      = nullptr;  // OIS key input (optional, WndProc used)
    void*  InputMouseMoved      = nullptr;  // OIS mouse input (optional, WndProc used)

    // ── Squad / Platoon ──
    void*  SquadCreate          = nullptr;  // Squad position reset / management
    void*  SquadAddMember       = nullptr;  // Delayed spawning / member additions

    // ── Inventory / Items ──
    void*  ItemPickup           = nullptr;  // Inventory addItem
    void*  ItemDrop             = nullptr;  // Inventory removeItem
    void*  BuyItem              = nullptr;  // Shop purchase

    // ── Faction / Diplomacy ──
    void*  FactionRelation      = nullptr;  // Faction relation changes

    // ── AI System ──
    void*  AICreate             = nullptr;  // AI controller creation
    void*  AIPackages           = nullptr;  // AI behavior package loader

    // ── Turret / Ranged ──
    void*  GunTurret            = nullptr;  // Turret operation
    void*  GunTurretFire        = nullptr;  // Turret targeting/fire

    // ── Building Management ──
    void*  BuildingDismantle    = nullptr;  // Dismantle building
    void*  BuildingConstruct    = nullptr;  // Construction progress
    void*  BuildingRepair       = nullptr;  // Building repair

    // ── Known Base Pointers ──
    uintptr_t PlayerBase         = 0;
    uintptr_t GameWorldSingleton = 0;

    bool IsMinimallyResolved() const {
        // On Steam, PlayerBase may not be found during init (singletons are
        // discovered later when the game loads and the values become non-null).
        // Consider resolved if we have either PlayerBase OR the critical hooks.
        if (PlayerBase != 0) return true;
        // Fallback: at least CharacterSpawn + GameFrameUpdate/TimeUpdate resolved
        // means string xref worked and hooks can install — singletons can be retried later.
        return (CharacterSpawn != nullptr) &&
               (GameFrameUpdate != nullptr || TimeUpdate != nullptr);
    }

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

    static constexpr int TotalFunctions() { return 41; }
};

// Forward declaration for PatternScanner (defined in scanner.h)
class PatternScanner;

// Resolve game function pointers using patterns + runtime string fallback
bool ResolveGameFunctions(const PatternScanner& scanner, GameFunctions& funcs);

// Re-run global pointer discovery after game has loaded.
bool RetryGlobalDiscovery(const PatternScanner& scanner, GameFunctions& funcs);

} // namespace kmp
