#pragma once
#include "kmp/types.h"
#include <cstdint>
#include <string>

// Reconstructed Kenshi game class layouts.
// Based on reverse engineering kenshi_x64.exe v1.0.68 via:
//   - Cheat Engine pointer chains (CE community verified)
//   - String xref analysis and binary scanning
//   - KServerMod and RE_Kenshi reference data
//   - fcs.def game data schema
//
// Use the pattern scanner to verify offsets at runtime.
// If offsets are -1, the accessor returns safe defaults.

namespace kmp::game {

// Forward declarations
struct KCharacter;
struct KSquad;
struct KGameWorld;
struct KBuilding;
struct KInventory;
struct KStats;
struct KFaction;
struct KItem;
struct KAIPackage;

// ═══════════════════════════════════════════════════════════════════════════
//  OFFSET TABLES
// ═══════════════════════════════════════════════════════════════════════════
// Filled at runtime by the scanner, or using hardcoded CE-verified fallbacks.

struct CharacterOffsets {
    // Core character data — KServerMod / CE verified for v1.0.68
    int name          = 0x18;    // Kenshi std::string (KServerMod verified)
    int faction       = 0x10;    // Faction* (KServerMod verified)
    int position      = 0x48;    // Vec3 read-only cached position (KServerMod verified)
    int rotation      = 0x58;    // Quat rotation (KServerMod verified)
    int sceneNode     = -1;      // Ogre::SceneNode* (not yet verified — runtime probe)
    // ✅ CONFIRMADO (RE de bytes 2026-06-18, AI::create 0x622110): el "AI package" del Character
    // es un AITaskSytem* (sic, RTTI .?AVAITaskSytem@@, vtable RVA 0x16E3F30) que AI::create crea
    // (objeto de 0x3B8 bytes) y escribe en char+0x20 con `mov [rbx+0x20],rax` (0x6221AF). La cola
    // de tareas es un lektor<Tasker*> INLINE en AITaskSytem+0x2E8 (size@+0x2F0 uint32, cap@+0x2F4,
    // data@+0x2F8 Tasker**). Un char con char+0x20==NULL no tiene IA y queda inerte (el think
    // pesado [char_vtbl+0x1D8]→0x5CE020 no tiene manager que procesar). Ver [DIAG-AITASK] en core.cpp.
    int aiPackage     = 0x20;    // AITaskSytem* — char+0x20 (CONFIRMADO RE; cola jobs en +0x2E8/+0x2F0)
    int inventory     = 0x2E8;   // Inventory* (KServerMod verified)
    int stats         = 0x450;   // Stats base (KServerMod verified)
    int equipment     = -1;      // Equipment array (runtime probed)
    int currentTask   = -1;      // Current task type (not yet verified)
    int isAlive       = -1;      // Alive flag — use health chain fallback
    // isPlayerControlled: -1 = pendiente, -2 = N/A (campo INEXISTENTE en Character).
    // RE 2026-06-17 (KenshiLib Character.h + binario 1.0.68): Character NO tiene este
    // campo. La distinción player/NPC se deriva por facción:
    //   char.faction(+0x10) == gameWorld.player(+0x580).faction  (Character::isPlayerCharacter()).
    // El probe diferencial quedó neutralizado en game_offset_prober.cpp.
    int isPlayerControlled = -1; // Ver nota arriba — campo inexistente, se deriva por facción

    // Direct health offset (if scanner finds it; otherwise use chain below)
    int health            = -1;      // Direct offset to health array (use chain if -1)

    // Movement
    int moveSpeed     = -1;      // Offset to current move speed float (derived from physics)
    int animState     = -1;      // Offset to animation state index

    // Cadena de salud CANÓNICA — verificada en BYTES sobre kenshi_x64.exe Steam 1.0.68:
    //   Character+0x458 = MedicalSystem INLINE (by-value, NO es puntero)
    //   partArray = *(void**) (char + 0x5F8)          // = medical(0x458) + 0x1A0 → HealthPartStatus*[]
    //   partCount = *(int*)   (char + 0x5F0)          // = medical(0x458) + 0x198
    //   part_i    = *(void**) (partArray + i*8)       // array de PUNTEROS, stride 8
    //   flesh     = *(float*) (part_i + 0x40)         // fleshStun@+0x44, _maxHealth@+0x54
    // La cadena vieja [char+0x2B8]→[+0x5F8]→+0x40 era INVÁLIDA: char+0x2B8 = CharacterMemory*
    // (_myMemory, una copia) — un deref de más. 0x458+0x1A0 = 0x5F8, por eso "coincidía".
    // El motor jamás escribe salud por esa ruta (confirmado: MedicalSystem::_setHealth 0x645EF0
    // usa [rcx+0x198]/[rcx+0x1A0]; MedicalSystem::applyDamage 0x64F300 escribe [part+0x40]/[+0x44]).
    int healthPartArray = 0x5F8;  // char → HealthPartStatus** (array de punteros)
    int healthPartCount = 0x5F0;  // char → int, nº de partes (humanos = 7)
    int healthBase      = 0x40;   // HealthPartStatus → flesh (float)
    int healthStride    = 8;      // stride DENTRO del array de punteros (sizeof(void*))

    // Writable position chain (from KServerMod RE):
    // character -> AnimationClassHuman ptr (+animClassOffset)
    //   -> CharMovement ptr (+charMovementOffset from AnimClass)
    //     -> writable Vec3 (+writablePosOffset from CharMovement)
    //       -> x,y,z floats (+writablePosVecOffset within Vec3 struct)
    // Writing here actually moves the character in the physics engine.
    int animClassOffset      = -1;    // Offset to AnimationClassHuman* on character
    int charMovementOffset   = 0xC0;  // AnimClass -> CharMovement* (KServerMod verified)
    int writablePosOffset    = 0x320; // CharMovement -> writable position struct
    int writablePosVecOffset = 0x20;  // position struct -> x float

    // Squad pointer (heuristic: near faction in struct)
    int squad         = -1;      // Offset to KSquad* (discovered at runtime)

    // GameData backpointer (template/archetype data)
    int gameDataPtr   = 0x40;    // Offset to GameData* template

    // Money chain (CE-verified)
    // character+0x298 -> +0x78 -> +0x88 = money (int)
    int moneyChain1   = 0x298;
    int moneyChain2   = 0x78;
    int moneyBase     = 0x88;
};

struct SquadOffsets {
    int name           = 0x10;   // Offset to name string
    int memberList     = 0x28;   // Offset to member pointer array
    int memberCount    = 0x30;   // Offset to member count (int)
    int factionId      = 0x38;   // Offset to faction pointer/id
    int isPlayerSquad  = 0x40;   // Offset to is-player-squad flag
};

struct WorldOffsets {
    int timeOfDay      = -1;     // On TimeManager (+0x08), NOT GameWorld — use time_hooks
    int gameSpeed      = 0x700;  // GameWorld+0x700 (KenshiLib verified)
    int weatherState   = -1;     // Not yet verified on GameWorld
    // ⚠ DEPRECADO: GameWorld+0x0888 NO es la lista de personajes en 1.0.68 — es
    // 'mainUpdateListRemovalQueue' (cola de borrado, normalmente vacía). Leer aquí
    // devolvía faction=0x0 y nombres basura. La lista real del jugador se obtiene
    // ahora vía player(+0x580) -> PlayerInterface(+0x2B0 playerCharacters).
    // Se conserva solo como ÚLTIMO fallback histórico.
    int characterList  = 0x0888; // [DEPRECADO] removal queue, NO la lista de personajes
    int player         = 0x0580; // GameWorld -> PlayerInterface* (KenshiLib GameWorld.h:137)
    int buildingList   = -1;     // Not yet verified
    int paused         = 0x08B9; // GameWorld+0x08B9 — bool paused flag (RE verified)
    int zoneManager    = 0x08B0; // GameWorld+0x08B0 (KenshiLib verified)
    int characterCount = -1;     // Derived from list (lektor length at +0x00)
};

// Offsets dentro de PlayerInterface (apuntado por GameWorld+0x580).
// Verificados en KenshiLib Include/kenshi/PlayerInterface.h y Faction.h para v1.0.68.
struct PlayerInterfaceOffsets {
    int participant      = 0x02A0; // PlayerInterface -> participant (Faction*) — PlayerInterface.h:248
    // ⚠ NUEVO (audit-09, confirmado RUNTIME 2026-06-18 + bytes): controlledChar es el char que
    // el jugador REALMENTE controla (sobre el que el motor aplica las órdenes). Es DISTINTO de
    // data[0] del lektor playerCharacters. Escrito por SetControlledChar (RVA 0x802520) como
    // Faction(PI+0x2A0)+0x218[memberCount-1]; consumido en 0x50E9CF (mov rcx,[rcx+0x2A8]).
    // El mod DEBE resolver el char primario por aquí, no por data[0] (que era 'Dani' != 'Sinnombre_0').
    int controlledChar   = 0x02A8; // PlayerInterface -> controlledChar (Character*) — char activo real
    int playerCharacters = 0x02B0; // PlayerInterface -> lektor<Character*> — PlayerInterface.h:250
};

// Offsets adicionales dentro de Faction (apuntado por participant / character.faction).
// Verificados en KenshiLib Include/kenshi/Faction.h para v1.0.68.
struct FactionExtraOffsets {
    int nameStr   = 0x01A8; // Faction -> name (std::string) — Faction.h:147
    int isPlayer  = 0x0250; // Faction -> isPlayer (PlayerInterface*) — Faction.h:158
};

struct BuildingOffsets {
    int name           = 0x10;   // Building name string
    int position       = 0x48;   // Vec3 world position
    int rotation       = 0x58;   // Quat rotation
    int ownerFaction   = 0x80;   // Faction pointer
    int health         = 0xA0;   // Building health float
    int maxHealth      = 0xA4;   // Building max health float
    int isDestroyed    = 0xA8;   // Destroyed flag (bool)
    int functionality  = 0xC0;   // BuildingFunctionality pointer
    int inventory      = 0xE0;   // Inventory pointer (if has_inventory)
    int townId         = 0x100;  // Town/settlement ID
    int buildProgress  = 0x110;  // Construction progress 0.0-1.0
    int isConstructed  = 0x114;  // Fully constructed flag
};

struct InventoryOffsets {
    int items          = 0x10;   // Item array pointer (lektor<Item*> _allItems — KenshiLib Inventory.h)
    int itemCount      = 0x18;   // Number of items (int) — size del lektor en +0x18
    int width          = 0x20;   // Grid width
    int height         = 0x24;   // Grid height
    // CORRECCIÓN audit-02 (2026-06-18): owner ERA +0x28 (INCORRECTO).
    // KenshiLib Inventory.h confirma que el campo real 'owner' (RootObject*) está en +0x88
    // (callbackObject vive en +0x80; owner en +0x88; totalWeight float en +0x90).
    // El bug histórico de pickup/drop (pasar inventory* en vez del char* dueño) se arregla
    // leyendo +0x88, no +0x28. Antes: 0x28 → ahora: 0x88.
    int owner          = 0x88;   // Owner character/building pointer (RootObject*) — KenshiLib +0x88
    int maxStackMult   = 0x30;   // Stackable bonus multiplier
};

struct ItemOffsets {
    // ⚠ CORRECCIÓN audit-14 (2026-06-19): TODO ItemOffsets estaba ⛔ (offsets viejos 0x10-0x58).
    // Item == InventoryItemBase (hereda RootObjectBase). Verificado contra KenshiLib Item.h
    // (// 0xNN Member) + audit-12 (quality 0x11C, weight 0x120, quantity 0x12C en bytes).
    // El bug histórico: templateId=0x20 caía DENTRO de std::string displayName (0x18..0x40),
    // por lo que TryGetItemTemplateId enviaba BASURA por red (C2S_ItemPickup/Drop/Trade/Build).
    //   Layout real (InventoryItemBase): displayName@0x18 · data(GameData* plantilla)@0x40 ·
    //   slotType(AttachSlot)@0x110 · chargesLeft(float)@0x118 · quality(float)@0x11C ·
    //   weight(float)@0x120 · quantity(int)@0x12C · itemWidth@0x130 · itemHeight@0x134.
    int name           = 0x18;   // ✅ displayName (std::string, RootObjectBase) — era 0x10 (⛔)
    int templateId     = 0x40;   // ✅ data (GameData* plantilla, RootObjectBase) — era 0x20 (⛔, caía en displayName)
    int stackCount     = 0x12C;  // ✅ quantity (int) — era 0x30 (⛔)
    int quality        = 0x11C;  // ✅ float quality (0-100) — era 0x38 (⛔)
    int value          = -1;     // ⛔ NO existe plano (era 0x40 = data). El valor se calcula (getValueSingle RVA 0x7A7D30).
    int weight         = 0x120;  // ✅ float weight (kg) — era 0x48 (⛔)
    int equipSlot      = 0x110;  // ✅ AttachSlot slotType — era 0x50 (⛔)
    int condition      = 0x118;  // ✅ chargesLeft (float) — era 0x58 (⛔, caía en handle de RootObjectBase)
};

struct FactionOffsets {
    // ⚠ CORRECCIÓN audit-02 (2026-06-18): id ERA +0x08 (INCORRECTO Y PELIGROSO).
    // KenshiLib Faction.h confirma que en +0x08 está 'bool _antiSlavery', NO un id.
    // Faction NO tiene ningún campo 'uint32 id' plano: el identificador estable de una
    // facción es su 'name' (std::string @+0x1A8, ✅ confirmado) o su GameData* (data @+0x240,
    // que expone el string-id usado por FactionManager::getFactionByStringID).
    //
    // Leer un bool en +0x08 y enviarlo como "factionId" de red producía ids basura (0/1) que
    // colisionan entre todas las facciones → el matching cliente/servidor empareja facciones
    // equivocadas o ninguna. Esto está LIGADO al bug conocido de facciones (enemigos huyen),
    // porque C2S_FactionRelation no podía resolver los punteros correctos.
    //
    // Se pone a -1 (NO RESUELTO) en lugar de a otro offset a ciegas: NO existe un equivalente
    // uint32 trivial. Todos los call-sites ya hacen fallback graceful con `if (fIdOff >= 0)`
    // (faction_hooks, packet_handler, entity_hooks, player_controller, kenshi_sdk, core,
    // builtin_commands) → con -1 dejan de enviar/leer basura. El fix REAL (identificar la
    // facción por name/GameData en el protocolo) es trabajo de la cola de facciones (#4),
    // no de este fix de offsets.  Antes: 0x08 → ahora: -1 (pendiente RE del identificador real).
    int id             = -1;     // NO RESUELTO — +0x08 era _antiSlavery (bool), no un id. Ver nota.
    int name           = 0x1A8;  // Faction name (std::string) — KenshiLib Faction.h:147, +0x1A8 ✅
    // ⚠ audit-02: los offsets de abajo NO están confirmados contra KenshiLib (varios chocan:
    // +0x30 = allowSlavesWeapons, +0x90 = dentro de tradeCulture). Se conservan sus valores
    // por ahora porque NO se usan en rutas críticas, pero quedan marcados como dudosos.
    int members        = 0x30;   // ❓ DUDOSO (KenshiLib: +0x30 = allowSlavesWeapons, no members)
    int memberCount    = 0x38;   // ❓ DUDOSO (no confirmado)
    // ✅ CONFIRMADOS (audit-09, bytes SetControlledChar 0x80267A/0x802683 + runtime 2026-06-18):
    //   La lista REAL de miembros de la facción del jugador (de donde el motor saca controlledChar):
    //     memberCountReal @+0x210 (uint32) ; memberArrayReal @+0x218 (Character**).
    //   SetControlledChar elige memberArrayReal[memberCountReal-1] (ÚLTIMO miembro) y lo vuelca a
    //   PlayerInterface+0x2A8. Usamos estos como FALLBACK al resolver el char primario del host.
    int memberCountReal = 0x210; // Faction -> memberCount real (uint32) — confirmado RE
    int memberArrayReal = 0x218; // Faction -> memberArray real (Character**) — confirmado RE
    // ✅ CONFIRMADO (RE de bytes Steam 1.0.68, triple verificación 2026-06-19):
    //   Faction+0x78 = FactionRelations* (boost::unordered_map<Faction*,RelationData>, NO MSVC-std).
    //   La hostilidad la decide isEnemy 0x6B26D0 = (rel<=-30). ⚠ CORRECCIÓN audit-16 (2026-06-19):
    //   0x6B2630 SÍ es FactionRelations::isAlly (TRUE=ALIADO; compara rel>=+50.0f .rdata 0x1683170 y
    //   lee flag alliance@RelationData+0x0). audit-15 lo etiquetó AL REVÉS ("isEnemy enriquecida") →
    //   el hook quedó invertido y SABOTEABA el ataque. El encolador de ataque 0x6744A0 llama a ESTA
    //   isAlly (this=atacante->relations Faction+0x78) Y a Character::isAlly 0x7923D0 (char vt+0x3F0);
    //   AMBOS deben dar FALSE (no-aliado) para que el ataque se encole. 0x7923D0 NO se hookea: delega
    //   en 0x6B2630 tras sus checks propios. Es la función que se HOOKEA (polaridad isAlly corregida,
    //   ver FIX-HOSTILITY-HOOK en core.cpp).
    //   Layout boost real en FactionRelations: bucketCount @+0x38, SIZE @+0x40 (no +0x28),
    //   buckets @+0x58, defaultRelation @+0x60 (float — el FIX clave), nodo: +0x10 key, +0x1C float.
    //   Ver FactionRelationsOffsets para el detalle. Lo usa el FIX-HOSTILITY: en vez de iterar el
    //   map (recorrido boost frágil), escribe FR+0x60 = -100.0 en las facciones "Player N" → el host
    //   se vuelve enemigo de todo sin entry. SetControlledChar 0x802520 RESETEA este +0x78 al
    //   ejecutarse, así que el FIX se aplica DESPUÉS del FIX-CONTROL.
    int relations      = 0x78;   // ✅ FactionRelations* (Faction+0x78) — rango [-100,+100]
    int color1         = 0x80;   // ❓ DUDOSO (KenshiLib: +0x80 = factionOwnerships*)
    int color2         = 0x84;   // ❓ DUDOSO (no confirmado)
    // ⚠ CORRECCIÓN audit-14 (2026-06-19): isPlayerFaction ERA 0x90 (⛔: +0x90 = TradeCulture
    // inline, NO un flag de jugador → lecturas basura). El flag jugador REAL es el puntero
    // PlayerInterface* en +0x250 (== isPlayerIface, abajo): != 0 ⇒ facción de jugador.
    // IMPORTANTE: +0x250 es un PUNTERO (8 bytes), NO un bool. Los call-sites que lo consultan
    // (FactionAccessor::IsPlayerFaction, SEH_CheckIsPlayerFaction en entity_hooks,
    // player_controller) se actualizaron para leerlo como uintptr_t y comprobar != 0 — leerlo
    // como bool de 1 byte daría falsos negativos (byte bajo del puntero == 0). Antes 0x90 (⛔).
    int isPlayerFaction = 0x250; // ✅ = isPlayerIface (PlayerInterface*, != 0 ⇒ jugador) — era 0x90 (⛔)
    int money          = 0xA0;   // ❓ DUDOSO (no confirmado)
    // ✅ CONFIRMADOS (RE de bytes Steam 1.0.68, 2026-06-18) — usados por el FIX-HOSTILITY:
    int isPlayerIface  = 0x250;  // PlayerInterface* (≠0 ⇒ facción de jugador). Distingue jugador vs mundo.
    int data           = 0x240;  // GameData* (string-id estable del record FCS en GameData+0x58)
    // relCount real del map de relaciones = *(uint32*)(faction + relations[0x78] + 0x28)
};

// ── FactionManager (instancia embebida en GameWorld+0x21345B8) — RE confirmado 2026-06-18 ──
// Layout extraído de getFactionByStringID (RVA 0x2E7A20): array PLANO de Faction*, no lektor/vector.
struct FactionManagerOffsets {
    int factionCount   = 0x08;   // uint32 — nº de facciones cargadas
    int factionArray   = 0x10;   // Faction** — array contiguo de punteros a Faction
};

// ── FactionRelations (boost::unordered_map<Faction*, RelationData>, en Faction+0x78) ──
// ⚠ CORRECCIÓN CRÍTICA (RE de bytes Steam 1.0.68, triple verificación 2026-06-19):
//   El map NO es un std::_Hash de MSVC (eso era una suposición ERRÓNEA del FIX previo, que por
//   eso leía basura y NUNCA pobló nada). Es un boost::unordered::unordered_map clásico (closed
//   addressing). Lo confirma OgreUnordered.h (ogre_unordered_map = boost::unordered) + el
//   desensamblado de getRelationData (0x6B4C60) y find_or_insert (0x6B94F0).
//
//   Layout REAL de boost::table embebido en FactionRelations+0x20:
//     FR+0x38 = bucket_count (size_t)        — leído como [this+0x38] en getRelationData
//     FR+0x40 = element_count (size_t) = SIZE del map (relCount real, ANTES creíamos +0x28)
//     FR+0x58 = buckets (node**)             — array de cabezas de bucket
//   Nodo boost: +0x00 next, +0x08 hash, +0x10 key(Faction*), +0x18 RelationData{relation@+0x4}.
//     → el float 'relation' que leen isEnemy/isAlly está en nodo+0x18+0x4 = nodo+0x1C.
//
//   CAMPO CLAVE para el FIX-HOSTILITY (verificado en isEnemy 0x6B26D0 e isAlly 0x6B2630):
//     FR+0x60 = defaultFactionRelation (float). Cuando NO hay entry específica para 'other',
//     isEnemy/isAlly leen ESTE float como la relación. Escribir FR+0x60 = -100.0 hace que
//     isEnemy(faction, X)=true para CUALQUIER X sin entry → hostilidad global de la facción.
//     Es la vía MÁS ROBUSTA (4 bytes atómicos, sin alocar, sin iterar el map boost, sin hook).
struct FactionRelationsOffsets {
    int bucketCount    = 0x38;   // boost: nº de buckets (size_t)
    int size           = 0x40;   // boost: element_count = nº de entradas (relCount real) ⚠ era 0x28
    int buckets        = 0x58;   // boost: node** array de cabezas de bucket
    int defaultRelation = 0x60;  // ✅ float leído por isEnemy/isAlly cuando no hay entry (FIX clave)
    // Nodo boost: +0x00 next, +0x08 hash, +0x10 key(Faction*), +0x18 RelationData (relation @+0x4).
    int nodeNext       = 0x00;
    int nodeKey        = 0x10;
    int nodeRelation   = 0x1C;   // = nodo+0x18 (RelationData) + 0x4 (relation) — float que ve el motor
};

struct GameDataOffsets {
    int id             = 0x08;   // Template ID (uint32_t)
    int managerPtr     = 0x10;   // GameDataManager* backpointer
    int name           = 0x28;   // Template name (Kenshi std::string)
    // ✅ CONFIRMADO (RE de bytes Steam 1.0.68, audit-15 2026-06-19, cruce con KenshiLib GameData.h):
    //   GameData+0x58 = stringID (Kenshi std::string) = el ID ESTABLE del record FCS
    //   (p.ej. "204-gamedata.base" para la facción del jugador vanilla 'Nameless').
    //   ⚠ NO confundir con +0x28 (name, legible para humanos) ni con la vieja suposición +0x18.
    //   Lo usa el hook del gate de combate (FIX-HOSTILITY-HOOK) para localizar la facción Nameless
    //   por su stringID y heredar sus relaciones reales (vía Faction+0x240 → GameData+0x58).
    int stringID       = 0x58;   // ✅ Kenshi std::string — string-id estable del record (getFactionByStringID)
};

struct TimeManagerOffsets {
    int timeOfDay      = 0x08;   // Float 0.0-1.0 (day cycle)
    int gameSpeed      = 0x10;   // Float (current game speed multiplier)
};

struct StatsOffsets {
    // Core combat stats
    int meleeAttack    = 0x00;   // Melee attack skill (float 0-100)
    int meleeDefence   = 0x04;   // Melee defence skill
    int dodge          = 0x08;   // Dodge skill
    int martialArts    = 0x0C;   // Martial arts skill
    int strength       = 0x10;   // Strength
    int toughness      = 0x14;   // Toughness
    int dexterity      = 0x18;   // Dexterity
    int athletics      = 0x1C;   // Athletics (movement speed)
    // Ranged
    int crossbows      = 0x20;   // Crossbow skill
    int turrets        = 0x24;   // Turret skill
    int precision      = 0x28;   // Precision shooting
    // Stealth
    int stealth        = 0x30;   // Stealth skill
    int assassination  = 0x34;   // Assassination skill
    int lockpicking    = 0x38;   // Lockpicking
    int thievery       = 0x3C;   // Thievery
    // Science/Tech
    int science        = 0x40;   // Science
    int engineering    = 0x44;   // Engineering / robotics
    int medic          = 0x48;   // Field medic
    // Labor
    int farming        = 0x50;   // Farming
    int cooking        = 0x54;   // Cooking
    int weaponsmith    = 0x58;   // Weapon smithing
    int armoursmith    = 0x5C;   // Armour smithing
    int labouring      = 0x60;   // Labouring (mining, hauling)
};

// Combined offsets structure
struct GameOffsets {
    CharacterOffsets        character;
    SquadOffsets            squad;
    WorldOffsets            world;
    BuildingOffsets         building;
    InventoryOffsets        inventory;
    ItemOffsets             item;
    FactionOffsets          faction;
    PlayerInterfaceOffsets  playerInterface; // GameWorld+0x580 -> PlayerInterface
    FactionExtraOffsets     factionExtra;    // name/isPlayer offsets dentro de Faction
    FactionRelationsOffsets factionRelations;// boost::unordered_map en Faction+0x78 (size/default/nodo)
    StatsOffsets            stats;
    GameDataOffsets         gameData;
    TimeManagerOffsets      timeManager;

    // Version string found in memory (for validation)
    char gameVersion[32] = {};

    // Whether offsets were discovered by scanner (vs hardcoded)
    bool discoveredByScanner = false;
};

// Singleton accessor for offsets
GameOffsets& GetOffsets();

// Initialize offsets from scanner results (call early in startup)
void InitOffsetsFromScanner();

// PlayerBase bridge: set by Core after pattern resolution, read by CharacterIterator.
uintptr_t GetResolvedPlayerBase();
void SetResolvedPlayerBase(uintptr_t addr);

// GameWorld bridge: set by Core after pattern resolution, read by CharacterIterator as fallback.
uintptr_t GetResolvedGameWorld();
void SetResolvedGameWorld(uintptr_t addr);

// Getter DIRECTO de la facción del jugador, SIN iterar la lista de personajes.
// Resuelve: GameWorld -> +0x580 (player/PlayerInterface*) -> +0x2A0 (participant) = Faction*.
// Devuelve 0 si la cadena no es válida (cada paso se valida como puntero de heap).
// Es la fuente PRIMARIA de facción: no depende de que la lista de personajes esté poblada.
uintptr_t GetPlayerFactionDirect();

// Getter DIRECTO del personaje PRIMARIO del jugador (el que controla), SIN iterar por nombre.
// Resuelve: GameWorld -> +0x580 (player) -> +0x2B0 (playerCharacters lektor) -> data[0].
// Devuelve 0 si la lista aún no está poblada (justo tras la carga) — reintentar por tick.
// Vía ROBUSTA para el flujo connected-then-load (no depende del nombre "Player N").
uintptr_t GetPlayerPrimaryCharacterDirect();

// [FIX-GHOST 2026-07] Comprueba si charPtr está en la lista NATIVA de personajes
// del jugador (PlayerInterface+0x2B0, lektor<Character*>) — la fuente de verdad
// del motor. Si un Character NO está aquí, NO es del jugador aunque su nombre
// encaje con el patrón "Player N" (evita reclamar NPCs fantasma del mundo).
// Devuelve:  1 = está en la lista;  0 = lista legible pero NO está;
//           -1 = lista no disponible (juego sin cargar / lektor sin poblar).
int IsInPlayerCharactersList(uintptr_t charPtr);

// Resultado de FixCharacterFactionTo (ver abajo). Permite al orquestador decidir
// cuándo dar por arreglado el char del host y dejar de reintentar.
enum class FixFactionResult {
    InvalidChar,      // char nulo / no alineado / vtable fuera del módulo
    NoPlayerFaction,  // playerFaction no es un puntero de heap válido (no hay fuente)
    AlreadyCorrect,   // char.faction(+0x10) ya == playerFaction (no se tocó nada)
    Fixed,            // se escribió y la relectura confirma faction == playerFaction
    WriteFailed,      // se escribió pero la relectura NO coincide (memoria protegida?)
    Exception,        // SEH: el char se liberó o la memoria no es accesible
};

// ── FixCharacterFactionTo ──
// Arregla la facción del personaje del HOST escribiendo la PLAYER faction válida en
// char+0x10 si no coincide ya. Es la base del fix de combate: el motor reconoce al
// jugador con char.faction(+0x10) == gameWorld.player(+0x580).faction. Si char+0x10
// es NULL/incorrecta, el motor NO te trata como jugador y rechaza tus órdenes de ataque.
//
// 'playerFaction' debe venir de GetPlayerFactionDirect() (GameWorld+0x580 -> +0x2A0).
// SEGURO para el host: la player faction vive toda la partida (no se descarga con
// zonas), a diferencia de las facciones de NPC del caso remoto (que causaban UAF).
// Protegido con SEH; loguea SIEMPRE [DIAG-FAC] con faction antes/después y si coincide.
FixFactionResult FixCharacterFactionTo(void* charPtr, uintptr_t playerFaction);

// VOLCADO DE DIAGNÓSTICO [DIAG] — SONDA v2, solo log, NO cambia comportamiento.
// Resuelve gwObj (GameWorld) y pbObj (PlayerBase/PlayerInterface) y escanea EN RANGO:
//   A) pbObj +0x00..+0x400 como Faction*; B) gwObj +0x400..+0xA00 (PlayerIface* y Faction*);
//   C) char[0].faction (char+0x10, VERIFICADO) = facción del jugador con CERTEZA.
// Filtra candidatos con un test de string LEGIBLE (>=3 chars, >=80% ASCII imprimible).
// Loguea cada Faction* candidato en hex para cruzarlos. Máx 6 volcados globales.
void DiagDumpPlayerFaction();

// Bombea la sonda desde el game tick (OnGameTick). Throttle de 2s REALES (steady_clock).
// Solo dispara cuando el PlayerBase resuelto es un puntero de heap válido (el player ya existe).
// Llamar una vez por tick en AMBAS ramas de OnGameTick (sync + legacy).
void DiagTickPump();

// Loading state bridge: set by Core when entering/exiting Loading phase.
// CharacterIterator checks this and skips game memory reads during loading
// to prevent heap corruption from non-atomic lektor reads.
bool IsGameLoading();
void SetGameLoadingState(bool loading);

// SetPosition bridge: set by Core with the resolved CharacterSetPosition function ptr.
void SetGameSetPositionFn(void* fn);

// SetPaused bridge: set by Core con el puntero al setter OFICIAL GameWorld::setPaused
// (RVA 0x787D40 en 1.0.68). Si está disponible, GameWorldAccessor::SetPaused lo invoca
// en lugar de escribir GameWorld+0x8B9 a pelo — así refresca los caches de pausa de los
// subsistemas (obj+0xB8), oculta el cartel "PAUSED" del HUD y emite "Resume_Game".
// Esto arregla la "pausa fantasma" que bloqueaba las órdenes del jugador (atacar/hablar).
void SetGameSetPausedFn(void* fn);
bool HasGameSetPausedFn();

// ── Deferred AnimClass Probing ──
// Schedule a character for animClassOffset discovery on subsequent game ticks.
// The probe needs a non-zero cached position to validate the chain, so freshly
// spawned characters may need several frames before the position settles.
void ScheduleDeferredAnimClassProbe(uintptr_t charPtr);

// Process all deferred probes. Call once per game tick from game_tick_hooks.
// Returns true if animClassOffset was successfully discovered.
bool ProcessDeferredAnimClassProbes();

// Reset all probe statics (animClass, equipment, writePos logging, deferred queue).
// Call on disconnect/reconnect or second game load to prevent stale state.
void ResetProbeState();

// ── Player Controlled Offset Discovery ──
// Discovers the isPlayerControlled bool offset by comparing a known player-controlled
// character with an NPC. Call after game load when both types are available.
void ProbePlayerControlledOffset(uintptr_t playerCharPtr, uintptr_t npcCharPtr);

// Write the isPlayerControlled flag on a character (requires discovered offset).
bool WritePlayerControlled(uintptr_t charPtr, bool controlled);

// ── Unified Runtime Offset Prober ──
// Discovers sceneNode, isPlayerControlled, aiPackage, equipment, animClassOffset, squad
// offsets at runtime by probing live game objects. Caches results to disk.
// Include game_offset_prober.h for the full API (RunOffsetProber, LoadOffsetCache, etc.).

// ═══════════════════════════════════════════════════════════════════════════
//  GAME ENUMS (from fcs_enums.def)
// ═══════════════════════════════════════════════════════════════════════════

// Task types - subconjunto de los 291 taskTypes del enum nativo relevantes para multijugador.
// FUENTE DE VERDAD: KenshiLib Enums.h (GPL-3.0) `enum TaskType` (índices secuenciales desde 0),
//   verificada por convergencia con los literales del binario que ya usa producción
//   (p.ej. USE_BED=0x62 en core.cpp, RANGED_ATTACK=0x106 en combat_hooks.cpp canUseArms).
// ⚠ CORRECCIÓN 2026-07-13 (game-reverse-engineer): los valores >= IDLE estaban DESPLAZADOS
//   respecto al binario real (off-by-one/varios) — ver auditoría en la wiki KENSHI. El enum NO
//   se usaba por scope salvo TaskType::NULL_TASK (=0, intacto), así que la corrección NO altera
//   ningún fix desplegado; solo elimina una trampa latente para código futuro y para
//   CharacterAccessor::GetCurrentTask() (que devuelve el valor CRUDO del binario).
//   Índices decimales anotados = valor real del binario (== KenshiLib).
enum class TaskType : uint32_t {
    NULL_TASK = 0,
    MOVE_ON_FREE_WILL = 1,
    BUILD = 2,
    PICKUP = 3,
    MELEE_ATTACK = 4,
    FOCUSED_MELEE_ATTACK = 5,
    EQUIP_WEAPON = 6,
    UNEQUIP_WEAPON = 7,
    CHOOSE_ENEMY_AND_ATTACK = 9,
    IDLE = 14,                    // era 13 (mal)
    PROTECT_ALLIES = 15,          // era 14 (mal)
    ATTACK_ENEMIES = 16,          // era 15 (mal)
    PATROL = 22,                  // era 21 (mal)
    FIRST_AID_ORDER = 25,         // era 26 (mal)
    LOOT_TARGET = 26,             // era 27 (mal)
    HOLD_POSITION = 30,           // era 31 (mal)
    FOLLOW_PLAYER_ORDER = 44,     // era 45 (mal)
    OPERATE_MACHINERY = 87,       // era 88 (mal)
    USE_TRAINING_DUMMY = 97,      // era 98 (mal)
    USE_BED = 98,                 // era 99 (mal) — coincide con kTT_USE_BED=0x62 en core.cpp
    USE_TURRET = 146,             // era 148 (mal)
    MAN_A_TURRET = 149,           // era 151 (mal)
    RANGED_ATTACK = 262,          // era 278 (mal) — 0x106, coincide con canUseArms en combat_hooks.cpp
    SHOOT_AT_TARGET = 235,        // era 244 (mal)
};

// Attach slots for equipment (from fcs_enums.def)
enum class AttachSlot : uint8_t {
    WEAPON    = 0,
    BACK      = 1,
    HAIR      = 2,
    HAT       = 3,
    EYES      = 4,
    BODY      = 5,
    LEGS      = 6,
    NONE      = 7,
    SHIRT     = 8,
    BOOTS     = 9,
    GLOVES    = 10,
    NECK      = 11,
    BACKPACK  = 12,
    BEARD     = 13,
    BELT      = 14,
};

// NPC type classification
enum class CharacterType : uint8_t {
    OT_NONE       = 0,
    OT_MILITARY   = 1,
    OT_CIVILIAN   = 2,
    OT_TRADER     = 3,
    OT_SLAVE      = 4,
    OT_NOBLE      = 5,
    OT_BANDIT     = 6,
    OT_ANIMAL     = 7,
};

// Weather affecting types
enum class WeatherType : uint8_t {
    WA_NONE       = 0,
    WA_RAIN       = 1,
    WA_ACID_RAIN  = 2,
    WA_DUST_STORM = 3,
    WA_GAS        = 4,
};

// Building function types
enum class BuildingFunction : uint8_t {
    BF_ANY           = 0,
    BF_BED           = 1,
    BF_CAGE          = 2,
    BF_STORAGE       = 3,
    BF_CRAFTING      = 4,
    BF_RESEARCH      = 5,
    BF_TURRET        = 6,
    BF_GENERATOR     = 7,
    BF_FARM          = 8,
    BF_MINE          = 9,
    BF_TRAINING      = 10,
};

// ═══════════════════════════════════════════════════════════════════════════
//  CHARACTER ACCESSOR
// ═══════════════════════════════════════════════════════════════════════════
// Safe accessor that reads game memory using the offset table.

class CharacterAccessor {
public:
    explicit CharacterAccessor(void* characterPtr)
        : m_ptr(reinterpret_cast<uintptr_t>(characterPtr)) {}

    bool IsValid() const { return m_ptr != 0; }

    Vec3 GetPosition() const;
    Quat GetRotation() const;
    float GetHealth(BodyPart part) const;
    bool IsAlive() const;
    bool IsPlayerControlled() const;
    float GetMoveSpeed() const;
    uint8_t GetAnimState() const;

    // Read the character's name (MSVC std::string)
    std::string GetName() const;

    // Write the character's name (SSO-safe for names <= 15 chars, heap for longer)
    bool WriteName(const std::string& name);

    // Write the name to the GameData template too (some UI reads from template)
    bool WriteNameToGameData(const std::string& name);

    // Get the inventory pointer for InventoryAccessor
    uintptr_t GetInventoryPtr() const;

    // Get the faction pointer
    uintptr_t GetFactionPtr() const;

    // Write faction pointer (point character to a different faction)
    bool WriteFaction(uintptr_t factionPtr);

    // Get the GameData* template pointer (backpointer at +0x40)
    uintptr_t GetGameDataPtr() const;

    // Write the character's writable (physics) position.
    bool WritePosition(const Vec3& pos);

    // Get current task type
    TaskType GetCurrentTask() const;

    // Get stats pointer for StatsAccessor
    uintptr_t GetStatsPtr() const;

    // Get equipment slot item pointer
    uintptr_t GetEquipmentSlot(EquipSlot slot) const;

    // Get squad pointer (if character is in a squad)
    uintptr_t GetSquadPtr() const;

    // Get AI package pointer
    uintptr_t GetAIPackagePtr() const;

    // Get money via pointer chain
    int GetMoney() const;

    // Set the isPlayerControlled flag (requires discovered offset from ProbePlayerControlledOffset)
    bool SetPlayerControlled(bool controlled);

    // Get raw pointer for comparison
    uintptr_t GetPtr() const { return m_ptr; }

private:
    uintptr_t m_ptr;
};

// ═══════════════════════════════════════════════════════════════════════════
//  SQUAD ACCESSOR
// ═══════════════════════════════════════════════════════════════════════════

class SquadAccessor {
public:
    explicit SquadAccessor(void* squadPtr)
        : m_ptr(reinterpret_cast<uintptr_t>(squadPtr)) {}

    bool IsValid() const { return m_ptr != 0; }

    std::string GetName() const;
    int GetMemberCount() const;
    uintptr_t GetMember(int index) const;
    uintptr_t GetFactionPtr() const;
    bool IsPlayerSquad() const;

    uintptr_t GetPtr() const { return m_ptr; }

private:
    uintptr_t m_ptr;
};

// ═══════════════════════════════════════════════════════════════════════════
//  INVENTORY ACCESSOR
// ═══════════════════════════════════════════════════════════════════════════

class InventoryAccessor {
public:
    explicit InventoryAccessor(void* invPtr)
        : m_ptr(reinterpret_cast<uintptr_t>(invPtr)) {}
    explicit InventoryAccessor(uintptr_t invAddr)
        : m_ptr(invAddr) {}

    bool IsValid() const { return m_ptr != 0; }

    int GetItemCount() const;
    uintptr_t GetItem(int index) const;
    int GetWidth() const;
    int GetHeight() const;

    // Modify quantity of existing item stack (cannot create new items)
    bool AddItem(uint32_t templateId, int quantity);
    bool RemoveItem(uint32_t templateId, int quantity);
    bool SetEquipment(EquipSlot slot, uint32_t templateId);

    uintptr_t GetPtr() const { return m_ptr; }

private:
    uintptr_t m_ptr;
};

// ═══════════════════════════════════════════════════════════════════════════
//  BUILDING ACCESSOR
// ═══════════════════════════════════════════════════════════════════════════

class BuildingAccessor {
public:
    explicit BuildingAccessor(void* bldPtr)
        : m_ptr(reinterpret_cast<uintptr_t>(bldPtr)) {}

    bool IsValid() const { return m_ptr != 0; }

    std::string GetName() const;
    Vec3 GetPosition() const;
    float GetHealth() const;
    float GetMaxHealth() const;
    bool IsDestroyed() const;
    float GetBuildProgress() const;
    bool IsConstructed() const;
    uintptr_t GetOwnerFaction() const;
    uintptr_t GetInventoryPtr() const;

    uintptr_t GetPtr() const { return m_ptr; }

private:
    uintptr_t m_ptr;
};

// ═══════════════════════════════════════════════════════════════════════════
//  FACTION ACCESSOR
// ═══════════════════════════════════════════════════════════════════════════

class FactionAccessor {
public:
    explicit FactionAccessor(void* factionPtr)
        : m_ptr(reinterpret_cast<uintptr_t>(factionPtr)) {}

    bool IsValid() const { return m_ptr != 0; }

    std::string GetName() const;
    int GetMemberCount() const;
    bool IsPlayerFaction() const;
    int GetMoney() const;

    uintptr_t GetPtr() const { return m_ptr; }

private:
    uintptr_t m_ptr;
};

// ═══════════════════════════════════════════════════════════════════════════
//  STATS ACCESSOR
// ═══════════════════════════════════════════════════════════════════════════

class StatsAccessor {
public:
    explicit StatsAccessor(void* statsPtr)
        : m_ptr(reinterpret_cast<uintptr_t>(statsPtr)) {}

    bool IsValid() const { return m_ptr != 0; }

    float GetMeleeAttack() const;
    float GetMeleeDefence() const;
    float GetDodge() const;
    float GetMartialArts() const;
    float GetStrength() const;
    float GetToughness() const;
    float GetDexterity() const;
    float GetAthletics() const;
    float GetStealth() const;
    float GetCrossbows() const;
    float GetMedic() const;

    uintptr_t GetPtr() const { return m_ptr; }

private:
    uintptr_t m_ptr;
};

// ═══════════════════════════════════════════════════════════════════════════
//  CHARACTER LIST ITERATOR
// ═══════════════════════════════════════════════════════════════════════════
// Iterates over all characters in the game world

class CharacterIterator {
public:
    CharacterIterator();

    bool HasNext() const;
    CharacterAccessor Next();
    void Reset();

    int Count() const { return m_count; }

private:
    uintptr_t m_listBase = 0;
    int       m_index    = 0;
    int       m_count    = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
//  GAME WORLD ACCESSOR
// ═══════════════════════════════════════════════════════════════════════════
// Reads global game state from the GameWorld singleton

class GameWorldAccessor {
public:
    explicit GameWorldAccessor(uintptr_t worldSingletonAddr)
        : m_addr(worldSingletonAddr) {}

    bool IsValid() const { return m_addr != 0; }

    float GetTimeOfDay() const;    // 0.0-1.0 (0=midnight, 0.5=noon)
    float GetGameSpeed() const;    // 1.0=normal
    int GetWeatherState() const;

    // Write time of day directly (for server sync)
    bool WriteTimeOfDay(float time);
    // Write game speed directly (for server sync)
    bool WriteGameSpeed(float speed);

    // ── Pausa (GameWorld+0x8B9, bool de 1 byte — CONFIRMADO en KenshiLib GameWorld.h
    //    y en el binario 1.0.68: isPaused @RVA 0xDEE00 hace movzx eax,[rcx+8B9h];
    //    el game loop @RVA 0x788A00 hace cmp byte [rsi+8B9h],0 y, si está pausado,
    //    fuerza el delta de simulación a 0 — congelando IA/combate/movimiento).
    //    Estos métodos usan ResolveWorldObject() para manejar el layout de instancia
    //    embebida de Steam 1.0.68 (NO se puede dereferenciar a ciegas el singleton). ──
    // Devuelve: 1=pausado, 0=despausado, -1=desconocido (no se pudo resolver/leer).
    int  GetPausedRaw() const;
    // Escribe el flag paused. Devuelve true si la escritura tuvo éxito.
    bool SetPaused(bool paused);
    // Expone la dirección del OBJETO GameWorld real (instancia o *puntero), o 0.
    // Útil para sondas de diagnóstico [DIAG] sin duplicar la lógica de resolución.
    uintptr_t GetWorldObject() const { return ResolveWorldObject(); }

private:
    uintptr_t m_addr;  // Direccion resuelta: puede ser la INSTANCIA GameWorld embebida
                       // (1.0.68, m_addr ES el objeto) o un puntero clasico a GameWorld.

    // Resuelve el OBJETO GameWorld real a partir de m_addr. Maneja ambos layouts:
    //   - instancia embebida (1.0.68): *m_addr es la vtable (.text) -> el objeto es m_addr
    //   - puntero clasico             : *m_addr es heap-ptr al objeto -> el objeto es *m_addr
    // Devuelve 0 si no hay objeto valido. Definido en game_world.cpp.
    uintptr_t ResolveWorldObject() const;
};

// ═══════════════════════════════════════════════════════════════════════════
//  FUNCTION POINTER TYPEDEFS
// ═══════════════════════════════════════════════════════════════════════════
// Signatures for hooked game functions. __fastcall = Microsoft x64 calling convention.

namespace func_types {

// Entity lifecycle
using CharacterCreateFn   = void*(__fastcall*)(void* factory, void* templateData);
using CharacterDestroyFn  = void(__fastcall*)(void* nodeList, void* building);
using CreateRandomSquadFn = void*(__fastcall*)(void* factory, void* squadTemplate);
using CharacterSerialiseFn = void(__fastcall*)(void* character, void* stream);

// Movement
using SetPositionFn       = void(__fastcall*)(void* havokChar, float x, float y, float z);
using MoveToFn            = void(__fastcall*)(void* character, float x, float y, float z, int moveType);

// Combat
using ApplyDamageFn       = void(__fastcall*)(void* target, void* attacker, int bodyPart, float cut, float blunt, float pierce);
using StartAttackFn       = void(__fastcall*)(void* attacker, void* target, void* weapon);
using CharacterDeathFn    = void(__fastcall*)(void* character, void* killer);
using CharacterKOFn       = void(__fastcall*)(void* character, void* attacker, int reason);
using HealthUpdateFn      = void(__fastcall*)(void* character);
using MartialArtsCombatFn = void(__fastcall*)(void* attacker, void* target);

// World / Zones
using ZoneLoadFn          = void(__fastcall*)(void* zoneMgr, int zoneX, int zoneY);
using ZoneUnloadFn        = void(__fastcall*)(void* zoneMgr, int zoneX, int zoneY);
using BuildingPlaceFn     = void(__fastcall*)(void* world, void* building, float x, float y, float z);
using BuildingDestroyedFn = void(__fastcall*)(void* building);

// Game loop / Time
using GameFrameUpdateFn   = void(__fastcall*)(void* rcx, void* rdx);
using TimeUpdateFn        = void(__fastcall*)(void* timeManager, float deltaTime);

// Save / Load
using SaveGameFn          = void(__fastcall*)(void* saveManager, const char* saveName);
using LoadGameFn          = void(__fastcall*)(void* saveManager, const char* saveName);
using ImportGameFn        = void(__fastcall*)(void* saveManager, const char* saveName);

// Squad / Platoon
using SquadCreateFn       = void*(__fastcall*)(void* squadManager, void* templateData);
using SquadAddMemberFn    = void(__fastcall*)(void* squad, void* character);

// Inventory / Items
using ItemPickupFn        = void(__fastcall*)(void* inventory, void* item, int quantity);
using ItemDropFn          = void(__fastcall*)(void* inventory, void* item);
using BuyItemFn           = void(__fastcall*)(void* buyer, void* seller, void* item, int quantity);

// Faction / Diplomacy
using FactionRelationFn   = void(__fastcall*)(void* factionA, void* factionB, float relation);

// AI
using AICreateFn          = void*(__fastcall*)(void* character, void* faction);
using AIPackagesFn        = void(__fastcall*)(void* character, void* aiPackage);

// Turret
using GunTurretFn         = void(__fastcall*)(void* turret, void* operator_);
using GunTurretFireFn     = void(__fastcall*)(void* turret, void* target);

} // namespace func_types

} // namespace kmp::game
