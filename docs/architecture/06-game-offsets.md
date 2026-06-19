# 06 — Mapa de Estructuras del Juego y Offsets (kenshi_x64.exe v1.0.68)

> **Ámbito:** Mapeo a fondo de `KenshiMP.Core/game/` y de TODOS los offsets de
> estructura del binario Steam 1.0.68. Cruzado con `docs/reverse-engineering/`,
> `docs/offsets.json` y `docs/patterns.json`.
>
> **Regla de oro RE (de las notas):** los offsets de CAMPO de struct son iguales
> entre Steam/GOG; las RVA de FUNCIÓN y de SINGLETON cambian por versión/plataforma.
> Por eso los offsets de abajo se consideran portables, pero las RVA NO.
>
> **Leyenda de fiabilidad:**
> - ✅ **VERIFICADO** — confirmado por CE, KServerMod, KenshiLib o disasm/RTTI.
> - 🟡 **PROBE/RUNTIME** — descubierto en ejecución (puede fallar), o derivado.
> - ❓ **DUDOSO/UNVERIFIED** — heurística o copiado sin verificar. NO confiar.
> - ⛔ **DEPRECADO/INEXISTENTE** — confirmado erróneo o el campo no existe.

---

## 0. Layout del PE y secciones (v1.0.68 Steam)

| Sección | VA (RVA) | Tamaño | Nota |
|---|---|---|---|
| `.text`  | `0x00001000` | `0x01671412` (~22.4 MB) | código; rango válido de vtables-as-code |
| `.rdata` | `0x01673000` | `0x0054A4CB` (~5.3 MB)  | strings debug `[Clase::metodo]`, vtables, RTTI |
| `.data`  | `0x01BBE000` | `0x0058E000` | **instancia embebida de GameWorld vive aquí** |
| ImageBase | `0x140000000` | — | binario MSVC x64 |

`__security_cookie` = RVA `0x21173C8` (patrón `48 8B 05 dd dd dd dd 48 33 C4`). No confundir con singletons.

---

## 1. Singletons y cómo el mod los resuelve

### 1.1 GameWorld — INSTANCIA estática embebida (no es puntero)

El hallazgo más importante de la sesión 2026-06-17:

- **GameWorld NO es `GameWorld* ou`. Es una instancia estática embebida en `.data`.**
- **RVA de la instancia = `0x2134110`** → el objeto GameWorld real es `modBase + 0x2134110`.
- Su **primer qword ES la vtable en `.text`** (`0x1722608`), NO un heap-ptr.
- Histórico: `0x2131020` (1.0.65) y `0x02133040` (GOG) dan NULL/basura en Steam 1.0.68.
- Evidencia: RTTI `.?AVGameWorld@@` (TD `0x2007FD0`, COL `0x185A0F0`), ctor `0x874E70`,
  dtor `0x86D650`, game loop en `0x82B8C8` hace `lea rcx,[0x2134110]` (LEA, no MOV → objeto
  embebido), 513 sitios usan `lea reg,[0x2134110]` para pasar `this`.

**Implicación crítica para el mod:** NO se puede dereferenciar el singleton a ciegas.
`*(base+0x2134110)` leería la vtable y todos los offsets darían basura. Por eso existe
`ResolveWorldObject()` (en `game_world.cpp`) y `ValidateGameWorldGlobal()` (en `core.cpp`),
que aceptan **ambos layouts**:

```
(a) puntero clásico  : *addr es heap-ptr fuera del módulo → el objeto es *addr
(b) instancia directa: *addr ES la vtable en .text        → el objeto es addr mismo
```

Campos de GameWorld verificados por conteo de xref absoluto (`lea reg,[instancia+off]`):

| Campo | RVA absoluta | Offset desde GameWorld | xrefs | Fiabilidad |
|---|---|---|---|---|
| player (PlayerInterface*) | `0x2134690` | `+0x580` | 609 | ✅ |
| zoneManager | `0x21349C0` | `+0x8B0` | 284 | ✅ |
| theFactory | `0x21345B0` | `+0x4A0` | 98 | ✅ |
| factionMgr | `0x21345B8` | `+0x4A8` | 67 | ✅ |
| navmesh | `0x21345C0` | `+0x4B0` | 130 | ✅ |
| nodeList | `0x21345C8` | `+0x4B8` | 62 | ✅ |

### 1.2 PlayerBase — puntero global clásico

- RVA `0x01AC8A90` (sección `.rdata`). El QWORD aquí lo rellena el juego en runtime con
  un heap-ptr. En reposo contiene `0x0002EF3700000000` (no es puntero válido).
- Se resuelve como `modBase + 0x01AC8A90` y se dereferencia una vez.
- ⚠️ Estos RVA son **GOG/Steam-específicos**. En Steam, si dan basura, `core.cpp` los limpia
  a 0 y cae al fallback de `entity_hooks` + cache de loading.

### 1.3 Flujo de resolución en el mod (`core.cpp`)

1. `ResolveGameFunctions()` rellena `m_gameFuncs.PlayerBase` y `m_gameFuncs.GameWorldSingleton`.
2. `validateSingleton()` valida PlayerBase (espera heap-ptr fuera del módulo, doble deref).
3. `ValidateGameWorldGlobal()` valida GameWorld (acepta instancia-directa O puntero).
4. Si Steam y ambos quedan en 0 → modo fallback por hooks de creación de personajes.
5. Bridges: `SetResolvedPlayerBase()` / `SetResolvedGameWorld()` exponen las direcciones a
   `game_character.cpp` (que las lee vía `GetResolvedPlayerBase/GameWorld`).

| Singleton | RVA usada | Tipo | Fiabilidad |
|---|---|---|---|
| GameWorldSingleton | `0x2134110` | instancia embebida (`.data`) | ✅ (Steam 1.0.68) |
| PlayerBase | `0x01AC8A90` | puntero global (`.rdata`) | 🟡 (validado en runtime) |

---

## 2. Character (KCharacter) — layout

Fuente combinada: KServerMod `structs.h` + cadenas CE + RE 1.0.68.
Definido en `CharacterOffsets` (`game_types.h:34`).

| Offset | Campo | Tipo | Fiabilidad | Fuente |
|---|---|---|---|---|
| `+0x00` | vtable | `void*` | ✅ | apunta a `.rdata`/módulo (test de validez del iterator) |
| `+0x08` | unknown / squad candidate | `void*` | ❓ | probe heurístico (0x08/0x20/0x28/0x30/0x38) |
| `+0x10` | **faction** | `Faction*` | ✅ | KServerMod — **clave para combate** |
| `+0x18` | name | `std::string` (MSVC SSO) | ✅ | KServerMod |
| `+0x40` | gameDataPtr | `GameData*` (backpointer template) | ✅ | KServerMod |
| `+0x48` | position (cacheada, read-only) | `Vec3` | ✅ | KServerMod |
| `+0x58` | rotation | `Quat` (w,x,y,z) | ✅ | KServerMod |
| `+0x298` | moneyChainPtr1 | `void*` | ✅ | CE (ver cadena dinero) |
| `+0x2B8` | healthChainPtr1 | `void*` | ✅ | CE (ver cadena salud) |
| `+0x2E8` | inventory | `Inventory*` | ✅ | KServerMod |
| `+0x2F0..0x440` | equipment[14] | `Item*[]` | 🟡 | probe runtime (`ProbeEquipmentOffset`) |
| `+0x450` | stats (INLINE, no puntero) | `Stats` (≥0x64 bytes) | ✅ | KServerMod |

### Campos NO resueltos (offset = -1)

| Campo | Estado | Nota |
|---|---|---|
| sceneNode | ❓ -1 | probe `game_offset_prober.cpp:ProbeSceneNode` |
| aiPackage | ❓ -1 | probe `ProbeAIPackage` |
| currentTask | ❓ -1 | sin verificar |
| isAlive | ❓ -1 | fallback: salud chest/head > -100 |
| moveSpeed / animState | ❓ -1 | derivados de física / sin verificar |
| animClassOffset | 🟡 -1 | probe 0x60-0x200, valida cadena de posición escribible |
| squad | ❓ -1 | heurística frágil (nombre std::string en squad+0x10) |
| **isPlayerControlled** | ⛔ **-1 (INEXISTENTE)** | ver nota abajo |

### isPlayerControlled — campo INEXISTENTE (no buscarlo más)

RE 2026-06-17 (KenshiLib `Character.h` + binario): **Character NO tiene este campo.**
La distinción player/NPC se deriva **por facción**:

```
Character::isPlayerCharacter()  ⟺  char.faction(+0x10) == gameWorld.player(+0x580).faction
```

El probe diferencial (`ProbePlayerControlledOffset`) quedó neutralizado. Esto es la raíz
del **fix de combate**: si `char+0x10` es NULL/incorrecta, el motor no te reconoce como
jugador y rechaza tus órdenes de ataque.

### Cadenas de punteros (CE-verificadas) ✅

**Salud** — `char+0x2B8 → +0x5F8 → +0x40 + bodyPart*8`
- Body parts: Head(0), Chest(1), Stomach(2), LArm(3), RArm(4), LLeg(5), RLeg(6).
- Stride 8: por parte hay `float health` + `float stun`.

**Dinero** — `char+0x298 → +0x78 → +0x88` = `int money`.

**Posición escribible (física)** — `char+animClassOffset → +0xC0 → +0x320 → +0x20`
- `AnimClass +0xC0` → CharMovement* (✅ KServerMod).
- `CharMovement +0x320 +0x20` → Vec3 escribible (escribir aquí mueve de verdad en Havok).
- `animClassOffset` se descubre en runtime; si no, `WritePosition` cae a métodos peores.

---

## 3. GameWorld / mundo — `WorldOffsets` (`game_types.h:100`)

| Offset | Campo | Tipo | Fiabilidad | Nota |
|---|---|---|---|---|
| `+0x580` | player (PlayerInterface*) | ptr | ✅ | KenshiLib GameWorld.h:137 — fuente de facción del jugador |
| `+0x700` | gameSpeed / frameSpeedMult | float | ✅ | KenshiLib |
| `+0x8B0` | zoneManager | `ZoneManager*` | ✅ | KenshiLib |
| `+0x8B9` | **paused** (bool 1 byte) | bool | ✅ | RE: `isPaused`@RVA 0xDEE00 `movzx eax,[rcx+8B9h]`; loop@0x788A00 `cmp byte[rsi+8B9h],0` |
| `+0x888` | ~~characterList~~ | — | ⛔ **DEPRECADO** | es `mainUpdateListRemovalQueue` (cola de borrado), NO la lista de personajes |
| timeOfDay | — | — | ⛔ N/A en GameWorld | vive en TimeManager+0x08, usar time_hooks |
| weatherState / buildingList / characterCount | ❓ -1 | — | sin verificar |

**La lista real de personajes del jugador NO está en `+0x888`.** Se obtiene vía:
`GameWorld → +0x580 (player) → PlayerInterface +0x2B0 (playerCharacters, lektor<Character*>)`.

### PlayerInterface — `PlayerInterfaceOffsets` (`game_types.h:119`)

Apuntado por `GameWorld+0x580`. Verificado en KenshiLib `PlayerInterface.h`.

| Offset | Campo | Tipo | Fiabilidad |
|---|---|---|---|
| `+0x2A0` | participant (Faction*) | `Faction*` | ✅ PlayerInterface.h:248 — **facción del jugador** |
| `+0x2B0` | playerCharacters | `lektor<Character*>` | ✅ PlayerInterface.h:250 |

### lektor (contenedor dinámico de Kenshi) — layout de 24 bytes ✅

Verificado disasm `push_back` en RVA `0x787512` y `0x799BFF`:

| Offset | Campo | Tipo |
|---|---|---|
| `+0x00` | vtable/header (polimórfico — NO leer como count/ptr) | qword |
| `+0x08` | size | uint32 |
| `+0x0C` | capacity | uint32 |
| `+0x10` | data (T** backing array) | ptr |

> Bug histórico: leer `+0x888` como lektor o asumir layout `{count,ptr}`/`{ptr,count}` de
> 16 bytes daba `faction=0xFA3000007FF71F5A`, `name="race"`. El layout correcto es el de arriba.

---

## 4. Sistema de FACCIÓN del jugador (núcleo del fix de combate)

### 4.1 Fuente PRIMARIA — `GetPlayerFactionDirect()` (`game_character.cpp:838`)

Resuelve la facción SIN iterar personajes, validando cada salto como heap-ptr:

```
GameWorld → +0x580 (player/PlayerInterface*) → +0x2A0 (participant) = Faction*
```

Es la fuente de verdad. No depende de que la lista de personajes esté poblada. La player
faction vive toda la partida (no se descarga con zonas) → **seguro escribirla en el host**.

### 4.2 El fix — `FixCharacterFactionTo()` (`game_character.cpp:1254`)

Escribe la player faction en `char+0x10` si no coincide ya. Todo bajo SEH (`SEH_RawFixFaction`,
sin objetos C++ en el `__try` por restricción MSVC C2712). Valida vtable dentro del módulo
antes de tocar nada. Devuelve `FixFactionResult` (InvalidChar/NoPlayerFaction/AlreadyCorrect/
Fixed/WriteFailed/Exception) para que el orquestador decida cuándo dejar de reintentar.

### 4.3 Caso REMOTO — escritura de facción DESHABILITADA ⛔

`player_controller.cpp` (`SetLocalFactionPtr`, `OnRemoteCharacterSpawned`): escribir el
puntero de facción de un NPC capturado en el personaje remoto causa **use-after-free**:
cuando la zona del NPC origen se descarga, su objeto Faction se libera y el motor crashea
accediendo a `faction+0x250` (game+0x927E94, puntero de 32 bits sign-extended → prefijo
`0xFFFFFFFF`). Por eso el remoto conserva su facción de fábrica. El offset `+0x250`
(Faction → isPlayer/PlayerInterface*) está confirmado en KenshiLib `Faction.h:158`.

### 4.4 Elección por votación — `OnGameWorldLoaded()` (`player_controller.cpp:232`)

Fallback que escanea hasta 12 personajes y vota la facción más frecuente, con bonus por
match de nombre (+10) y flag `isPlayerFaction` (+3). Maneja el caso de que el primer
personaje sea un NPC contratado.

### 4.5 Parche del faction string de lobby — `lobby_manager.cpp`

Sobrescribe en `.rdata` el string `"204-gamedata.base"` por el del mod (ej. `"10-kenshi-online.mod"`):
- RVA candidatas: `modBase+0x16C4258` (Steam 1.0.68), `modBase+0x16C2F68` (GOG 1.0.68).
- Fallback: búsqueda lineal en `.rdata` del literal. `VirtualProtect` + escritura + restaurar.

---

## 5. Faction — `FactionOffsets` (`game_types.h:166`)

| Offset | Campo | Tipo | Fiabilidad |
|---|---|---|---|
| `+0x08` | id | uint32 | 🟡 hardcoded en 6+ hooks, ahora sí en la struct |
| `+0x1A8` | name | `std::string` | ✅ KenshiLib Faction.h:147 (antes `0x10`, **incorrecto**) |
| `+0x250` | isPlayer (PlayerInterface*) | ptr | ✅ Faction.h:158 (campo del crash UAF) |
| `+0x30` | members | ptr array | ❓ |
| `+0x38` | memberCount | int | ❓ |
| `+0x50` | relations | map ptr | ❓ nunca dereferenciado |
| `+0x80/0x84` | color1/color2 | uint32 ARGB | ❓ |
| `+0x90` | isPlayerFaction | bool | ❓ (usado en votación, sin verificar) |
| `+0xA0` | money | int | ❓ |

> `FactionExtraOffsets` (`game_types.h:126`) duplica `nameStr=0x1A8` e `isPlayer=0x250`
> para las sondas de diagnóstico. La discrepancia de rango de relación (juego 0-100 vs
> protocolo -100..+100) sigue abierta.

---

## 6. Otras estructuras (TODOS los offsets son ❓ UNVERIFIED salvo nota)

### Squad — `SquadOffsets` (`game_types.h:92`)
`+0x10` name, `+0x28` memberList, `+0x30` memberCount, `+0x38` factionId, `+0x40` isPlayerSquad.
Todos ❓. `SquadAddMember`@`0x928423` es mid-function (no alineado), hook frágil.

### Building — `BuildingOffsets` (`game_types.h:131`)
`+0x10` name, `+0x48` pos, `+0x58` rot, `+0x80` ownerFaction, `+0xA0` health, `+0xA4` maxHealth,
`+0xA8` isDestroyed, `+0xC0` functionality, `+0xE0` inventory, `+0x100` townId, `+0x110`
buildProgress, `+0x114` isConstructed. Todos ❓ (mismo patrón que Character para name/pos/rot).
`buildingList` en WorldOffsets = -1 → no se pueden iterar edificios.

### Inventory — `InventoryOffsets` (`game_types.h:146`)
`+0x10` items, `+0x18` itemCount, `+0x20` width, `+0x24` height, `+0x28` owner, `+0x30` maxStackMult. ❓
**BUG conocido:** hooks de pickup/drop pasan `inventory*` a `GetNetId()` que espera `character*`;
hay que leer owner en `inv+0x28` primero.

### Item — `ItemOffsets` (`game_types.h:155`)
`+0x10` name, `+0x20` templateId (¿GameData* de 8 bytes, no uint32?), `+0x30` stackCount,
`+0x38` quality, `+0x40` value, `+0x48` weight, `+0x50` equipSlot, `+0x58` condition. ❓

### Stats — `StatsOffsets` (`game_types.h:189`) — INLINE en char+0x450
Bloque de floats 0-100 a offsets 0x00..0x60 (meleeAttack 0x00 … labouring 0x60). ❓ orden sin verificar.

### GameData / TimeManager
- GameData: `+0x08` id, `+0x10` managerPtr, `+0x28` name. ❓
- TimeManager: `+0x08` timeOfDay (float 0-1), `+0x10` gameSpeed. ✅ captura por hook runtime.

### std::string MSVC x64 (layout usado en todos los accesores) ✅
`+0x00` buf[16] (SSO) · `+0x10` size (u64) · `+0x18` capacity (u64). Si `capacity>15`, los
primeros 8 bytes son heap-ptr a los datos. Umbral SSO = 15 chars.

---

## 7. Funciones del juego (RVAs — ⚠️ NO portables entre versiones)

| Función | RVA | Firma (`__fastcall`) | Fiabilidad |
|---|---|---|---|
| CharacterSpawn (RootObjectFactory::process) | `0x581770` | `void*(factory, requestStruct)` | ✅ patrón+.pdata+xref, 6410 bytes |
| CharacterSerialise | `0x6280A0` | `void(char, stream)` | 🟡 |
| HavokCharacter::setPosition | `0x145E50` | `void(havokChar, Vec3*)` | 🟡 |
| CharacterMoveTo | `0x2EF4E3` | `void(char,x,y,z,moveType)` | 🟡 mid-func, NO hookear |
| ApplyDamage | `0x7A33A0` | `void(target,attacker,bodyPart,cut,blunt,pierce)` | 🟡 |
| CharacterDeath | `0x7A6200` | `void(char, killer)` | 🟡 |
| CharacterKO | `0x345C10` | `void(char, attacker, reason)` | 🟡 |
| AI::create | `0x622110` | `void*(char, faction)` | ✅ patrón corregido (41 bytes) |
| SquadAddMember | `0x928423` | `void(squad, char)` | 🟡 mid-func |
| CharacterDestroy (=NodeList::destroyNodesByBuilding) | `0x38A720` | `void(nodeList, building)` | ✅ patrón único — **OJO: no es el dtor real de Character** |
| GameFrameUpdate | `0x123A10` | `void(rcx, rdx)` | ✅ patrón extendido (45 bytes) |
| TimeUpdate | `0x214B50` | `void(timeMgr, dt)` | ✅ patrón único |

### Patrones AOB que necesitaron fix de unicidad (de `pattern-verification.md`)
- **AICreate**: el de 33 tokens tenía falso positivo en `0xAF870`. Extender a 41 bytes
  añadiendo `48 33 C4 48 89 84 24 80` tras los wildcards.
- **GameFrameUpdate**: falso positivo en `0x788100`. Extender a 45 bytes añadiendo
  `48 89 58 10 48 89 70 18 48 89 78 20 48`.
- CharacterDestroy y TimeUpdate ya son únicos.

### Fix MovRaxRsp (prólogo `48 8B C4` = `mov rax,rsp`)
22 de 41 funciones empiezan así y crashean con el trampoline de MinHook (RAX captura el
RSP del hook, no el del juego → `lea rbp,[rax-0xNN]` corrompe memoria). Solución: stub ASM
de dos partes (naked detour que guarda RSP + trampoline wrapper que hace swap de pila).
`HookManager::InstallRaw` lo auto-aplica. Detalle en `mov-rax-rsp-fix.md`.

---

## 8. Verificación de offsets en runtime — `game_offset_prober.cpp`

Descubre offsets desconocidos sondeando objetos vivos; cachea a `offset_cache.json`.
Probes: `ProbeSceneNode`, `ProbeIsPlayerControlled` (neutralizado — campo inexistente),
`ProbeAIPackage`, `ProbeMoveSpeed`, `ProbeAnimState`, `ProbeEquipmentOffset` (en
game_character), `ProbeAnimClassOffset` (valida cadena de posición). Todo corre bajo
`RunProbesSEH`. API: `RunOffsetProber`, `LoadOffsetCache`, `SaveOffsetCache`,
`ResetOffsetProber`, `IsProberComplete`.

Validaciones de puntero usadas en todo el código (criterio "heap-ptr válido del juego"):
`>= 0x10000`, `< 0x00007FFFFFFFFFFF`, 8-byte alineado, y FUERA de la imagen del módulo
(`< modBase || >= modBase+SizeOfImage`). Los objetos de clase del juego además tienen
vtable dentro del módulo (en `.rdata`/`.text`).

### Sonda de diagnóstico [DIAG] (`DiagDumpPlayerFaction` / `DiagTickPump`)
Escanea en rango buscando el offset real de la facción del jugador, filtra candidatos por
test de string legible (≥3 chars, ≥80% ASCII imprimible), y cruza con la facción de certeza
(`char[0].faction` en `+0x10`). Throttle de 2s reales, máx 6 volcados. Solo loguea, no
cambia comportamiento.

---

## 9. Mapa de archivos `KenshiMP.Core/game/`

| Archivo | Responsabilidad |
|---|---|
| `game_types.h` | TODAS las struct de offsets + accesores + typedefs de funciones. Pieza central. |
| `game_character.cpp` | CharacterAccessor, CharacterIterator, `GetPlayerFactionDirect`, `FixCharacterFactionTo`, cadenas salud/dinero/posición, probes diferidos, bridges (PlayerBase/GameWorld/loading), sonda [DIAG]. |
| `game_world.cpp` | GameWorldAccessor + `ResolveWorldObject` (maneja instancia embebida vs puntero), pausa, gameSpeed, timeOfDay. |
| `game_faction.cpp` | FactionAccessor (name/memberCount/isPlayerFaction/money). |
| `game_offset_prober.{h,cpp}` | Descubrimiento de offsets en runtime + cache a disco. |
| `spawn_manager.{h,cpp}` | Captura del RootObjectFactory, BD de templates, cola de spawn, templates de mod, `SpawnWithModTemplate`, `ReadKenshiString`. |
| `player_controller.{h,cpp}` | Facción local (votación), spawn de personajes remotos, write de facción remota DESHABILITADO (UAF). |
| `lobby_manager.{h,cpp}` | Parche del faction string en `.rdata` para asignación de facción de lobby. |
| `shared_save_sync.{h,cpp}` | Sync por tick: posición/gameSpeed entre host y remoto, descubrimiento de chars por nombre. |
| `game_squad.cpp` / `game_building.cpp` / `game_inventory.{cpp,h}` / `game_stats.cpp` | Accesores de squad/building/inventory/stats (offsets mayormente ❓). |
| `loading_orchestrator.{cpp,h}` | Orquestación de la fase de carga (skip de lecturas de memoria mientras carga). |
| `asset_facilitator.{cpp,h}` | Facilitador de assets. |

---

## 10. Cómo el mod accede a las estructuras (resumen de cadenas)

```
modBase + 0x2134110  ──►  GameWorld (instancia embebida, ResolveWorldObject)
   ├─ +0x580 ─► PlayerInterface
   │              ├─ +0x2A0 ─► Faction*  (facción del jugador) ◄── GetPlayerFactionDirect
   │              └─ +0x2B0 ─► lektor<Character*>  (lista real de personajes del jugador)
   │                              └─ data[i] ─► Character
   ├─ +0x700 ─► gameSpeed (float)
   ├─ +0x8B0 ─► ZoneManager*
   └─ +0x8B9 ─► paused (bool)

Character (char):
   +0x10 ─► Faction*   ◄── FixCharacterFactionTo escribe aquí la player faction (fix combate)
   +0x18 ─► name (std::string)
   +0x40 ─► GameData* (template)
   +0x48 ─► Vec3 posición cacheada
   +0x2B8 ─► +0x5F8 ─► +0x40+part*8 ─► salud (float)   [CE]
   +0x298 ─► +0x78  ─► +0x88        ─► dinero (int)     [CE]
   +0x2E8 ─► Inventory*
   +0x450 ─► Stats (inline)
```

---

*Generado por game-reverse-engineer. Cruzado con: `kenshi-re-memory.md`,
`character-class-analysis.md`, `class-analysis-building-inventory-faction-squad.md`,
`pattern-verification.md`, `mov-rax-rsp-fix.md`, `offsets.json`, `patterns.json` y el
código de `KenshiMP.Core/game/` + `core.cpp`.*
