# Auditoría 02 — Offsets/RVAs verificados contra el binario (Kenshi 1.0.68 Steam)

> **Objetivo:** verificar/encontrar por análisis estático los offsets y RVAs marcados como
> DUDOSOS o NO RESUELTOS en `03-scanner.md`, `06-game-offsets.md`, `02-hooks.md`.
>
> **Binario:** `E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe`
> (Steam 1.0.68, 36 718 592 bytes, PE32+ MSVC x64, ImageBase `0x140000000`).
>
> **Método:** análisis estático read-only con `pefile` + `iced-x86`/desensamblado manual.
> Resolución de funciones por **string ancla (.rdata) → xref RIP-relativo (LEA/MOV) en
> .text → función contenedora vía `.pdata` (RUNTIME_FUNCTION)**. Singletons por conteo de
> xrefs absolutos `lea/mov reg,[RIP+disp]`. `.pdata` tiene 77 108 entradas → límites de
> función autoritativos.
>
> **Estructuras:** cruzadas con `E:\Aplicaciones\ref-KenshiLib` (headers con offsets de
> campo + RVAs). ⚠️ Las **RVAs de KenshiLib son de OTRA versión** (no Steam 1.0.68): p. ej.
> `process` en KenshiLib = `0x580FF0` pero en el binario Steam = `0x581770`. Por eso las RVAs
> de FUNCIÓN se verificaron SIEMPRE contra el binario; los **offsets de CAMPO de KenshiLib SÍ
> son portables** y se confirmaron por frecuencia de acceso en .text.
>
> Layout PE confirmado: `.text` 0x1000–0x1672600 · `.rdata` 0x1673000–0x1BBD600 ·
> `.data` 0x1BBE000–0x214C000 · `.pdata` 0x214C000.

---

## TABLA 1 — RVAs de FUNCIÓN (símbolo → RVA verificada → método/confianza)

| Símbolo | RVA 1.0.68 verificada | Prólogo | Tamaño | Método / Confianza |
|---|---|---|---|---|
| **CharacterSpawn** (`RootObjectFactory::process`) | **0x581770** | `48 8B C4` (mov rax,rsp) | 6410 B | String `[RootObjectFactory::process] Character '`→LEA@0x582324→.pdata. ✅ **CONFIRMADO** |
| **StartAttack** (Cutting damage) | **0x7B2A20** | `48 8B C4` | 9253 B | String `Cutting damage`→LEA@0x7B36A2→.pdata. ✅ **CONFIRMADO — gana el orchestrator, NO 0x7A1650** |
| **CharacterMoveTo** | func real **0x2EF4A0**; el `0x2EF4E3` es **mid-function (+0x43)** | (mid) | — | Patrón `48 89 4C 24..FF 15` cae a +0x43 dentro de 0x2EF4A0. ✅ **CONFIRMADO mid-function → no hookear** |
| **CharAnimUpdate** (sitio inline) | sitio **0x65F6C7** dentro de func **0x65F160** (size 1920) | func: `48 8B C4` | — | `.pdata::FindContaining(0x65F6C7)`=0x65F160. Sitio inline confirmado mid-function. 🟡 **CONFIRMADO como sitio de hook inline** |
| CreateRandomSquad | **0x583A10** | `48 8B C4` | 10997 B | String `createRandomSquad] Missing squad leader`→LEA@0x5844CE. ✅ |
| BuildingPlace (`createBuilding`) | **0x57CC70** | `48 8B C4` | 9584 B | String `createBuilding] Building`→LEA@0x57EE44. ✅ |
| AICreate (`AI::create`) | **0x622110** | `40 57 48 81` | 313 B | String `[AI::create] No faction for`→LEA@0x6221D2. ✅ |
| CharacterDestroy (`NodeList::destroyNodesByBuilding`) | **0x38A720** | `48 8B C4 44` | 785 B | String→LEA@0x38A9A0 + patrón único. ✅ |
| CharacterSetPosition (`HavokCharacter::setPosition`) | **0x145E50** | `48 8B C4` | ~1014 B | Inicio de función válido (`.pdata` 0x145E50–0x146246). ✅ |
| ApplyDamage (Attack damage effect) | **0x7A33A0** | `48 8B C4` | 6925 B | String `Attack damage effect`→LEA@0x7A43CD. ✅ |
| TimeUpdate | **0x214B50** | `40 55 56 48` | 57 B | Patrón orch único (1 hit). ✅ (el string `timeScale` referencia getters vecinos 0x214D00/0x219DA9, no la fn principal) |
| GameFrameUpdate | **0x123A10** | `48 8B C4 55` | — | Patrón **45 bytes** = 1 hit (32 bytes daba 2: +falso 0x788100). ✅ |
| ZoneLoad | **0x377710** | `40 55 56 57` | 1521 B | String `zone.%d.%d.zone`→LEA@0x377C61 + patrón. ✅ |

### Discrepancias resueltas

1. **StartAttack 0x7B2A20 vs 0x7A1650** → **0x7B2A20 es la correcta.** El string `Cutting damage`
   (RVA 0x17157E8) está referenciado por un LEA dentro de la función 0x7B2A20 (size 9253,
   `mov rax,rsp`). El `0x7A1650` del comentario en `combat_hooks.cpp:405` es **erróneo** (no
   contiene ese string ni es la fn de iniciar ataque). **Acción sugerida:** corregir el
   comentario; el orchestrator ya usa 0x7B2A20.

2. **CharacterMoveTo 0x2EF4E3** → es **+0x43 dentro de la función real 0x2EF4A0**. Confirmado
   mid-function (el patrón `FF 15` IAT-call cae ahí). Mantener DESHABILITADO. Si algún día se
   quisiera hookear, el inicio real es **0x2EF4A0** (no 0x2EF4E3).

3. **`tracked:0` / `CharacterCreate:0`** → las RVAs de spawn/anim están **todas correctas en el
   binario**. Si el scanner devuelve 0, la causa NO es la RVA sino la resolución por string:
   `CharacterSpawn` requiere `.pdata::FindContaining` (el walk-back de 16 KB del camino B falla,
   ya documentado). `CharAnimUpdate` es un sitio inline mid-function (0x65F6C7) que sólo se
   resuelve por RVA directa/patrón inline, no por `.pdata` (no es inicio de función).

---

## TABLA 2 — Patrones AOB (unicidad verificada en .text 1.0.68)

| Patrón | Hits | RVAs | Estado |
|---|---|---|---|
| GameFrameUpdate (32 bytes) | **2** | 0x123A10, 0x788100 | ❌ NO único |
| GameFrameUpdate (45 bytes) | **1** | 0x123A10 | ✅ único (extensión correcta) |
| AICreate (29 bytes) | **2** | 0xAF870, 0x622110 | ❌ NO único |
| AICreate (41 bytes) | **1** | 0x622110 | ✅ único (extensión correcta) |
| CharacterDestroy | **1** | 0x38A720 | ✅ único |
| TimeUpdate (orch) | **1** | 0x214B50 | ✅ único |

Las extensiones de patrón documentadas en `pattern-verification.md` funcionan **exactamente**
como se describió (45/41 tokens eliminan el falso positivo). Re-verificar sólo si cambia el binario.

---

## TABLA 3 — Singletons (instancia/puntero verificados)

| Símbolo | RVA | Sección | Evidencia | Confianza |
|---|---|---|---|---|
| **GameWorld** (instancia embebida) | **0x2134110** | `.data` | **513 `lea reg,[0x2134110]`, 0 `mov`** → objeto embebido, no puntero. Valor estático=0 (vtable se instala en runtime). | ✅ **CONFIRMADO** |
| GameWorld vtable | **0x1722608** | `.rdata` | slot0=0x140026724 (→.text `mainLoop_GPUSensitiveStuff`); COL@vt-8=0x14185A0F0 (→.rdata 0x185A0F0, RTTI `.?AVGameWorld@@`). | ✅ |
| PlayerBase (puntero global) | **0x1AC8A90** | `.rdata` | 0 xrefs en .text (se rellena en runtime); valor estático `0x0002EF37…`=basura. | 🟡 (validar en runtime, como ya hace el mod) |
| GameWorld RVAs antiguas | 0x2131020 (1.0.65), 0x2133040 (GOG) | `.data` | **0 lea / 0 mov** → muertas en 1.0.68. | ⛔ no usar |

### Anchor `dayTime` — confirmado INEXISTENTE

`dayTime` y `[GameWorld::` **NO existen** en el binario 1.0.68 (búsqueda en .rdata = NOT FOUND).
Por eso el StringXref de GameWorld nunca encuentra nada. **Strings que SÍ existen** para anclar
GameWorld/TimeManager:
- **`timeScale`** → RVA **0x16A6F48** ← **usar este como anchor** (recomendado).
- `zone.%d.%d.zone` → 0x16C4CC0.
- `Kenshi 1.0.68` → 0x1692288.

**Acción sugerida:** sustituir el anchor `dayTime` por `timeScale` en `RegisterBuiltinPatterns`
(entradas `GameWorldSingleton` y `TimeUpdate`). GameWorld no depende del anchor para resolverse
(usa hardcoded 0x2134110 + disasm), pero `timeScale` haría el descubrimiento-por-string real.

---

## TABLA 4 — Offsets de ESTRUCTURA verificados (KenshiLib + frecuencia de acceso en .text)

> Fuente primaria: headers de KenshiLib (offsets de campo = portables entre versiones).
> Verificación cruzada: conteo de accesos `mov/lea reg,[reg+disp32]` en .text 1.0.68.

### Character (✅ confirmados por KenshiLib `Character.h` + frecuencia)
| Offset | Campo | Antes (mod) | Verificado | Accesos .text |
|---|---|---|---|---|
| `+0x10` | faction (en RootObject) | ✅ 0x10 | ✅ | — |
| `+0x18` | name (std::string) | ✅ | ✅ | — |
| `+0x2E8` | inventory (Inventory*) | ✅ | ✅ | 182 |
| `+0x450` | stats (CharStats*, **puntero**, NO inline) | "inline" | ⚠️ **es puntero `CharStats*`**, no struct inline | 298 |
| `+0x640` | movement (CharMovement*) | — | ✅ nuevo | 199 |
| `+0x648` | body (CharBody*) | — | ✅ nuevo | 90 |
| `+0x650` | ai (AI*) | — | ✅ nuevo | 228 |
| **`+0x658`** | **platoon (ActivePlatoon*)** = el "squad" del char | ❓ -1 / "GOG 0x658" | ✅ **CONFIRMADO 0x658** | 52 |
| `+0x448` | animation (AnimationClass*) | — | ✅ nuevo | — |

> ⚠️ **Corrección:** `stats` en `+0x450` es un **puntero `CharStats*`** (KenshiLib `Character.h:738`),
> no un bloque `Stats` inline. El offsets.json del mod dice "Stats (inline)". Las skills (melee, etc.)
> viven dentro de `CharStats` (que empieza en `+0x8 medical, +0x10 me`), NO directamente en char+0x450.
> El `StatsOffsets` 0x00..0x60 del mod (meleeAttack@0x00…) **no casa con CharStats de KenshiLib**
> (que tiene multiplicadores, no las skills 0-100). 🟡 **Las skills 0-100 requieren RE adicional**
> (probablemente en otra sub-estructura). El bloque `stats_offsets` de offsets.json queda ❓.

### Squad / Platoon — corrección del `+0x658/+0x1D8`
| Símbolo | Valor real | Nota |
|---|---|---|
| **Character → su squad** | **`Character+0x658` = `ActivePlatoon*`** | Este es el "squad" del personaje (KenshiLib `Character.h:749`). El doc lo llamaba "GOG 0x658" — ✅ correcto y portable. |
| **Platoon.activePlatoon** | `Platoon+0x1D8` = `ActivePlatoon*` | KenshiLib `Platoon.h:201`. El `+0x1D8` del doc es offset DENTRO de `Platoon`, no de Character. |
| **ActivePlatoon.isPlayer** | `ActivePlatoon+0xE8` = `PlayerInterface*` | KenshiLib `Platoon.h:295`. (NO +0x1D8.) |
| ActivePlatoon.me (Platoon*) | `+0x78` | |
| ActivePlatoon.squadleader (Character*) | `+0xA0` | |
| Platoon.ownerships | `+0x148` (money en Ownerships+0x88) | KenshiLib |
| SquadAddMember | **0x928423** mid-function (no alineado) | Sin re-verificar (RTTI vtable slot 2). Mantener resolución por vtable. |

> **Conclusión `+0x658/+0x1D8`:** ambos existen pero son cosas distintas. El squad de un
> personaje se lee en **`char+0x658`** (ActivePlatoon*). NO usar +0x1D8 sobre Character.

### GameWorld (✅ KenshiLib `GameWorld.h` — todos confirmados)
| Offset | Campo | Estado |
|---|---|---|
| `+0x4A0` | theFactory (RootObjectFactory*) | ✅ |
| `+0x4A8` | factionMgr (FactionManager*) | ✅ |
| `+0x4B0` | navmesh (NavMesh*) | ✅ |
| `+0x4B8` | nodeList (NodeList*) | ✅ |
| `+0x580` | player (PlayerInterface*) | ✅ |
| `+0x700` | frameSpeedMult (float) = gameSpeed | ✅ |
| `+0x888` | **mainUpdateListRemovalQueue** (lektor<Character*>) — **NO es characterList** | ✅ confirma DEPRECADO. ⚠️ `offsets.json` aún lo marca `characterList @0x888 verified:true` → **CORREGIR** |
| `+0x8B0` | zoneMgr (ZoneManager*) | ✅ |
| `+0x8B9` | paused (bool) | ✅ |

> ⚠️ **Bug en offsets.json:** `world_offsets.characterList = {offset:0x888, verified:true}` es
> **INCORRECTO** — `+0x888` es `mainUpdateListRemovalQueue` (cola de borrado). La lista real de
> personajes del jugador es `GameWorld+0x580 → PlayerInterface+0x2B0 (playerCharacters)`.

### PlayerInterface (✅ KenshiLib `PlayerInterface.h`)
| Offset | Campo | Estado |
|---|---|---|
| `+0x2A0` | participant (Faction*) = facción del jugador | ✅ |
| `+0x2B0` | playerCharacters (lektor<Character*>) | ✅ |

### Faction (✅ KenshiLib `Faction.h`) — **corrección importante del `id`**
| Offset | Campo real (KenshiLib) | Doc/offsets.json | Estado |
|---|---|---|---|
| `+0x08` | **`_antiSlavery` (bool)** | "factionId uint32" | ❌ **El mod lee `id` en +0x08 pero ahí está `_antiSlavery`** |
| `+0x1A8` | name (std::string) | ✅ 0x1A8 | ✅ |
| `+0x208` | activePlatoons (lektor<Platoon*>) | — | ✅ nuevo |
| `+0x240` | data (GameData*) | — | ✅ |
| `+0x248` | isAI (AIPlayer*) | — | ✅ |
| `+0x250` | isPlayer (PlayerInterface*) | ✅ 0x250 | ✅ (campo del crash UAF) |

> ⚠️ **Faction NO tiene vtable** (Ogre allocator en +0x8). **NO hay un campo `id` uint32 simple.**
> `faction_hooks.cpp` y `offsets.json` (`faction_offsets.factionId=0x08`, `members=0x30`,
> `isPlayerFaction=0x90`, `money=0xA0`) usan offsets **no confirmados / erróneos**:
> - `+0x08` = `_antiSlavery` (NO id) → el `factionId` que envía `FactionRelation` puede ser basura.
> - `+0x30` = `allowSlavesWeapons` (NO members array).
> - `+0x90` = dentro de `tradeCulture` (NO isPlayerFaction).
>
> 🔴 **Liga directa con el bug conocido de facciones (enemigos huyen).** El `factionId` enviado en
> `C2S_FactionRelation` no es un id real. **Recomendado:** identificar la facción por su `name`
> (`+0x1A8`, confirmado) o por puntero, no por un "id" en +0x08. Requiere RE adicional para hallar
> un identificador estable (no parece existir un `uint32 id` plano en Faction).

### Inventory (✅ KenshiLib `Inventory.h` — **corrige offsets.json**)
| Offset real | Campo | offsets.json (mod) | Estado |
|---|---|---|---|
| `+0x10` | `_allItems` (lektor<Item*>) | items@0x10 | 🟡 es lektor (size@+0x18, data@+0x20 dentro del lektor), no array plano |
| `+0x80` | callbackObject (RootObject*) | — | ✅ |
| **`+0x88`** | **owner (RootObject*)** | "owner @0x28" ❌ | ✅ **owner es +0x88, NO +0x28** |
| `+0x90` | totalWeight (float) | — | ✅ |

> 🔴 **Bug:** `inventory_offsets.owner` no está en offsets.json (game-offsets doc dice +0x28).
> El **owner real es `Inventory+0x88`**. El bug histórico de pickup/drop (pasar inventory* en vez
> de owner char*) se arregla leyendo **`inv+0x88`**, no `+0x28`. ⚠️ Verificar qué offset usa
> `GetOffsets().inventory.owner` en el código.

### Item (InventoryItemBase, ✅ KenshiLib `Item.h` — **offsets.json totalmente erróneo**)
| Offset real | Campo | offsets.json (mod) | Estado |
|---|---|---|---|
| `+0xC0` | manufacturerData (GameData*) | — | ✅ |
| `+0xC8` | materialData | — | ✅ |
| `+0x11C` | quality (float) | "quality @0x38" ❌ | ✅ +0x11C |
| `+0x120` | weight (float) | "weight @0x48" ❌ | ✅ +0x120 |
| `+0x129` | isEquipped (bool) | — | ✅ |
| `+0x12C` | quantity (int) | "stackCount @0x30" ❌ | ✅ +0x12C |
| `+0x130` | itemWidth (int) | — | ✅ |
| `+0x134` | itemHeight (int) | — | ✅ |

> 🔴 Los `ItemOffsets` del mod (name@0x10, templateId@0x20, stackCount@0x30, value@0x40…) son
> **inventados y no casan** con InventoryItemBase de KenshiLib. Reemplazar por los de arriba.
> (Item hereda de RootObject, base 0xC0; el nombre/template viven en la jerarquía RootObject/GameData.)

### Building (✅ KenshiLib `Building.h` — **corrige offsets.json**)
| Offset real | Campo | offsets.json (mod) | Estado |
|---|---|---|---|
| Building hereda RootObject (base 0xC0) | — | — | base distinta |
| `+0xC4` | designation (BuildingDesignation) | — | ✅ |
| `+0xD0` | residentSquad (hand) | — | ✅ |
| `+0xF0` | residentSquadTemplate (GameData*) | — | ✅ |
| ConstructionState.constructionProgress | dentro de getBuildState() +0x4 | "buildProgress @0x110" ❌ | 🟡 el progreso vive en un sub-objeto ConstructionState, no en Building+0x110 |

> 🔴 Los `BuildingOffsets` del mod (name@0x10, pos@0x48, ownerFaction@0x80, health@0xA0,
> isDestroyed@0xA8, buildProgress@0x110, isConstructed@0x114) **no están confirmados** y varios
> chocan con el layout KenshiLib (Building base RootObject=0xC0, sus campos propios empiezan en
> 0xC0). La construcción/progreso vive en `ConstructionState` (sub-objeto vía `getBuildState()`),
> no en offsets planos. Todos ❓ — requieren RE específico si se quiere sync de edificios fiable.

### Stats / CharStats — ❓ sin resolver
`Character+0x450` = `CharStats*` (puntero). `CharStats` (KenshiLib) contiene **multiplicadores**
(`athleticsMultiplier`, `skillMult*`), NO las skills 0-100 (meleeAttack, strength…). El bloque
`stats_offsets` de offsets.json (0x00=meleeAttack … 0x60=labouring) **no corresponde** a CharStats.
Las skills 0-100 viven en otra estructura (probablemente la lista de skills del MedicalSystem o
GameData del personaje). 🟡 **Requiere RE adicional.** No confiar en los offsets de skills actuales.

---

## Resumen de ACCIONES recomendadas (no aplicadas — auditoría read-only)

1. ✅ **StartAttack:** corregir comentario `combat_hooks.cpp:405` (0x7A1650 → 0x7B2A20). Orch ya OK.
2. ✅ **Anchor GameWorld/TimeUpdate:** cambiar `dayTime` → `timeScale` (RVA 0x16A6F48) en orchestrator.
3. 🔴 **Faction `id`:** `+0x08` es `_antiSlavery`, NO un id. Identificar facción por `name` (+0x1A8)
   o puntero. Ligado al bug de facciones (enemigos huyen).
4. 🔴 **Inventory owner:** es `+0x88` (no +0x28). Verificar/corregir `GetOffsets().inventory.owner`.
5. 🔴 **offsets.json:** corregir `world_offsets.characterList` (0x888 = removalQueue, no chars),
   `item_offsets` (todos), `faction_offsets` (id/members/isPlayerFaction/money), `building_offsets`.
6. 🟡 **Stats/skills 0-100:** `char+0x450` es `CharStats*` puntero (multiplicadores). Las skills
   reales necesitan RE adicional; el bloque `stats_offsets` actual no es fiable.
7. ✅ **Character.platoon (squad) = `char+0x658`** (ActivePlatoon*). Portable, confirmado. NO usar +0x1D8.
8. ✅ **CharacterMoveTo real = 0x2EF4A0** (el 0x2EF4E3 es +0x43 mid-function). Mantener deshabilitado.

*Verificado por game-reverse-engineer contra kenshi_x64.exe 1.0.68 Steam. Análisis estático
read-only con pefile + iced-x86 + KenshiLib. Ninguna RVA aceptada a ciegas: todas por
string-xref→.pdata o patrón único. Offsets de campo cruzados con KenshiLib y frecuencia de acceso.*
