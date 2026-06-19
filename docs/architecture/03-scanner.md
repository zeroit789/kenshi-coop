# 03 — KenshiMP.Scanner: Resolución de Direcciones en Runtime

> Mapa completo del subsistema que localiza funciones y globales de `kenshi_x64.exe`
> (Steam 1.0.68) en tiempo de ejecución. Generado por mapeo del código fuente
> (`KenshiMP.Scanner/`) cruzado con `docs/reverse-engineering/`.
> Sintaxis Intel. Offsets en hex. RVAs relativos al `ImageBase` (0x140000000) —
> en runtime se usan como `GetModuleHandle(NULL) + RVA`.

## 0. Resumen de arquitectura

El Scanner tiene **dos caminos de resolución coexistentes** que escriben en la misma
struct `GameFunctions`:

| Camino | Punto de entrada | Estrategia | Estado |
|--------|------------------|-----------|--------|
| **A. Orchestrator** (moderno) | `PatternOrchestrator::Run()` | Pipeline de 8 fases con descubrimiento (`.pdata` → strings → vtables → pattern → fallbacks → call graph → globals → emergencia) | El completo, basado en evidencia |
| **B. ResolveGameFunctions** (clásico) | `patterns.cpp::ResolveGameFunctions()` | Pattern scan directo + `RuntimeStringScanner` fallback + descubrimiento de globales por disasm | El que históricamente arranca el mod |

Ambos comparten: `Memory` (lecturas SEH-safe), parsing de patrones AOB, y validación
por `.pdata` (`RtlLookupFunctionEntry`). El orquestador es más robusto; `ResolveGameFunctions`
es autónomo y no depende de las fases. La clave de seguridad común: **una RVA hardcodeada
nunca se acepta a ciegas si existe patrón y el patrón falló** (= binario distinto → crash).

```
PatternScanner / ScannerEngine  (lee secciones PE, scan AOB SSE2)
        │
        ├── PDataEnumerator      (RUNTIME_FUNCTION → límites de función autoritativos)
        ├── StringAnalyzer       (strings .rdata + xrefs LEA RIP-rel → funciones)
        ├── VTableScanner        (RTTI COL → className → slots vtable)
        ├── CallGraph            (propagación de etiquetas; poco usado para resolver)
        │
        ├── PatternOrchestrator  (CAMINO A: orquesta todo lo anterior en 8 fases)
        └── ResolveGameFunctions (CAMINO B: standalone, usa RuntimeStringScanner)

HookManager + MovRaxRspFix       (instalan los detours sobre las direcciones resueltas)
```

Archivos:
- `src/scanner.cpp` / `scanner_engine.cpp` — motor de scan PE + AOB.
- `src/pdata_enumerator.cpp` — enumeración `.pdata`.
- `src/string_analyzer.cpp` — strings + xrefs.
- `src/vtable_scanner.cpp` — RTTI/vtables.
- `src/orchestrator.cpp` — CAMINO A.
- `src/patterns.cpp` + `include/kmp/patterns.h` — AOBs, anchors, struct, CAMINO B.
- `src/mov_rax_rsp_fix.cpp` — fix del prólogo `48 8B C4`.
- `src/hook_manager.cpp` — instalación de hooks + bypass.
- `src/function_analyzer.cpp` — análisis de prólogo (params/stack), informativo.
- `include/kmp/memory.h` — lecturas/escrituras SEH-safe + `FollowChain`.

---

## 1. Métodos de resolución (`ResolutionMethod`)

Definidos en `orchestrator.h`, nombrados por `ResolutionMethodName()`:

| Método | Confianza típica | Cómo funciona |
|--------|------------------|---------------|
| `PatternScan` | 1.00 | AOB SSE2 sobre `.text`. La fuente de verdad cuando el patrón es único. |
| `StringXref` | 0.7–0.9 | String en `.rdata` → LEA RIP-rel que lo referencia → función contenedora vía `.pdata`. |
| `VTableSlot` | 0.90 | RTTI: `className` → vtable → slot N. Usado para `SquadAddMember`. |
| `CallGraphTrace` | 0.60 | Función etiquetada por sus strings. Resolución débil. |
| `HardcodedOffset` | 0.5–0.95 | `base + RVA`. **Solo si NO hay patrón** (si hay patrón y falló = binario distinto). |
| `ComplexPattern` | variable | Ancla + componentes a offsets relativos + post-proceso (FollowCall/RIP/prologue). |
| `PDataLookup` / `Manual` / `None` | — | Auxiliares. |

**Filtro de alineación** (`ResolveEntry`): toda dirección de función resuelta por
PatternScan/StringXref/HardcodedOffset que **no esté alineada a 16 bytes** se **rechaza**
(probable hit a mitad de función / bloque SEH), salvo que `.pdata` la confirme como inicio
real (caso `SquadAddMember` @0x928423).

---

## 2. Pipeline del Orchestrator (CAMINO A) — 8 fases

`PatternOrchestrator::Run()` ejecuta en orden (`orchestrator.cpp`):

1. **RunPhase1_PData** — `PDataEnumerator::Enumerate()`. Lee el directorio de excepciones
   (`IMAGE_DIRECTORY_ENTRY_EXCEPTION`), construye índice ordenado de `RUNTIME_FUNCTION`
   (startVA/endVA). Es la fuente **autoritativa** de límites de función. Todo bajo SEH.
2. **RunPhase2_Strings** — `StringAnalyzer`: escanea ASCII (y wide si está activado) en
   `.rdata`, resuelve TODOS los LEA RIP-rel de `.text` (`48/4C 8D` mod=0 rm=5), mapea cada
   xref a su función contenedora vía `PDataEnumerator::FindContaining`, y etiqueta funciones
   por sus strings (`[Clase::metodo]` → `Clase::metodo`).
3. **RunPhase3_VTables** — `VTableScanner::ScanVTables()`: busca candidatos vtable en `.rdata`
   (slot[0] apunta a `.text` y slot[-1] = COL apunta a `.rdata`), lee RTTI COL →
   `TypeDescriptor` → desmangla `.?AVClassName@@`. Loguea diagnóstico de clases
   platoon/squad/active (para `SquadAddMember`).
4. **RunPhase4_PatternScan** — `BatchScan` SIMD de una pasada: indexa por primer byte fijo
   de cada patrón y barre `.text` una vez resolviendo todos los AOB simultáneamente.
5. **RunPhase5_StringFallback** — para los no resueltos: `TryStringXref` → `TryVTableSlot`
   → `TryHardcodedOffset` → `TryComplexPattern`, en ese orden.
6. **RunPhase6_CallGraph** — construye grafo (solo para resueltos por defecto), propaga
   etiquetas, intenta `TryCallGraphTrace` para lo que quede.
7. **RunPhase7_GlobalPointers** — (7a) descubre globales no resueltos con `stringAnchor`:
   busca la función que referencia el string y escanea su código por `MOV/LEA reg,[RIP+disp32]`
   que apunte a `.data` (vía `SEH_ScanCodeForGlobalPtr`). (7b) valida globales ya resueltos
   (doble-deref: valor heap fuera del módulo + vtable en módulo).
8. **RunPhase8_EmergencyCritical** — solo para entradas `critical` aún sin resolver:
   `TryDirectStringSearch` (búsqueda bruta del string en `.rdata` + scan LEA en `.text` +
   walk-back de prólogo) y `TryPrologueValidatedRVA` (acepta la RVA hardcodeada si los bytes
   ahí parecen un prólogo válido).

`RetryFailed()` / `RetryEntry()` reintentan todos los métodos hasta `maxRetries`.

### Walk-back de prólogo (`WalkBackToPrologue`)
Camina hacia atrás buscando padding `CC` (int3) → el siguiente byte no-CC es el inicio.
Valida contra prólogos MSVC conocidos: `48 8B C4` (mov rax,rsp), `48 89 5C` (mov [rsp+x],rbx),
`48 83 EC`/`48 81 EC` (sub rsp), `40 53..57` (push c/REX), `55`/`53`, `41 54..57` (push r8-r15).

---

## 3. CAMINO B — `ResolveGameFunctions` (standalone)

`patterns.cpp::ResolveGameFunctions(scanner, funcs)`:

1. **Pattern scan directo** (`tryPattern`): para cada función con AOB, `scanner.Find()`, luego
   **valida contra `.pdata`**: si el hit está a >0x10 del inicio real de función → es la
   función EQUIVOCADA → `target=nullptr` (no se hookea). Corrige offsets ≤0x10 (padding).
2. **RuntimeStringScanner fallback** (clase interna en `patterns.cpp`): para los que sigan
   null, busca string en `.rdata` → `FindStringXref` (LEA RIP-rel) → `FindFunctionStart`
   (primero `RtlLookupFunctionEntry`, fallback walk-back de 16 KB). Cruza con `.pdata`.
3. **Descubrimiento de globales por disasm de función** (PlayerBase / GameWorld):
   escanea el código de funciones ya resueltas por `MOV/LEA reg,[RIP+disp32]→.data`.

Diferencia clave con `RuntimeStringScanner` (camino B) vs `StringAnalyzer` (camino A): el
camino B usa walk-back agresivo (16 KB) que **falla en funciones grandes** como
`CharacterSpawn` (2996 bytes de xref a prólogo); el camino A resuelve eso vía
`.pdata::FindContaining`, que es exacto. Ver §8.

---

## 4. TABLA MAESTRA — Funciones que el mod resuelve

RVAs hardcodeadas = fallback **GOG/1.0.68**. Categoría/anchor/criticidad de
`RegisterBuiltinPatterns` (orchestrator.cpp) + `ResolveGameFunctions` (patterns.cpp).
`crit` = crítica para multiplayer (su hook NO se instala si no se resuelve).

### 4.1 Entity Lifecycle
| ID | RVA (GOG) | Método primario | String anchor | Patrón | crit |
|----|-----------|-----------------|---------------|--------|------|
| `CharacterSpawn` | 0x00581770 | StringXref(.pdata) | `[RootObjectFactory::process] Character` | sí (mov rax,rsp) | **sí** |
| `CharacterDestroy` | 0x0038A720 | Pattern | `NodeList::destroyNodesByBuilding` | sí (único, verificado) | **sí** |
| `CreateRandomSquad` | 0x00583A10 | Pattern/String | `[RootObjectFactory::createRandomSquad] Missing squad leader` | sí | no |
| `CharacterSerialise` | 0x006280A0 | Pattern/String | `[Character::serialise] Character '` | sí | no |
| `CharacterKO` | 0x00345C10 | Pattern | `knockout` (genérico) | sí (con wildcards) | no |
| `SquadSpawnBypass` | 0x004FF47C | Pattern | ` tried to spawn inside walls!` | inline | no |
| `CharAnimUpdate` | 0x0065F6C7 | Pattern (GOG) | — | inline (mov rcx,[rbx+320]) | no |

Notas struct: `FactoryCreate` (0x583400), `CreateRandomChar` (0x5836E0), `SquadSpawnCall`
(0x4FFA88) existen en `GameFunctions` pero **no se registran/resuelven** (sin patrón ni anchor).

### 4.2 Movement
| ID | RVA | Método | Anchor | Notas |
|----|-----|--------|--------|-------|
| `CharacterSetPosition` | 0x00145E50 | Pattern/String | `HavokCharacter::setPosition moved someone off the world` | OK |
| `CharacterMoveTo` | 0x002EF4E3 | — | `pathfind` (genérico) | **DESHABILITADO**: patrón=nullptr (IAT call no-wildcardable); anchor "pathfind" cae en función equivocada en Steam; hook deshabilitado (5 params + mov rax,rsp). |

### 4.3 Combat
| ID | RVA | Método | Anchor |
|----|-----|--------|--------|
| `ApplyDamage` | 0x007A33A0 | Pattern/String | `Attack damage effect` |
| `StartAttack` | 0x007B2A20 | Pattern/String | `Cutting damage` |
| `CharacterDeath` | 0x007A6200 | Pattern/String | `{1} has died from blood loss.` |
| `HealthUpdate` | 0x0086B2B0 | Pattern/String | `block chance` |
| `CutDamageMod` | 0x00889CD0 | Pattern | `cut damage mod` |
| `UnarmedDamage` | 0x000CE2D0 | Pattern | `unarmed damage bonus` |
| `MartialArtsCombat` | 0x00892120 | Pattern/String | `Martial Arts` |

### 4.4 World / Zones
| ID | RVA | Anchor | Notas |
|----|-----|--------|-------|
| `ZoneLoad` | 0x00377710 | `zone.%d.%d.zone` | wildcards en stack-size |
| `ZoneUnload` | 0x002EF1F0 | `destroyed navmesh` | |
| `BuildingPlace` | 0x0057CC70 | `[RootObjectFactory::createBuilding] Building` | |
| `BuildingDestroyed` | 0x00557280 | `Building::setDestroyed` | |
| `Navmesh` | 0x00881950 | (sin anchor) | solo patrón |
| `SpawnCheck` | 0x004FFAD0 | ` tried to spawn inside walls!` | |

### 4.5 Game Loop / Time (críticas)
| ID | RVA | Anchor | crit | Nota uniqueness |
|----|-----|--------|------|-----------------|
| `GameFrameUpdate` | 0x00123A10 | `Kenshi 1.0.` | **sí** | patrón extendido a 45 bytes (falso positivo en 0x00788100) |
| `TimeUpdate` | 0x00214B50 | `timeScale` | **sí** | patrón único, verificado |

### 4.6 Save / Load / UI
| ID | RVA | Anchor |
|----|-----|--------|
| `SaveGame` | 0x007EF040 | `quicksave` |
| `LoadGame` | 0x00373F00 | `[SaveManager::loadGame] No towns loaded.` |
| `ImportGame` | 0x00378A30 | `[SaveManager::importGame] No towns loaded.` |
| `CharacterStats` | 0x008BA700 | `CharacterStats_Attributes` |

### 4.7 Squad / Platoon
| ID | RVA | Método | Notas |
|----|-----|--------|-------|
| `SquadCreate` | 0x00480B50 | Pattern/String | `Reset squad positions` |
| `SquadAddMember` | 0x00928423 | **VTableSlot** | `vtableClass="ActivePlatoon\|Platoon\|Squad"`, slot 2 (+0x10). Patrón=nullptr y anchor `delayedSpawningChecks` ELIMINADOS (caen en función equivocada en Steam; 0x928423 es mid-function no-alineado). |

### 4.8 Inventory / Faction / AI / Turret / Building
| ID | RVA | Anchor | crit |
|----|-----|--------|------|
| `ItemPickup` | 0x0074C8B0 | `addItem` | no |
| `ItemDrop` | 0x00745DE0 | `removeItem` | no |
| `BuyItem` | 0x0074A630 | `buyItem` | no |
| `FactionRelation` | 0x00872E00 | `faction relation` | no |
| `AICreate` | 0x00622110 | `[AI::create] No faction for` | **sí** — patrón extendido a 41 bytes (falso positivo en 0x000AF870) |
| `AIPackages` | 0x00271620 | `AI packages` | no |
| `GunTurret` | 0x0043B690 | `gun turret` | no |
| `GunTurretFire` | 0x0043CDB0 | (sin anchor) | no |
| `BuildingDismantle` | 0x002A2860 | `dismantle` | no |
| `BuildingConstruct` | 0x005547F0 | `construction progress` | no |
| `BuildingRepair` | 0x00555650 | (sin anchor) | no |

### 4.9 Entradas registradas SIN patrón ni RVA (placeholders / aspiracionales)
`RegisterBuiltinPatterns` registra ~70 entradas más solo con `stringAnchor` y RVA=0
(research, crafting, dialogue, weather, terrain, animation, pathfinding, audio, modding,
UI/MyGUI, render/Ogre, physics/Havok, economy, stealth, medical, slavery, camp, limbs,
needs, worldstate). **No se resuelven a nada útil** hoy (anchors genéricos como `heal`,
`gate`, `price`, `camp`). Son ganchos para RE futuro, no funcionalidad activa.

---

## 5. GLOBALES — PlayerBase y GameWorld

### 5.1 PlayerBase
| Campo | Valor |
|-------|-------|
| RVA fallback (GOG) | **0x01AC8A90** |
| Sección | `.rdata` (no `.data`) |
| crit | **sí** |
| Anchors | `CharacterStats_Attributes`, `Reset squad positions`, `[Character::serialise] Character '`, `[RootObjectFactory::process] Character`, `quicksave` |

Resolución (orden, en `ResolveGameFunctions` + `RetryGlobalDiscovery`):
1. **Disasm de función** — escanea `CharacterSpawn/CharacterStats/SaveGame/LoadGame` por
   `MOV/LEA reg,[RIP→.data]`, nth=0..15.
2. **String-xref** — `FindGlobalNearString` con los anchors.
3. **Hardcoded** — `base+0x01AC8A90`.
4. **Pass 2 tentativo (Steam pre-load)** — acepta `.data` con valor null si la partida no
   ha cargado; se re-valida luego en `RetryGlobalDiscovery`.

Validación (`validatePlayerBase`): `*addr` debe ser puntero de heap (fuera del módulo) cuya
vtable apunte a `.text`. Es puntero-a-puntero clásico (`*PlayerBase → objeto`).

Cadenas CE conocidas (`patterns.h::KNOWN_CHAINS`, base 0x01AC8A90, **offsets estables entre
versiones**):
- Health: `+0x2B8 → +0x5F8 → +0x40` (float)
- StunDamage: `+0x2B8 → +0x5F8 → +0x44`
- Money: `+0x298 → +0x78 → +0x88` (int)
- CharList: `+0x0` (primer personaje, +8 por siguiente)

### 5.2 GameWorld — ⚠️ DISCREPANCIA CRÍTICA (instancia directa vs puntero)
| Campo | Valor |
|-------|-------|
| RVA hardcodeada actual (código) | **0x02134110** (1.0.68 Steam) |
| Historial | 0x02133040 (erróneo) → 0x02131020 (1.0.65, daba NULL) → **0x02134110** (1.0.68 OK) |
| crit | **sí** |
| Anchors | `dayTime`, `zone.%d.%d.zone`, `Kenshi 1.0.` |

**Hallazgo de RE (kenshi-re-memory.md, 2026-06-17):** en 1.0.68 GameWorld **NO es un puntero
global** (`GameWorld* ou`); `base+0x2134110` **ES la instancia estática embebida** en `.data`
(su primer qword es la vtable en `.text` 0x1722608). Confirmado por RTTI `.?AVGameWorld@@`,
ctor 0x874E70 / dtor 0x86D650, y 513 xrefs `lea reg,[0x2134110]` (lea, no mov → objeto
embebido). El código YA está adaptado: `resolveGwObject()` (en `patterns.cpp`) acepta AMBOS
layouts:
- (a) puntero clásico: `*candidateAddr` es heap-ptr → el objeto es `*candidateAddr`.
- (b) instancia directa: `*candidateAddr` ES la vtable (`.text`) → el objeto es `candidateAddr`.

⚠️ **Riesgo a vigilar:** el anchor `dayTime` y `[GameWorld::` **NO existen** en 1.0.68 (por eso
el StringXref de GameWorld falla y depende del disasm / hardcoded). Strings que SÍ existen:
`timeScale` (RVA 0x16A6F48), `zone.%d.%d.zone` (0x16C4CC0), `Kenshi 1.0.68` (0x1692288).
Conviene cambiar el anchor `dayTime` por `timeScale` para que el descubrimiento por string
funcione realmente en 1.0.68.

**Cadena de validación** (`validateGameWorld` / `validateGameWorldChain`, robusta a versión —
NO solo "objeto con vtable", que daba falsos positivos):
```
gwObj = resolveGwObject(candidato)        // instancia directa o *puntero
player  = *(gwObj  + 0x580)   // PlayerInterface*   (GameWorld.h:137)
faction = *(player + 0x2A0)   // Faction*           (PlayerInterface.h:248)
name    =   faction + 0x1A8   // std::string ASCII  (Faction.h:147) — refuerzo
```
Si la cadena no resuelve → no es el GameWorld → se descarta el candidato. `RetryGlobalDiscovery`
re-valida tras cargar partida y descarta el falso positivo que el Pass-2 tentativo pudiera fijar.

> Nota campos GameWorld verificados por xref absoluto (instancia+offset, en RE memory):
> player @+0x580, theFactory @+0x4A0, factionMgr @+0x4A8, navmesh @+0x4B0, nodeList @+0x4B8,
> zoneMgr @+0x8B0 (las RVAs absolutas 0x2134690 etc. = 0x2134110 + offset).

`GameWorldSingleton` está registrado en el orchestrator con anchor `dayTime` (mismo problema).

---

## 6. Patrones AOB (`patterns.h`)

- Notación IDA: bytes hex + `?`/`??` wildcard. Parsing en `ScannerEngine::Parse` /
  `PatternScanner::Parse` (token `?`/`??` → mask=false).
- Wildcards aplicados a bytes que cambian por versión: displacements LEA-rbp, tamaños de
  stack (`sub rsp, imm32`), targets de `call`, inmediatos.
- Scan SSE2 (`ScanRegionSSE2`): localiza el primer byte fijo con `_mm_cmpeq_epi8` +
  `_mm_movemask_epi8`, luego verifica el patrón completo. `BatchScan` indexa por primer byte
  fijo y barre `.text` una sola vez para todos los patrones.
- Por defecto se escanea **solo `.text`** (`GetScanRegions`).

**Patrones deshabilitados (=nullptr) a propósito:**
- `CHARACTER_MOVE_TO` — IAT indirect call `FF 15` no-wildcardable sin perder unicidad.
- `SQUAD_ADD_MEMBER` — caía mid-function en Steam; se resuelve por RTTI vtable.
- `INPUT_KEY_PRESSED` / `INPUT_MOUSE_MOVED` — input se intercepta en WndProc, no por patrón.

**Patrones que requirieron extensión por NO ser únicos (verificado v1.0.68):**
- `AI_CREATE` → 41 tokens (falso positivo en 0x000AF870; diverge en byte 40/46/49+).
- `GAME_FRAME_UPDATE` → 45 tokens (falso positivo en 0x00788100; diverge en byte 44:
  `48 89 78 20` MOV vs `0F 29 70 C8` MOVAPS).

`DetectGameVersion()` (declarado) lee la versión del PE para diagnóstico.

---

## 7. Instalación de hooks — HookManager + MovRaxRspFix

### 7.1 HookManager (`hook_manager.cpp`)
Singleton sobre MinHook + mutex. `InstallRaw(name, target, detour, &original)`:
1. **Validación `.pdata`**: si `target` no es inicio de `RUNTIME_FUNCTION` → **rechaza el hook**
   (evita hookear a mitad de función). Avisa si no está alineado a 16 (vtable-discovered OK).
2. **Análisis de prólogo**: detecta `48 8B C4` (mov rax,rsp).
3. Si tiene `mov rax,rsp` → camino MovRaxRsp (abajo). Si no → `MH_CreateHook` + `MH_EnableHook`
   estándar.

`Shutdown()`: solo `MH_DisableHook(MH_ALL_HOOKS)` — **nunca** `MH_RemoveHook`/`Uninitialize`
(los handlers `atexit` de Kenshi podrían llamar por punteros a trampolines liberados).
Restaura vtable hooks a mano y libera stubs.

`InstallVTableHook`: sobrescribe `vtable[index]` con `VirtualProtect`.

### 7.2 Bypass por software
Cada hook MovRaxRsp arranca con `bypassFlag=1` (passthrough). `Enable()`/`Disable()` para
MovRaxRsp **no** usan `MH_EnableHook/DisableHook` (re-parchean bytes + suspenden threads →
corrompen la cadena de detour tras un ciclo); en su lugar conmutan `bypassFlag` con
`InterlockedExchange`. Cuando bypass=1 el naked detour salta directo al raw trampoline.

### 7.3 MovRaxRspFix (`mov_rax_rsp_fix.cpp`) — el fix del prólogo `48 8B C4`
**Problema:** funciones MSVC que hacen `mov rax,rsp; ...; lea rbp,[rax-0xNN]` aliasan los slots
de los `push` con offsets `[rbp+XX]`. Cualquier byte extra en el stack (un return address de
más) desplaza los push 8 bytes y corrompe TODOS los locales.

**Solución:** dos stubs ASM generados en runtime por hook, en una página propia
(`VirtualAlloc`, `ALLOC_SIZE=0x200`):

Layout de la página (slots **globales por-hook**, no TLS):
```
+0x00 captured_rsp   (uint64)  RSP del caller del juego en entrada
+0x08 stub_rsp       (uint64)  RSP del hook C++ antes del swap
+0x10 saved_game_ret (uint64)  return address original del juego
+0x18 depth          (int32)   contador de reentrancia
+0x20 raw_trampoline (uint64)  trampolín MinHook crudo (para bypass/reentrante)
+0x28 bypass         (int32)   1=passthrough, 0=hook activo
+0x40 NAKED DETOUR   (~100 bytes de código)
+0xC0 TRAMPOLINE WRAPPER (~50 bytes)
```

- **Naked detour** (a donde salta MinHook): comprueba bypass → comprueba reentrancia
  (`depth++`, si >1 salta al raw trampoline) → guarda `captured_rsp` y `saved_game_ret` →
  `sub rsp, 0x1008` (gap 4 KB+8 que separa stacks y fija alineación a 16) → `call` (¡no jmp!)
  al hook C++ → `add rsp,0x1008` → restaura el return address → `ret` al juego.
- **Trampoline wrapper** (lo que el hook C++ llama como "original"): guarda `stub_rsp`, hace
  `rsp = captured_rsp`, parchea `[rsp]` con `return_point`, pone `rax=rsp`, salta a
  `trampoline+3` (salta el `mov rax,rsp` original). El original corre con CERO bytes extra en
  stack. Al `ret` cae en `return_point`, que restaura `stub_rsp` y vuelve al hook C++.

`STACK_GAP=0x1008` exacto: `0x1000` se queda dentro de una guard page (saltar 64 KB pasaría la
guard → crash) y `+8` da alineación de 16 correcta tras el `call`.

⚠️ **Thread safety:** los slots son globales por-hook, NO TLS. Es seguro porque la lógica de
juego de Kenshi es single-thread + el guard de reentrancia. Para multi-thread real habría que
migrar a TLS (anotado en el header). Vigilar si algún hook se llega a invocar desde el hilo de
render.

### 7.4 `BuildCustomCaller` (DEPRECATED)
Stub de 17 bytes (`mov rax,rsp; jmp [original+3]`). Sustituido por MovRaxRspFix porque
capturaba RSP a la profundidad de call equivocada (dentro del wrapper SEH del hook C++),
corrompiendo `[rbp+XX]`. `BuildRelayThunk` también deprecated (`add rax,8` desplazaba los
saves). No usar.

---

## 8. Offsets dudosos / fallos conocidos en 1.0.68

| Punto | Problema | Estado / mitigación |
|-------|----------|---------------------|
| **GameWorld anchor `dayTime`** | NO existe en 1.0.68 → StringXref de GameWorld nunca encuentra nada | Depende de disasm + hardcoded 0x2134110. **Recomendado:** cambiar anchor a `timeScale`. |
| **GameWorld layout** | Es instancia embebida, no `GameWorld**` | YA mitigado por `resolveGwObject` (acepta ambos). RVA correcta verificada 0x2134110. |
| **RVAs hardcodeadas** | Son GOG/1.0.68; en otra versión/plataforma apuntan a función distinta | Mitigado: no se aceptan si existe patrón y el patrón falló (binario distinto → crash). |
| **`CharacterSpawn` walk-back** | `RuntimeStringScanner::FindFunctionStart` (camino B) FALLA (2996 bytes xref→prólogo > límite + CC internos) | Camino A (orchestrator) lo resuelve por `.pdata::FindContaining`. `RtlLookupFunctionEntry` también lo cubre. |
| **`IsPrologue` no reconoce `48 8B C4`** | El walk-back del camino B no acepta mov rax,rsp como prólogo en algunas rutas | `RtlLookupFunctionEntry` lo cubre primero; cuidado al añadir funciones nuevas mov rax,rsp. |
| **`SquadAddMember` @0x928423** | Mid-function, no alineado a 16; hookear puede ser frágil | Resuelto por RTTI vtable slot 2; `.pdata` lo confirma como inicio válido pese a no estar alineado. Vigilar. |
| **`CharacterMoveTo`** | Patrón nullptr + anchor genérico + hook deshabilitado (5 params) | Sin sync de pathfinding; se compensa con polling de posición. |
| **Anchors genéricos** (`knockout`, `addItem`, `pathfind`, `heal`, `gate`...) | Pueden caer en función equivocada en Steam | Para los críticos se usa patrón único; los placeholders (§4.9) no se usan. |
| **`PlayerBase` valor estático** | En el archivo estático es 0x0002EF3700000000 (basura); se llena en runtime | Correcto: se valida tras cargar partida (Pass-2 + Retry). |
| **Patrones no únicos** | `AICreate`, `GameFrameUpdate` tenían falsos positivos | Corregidos extendiendo a 41/45 tokens. Re-verificar si cambia el binario. |
| **Slots MovRaxRsp globales (no TLS)** | Reentrancia entre hilos corrompería slots | Seguro hoy (single-thread + guard de depth). Migrar a TLS si se hookea algo del hilo de render. |

---

## 9. PE Layout de referencia (v1.0.68)
- `.text`  — VA 0x00001000, size 0x01671412 (~22.4 MB)
- `.rdata` — VA 0x01673000, size 0x0054A4CB (~5.3 MB)
- `.data`  — VA 0x01BBE000, size 0x0058E000
- ImageBase 0x140000000. `__security_cookie` @ RVA 0x21173C8 (no confundir con singletons).

## 10. Cómo añadir una función nueva (checklist)
1. Localizar string `[Clase::metodo]` única en `.rdata` → preferir StringXref.
2. Si no hay string, sacar patrón AOB del prólogo (12–15 bytes fijos mín.), wildcard en
   displacements/stack/calls. **Verificar 1 solo match en `.text`.**
3. Registrar en `RegisterBuiltinPatterns` (camino A) y/o `ResolveGameFunctions` (camino B) +
   añadir campo en `GameFunctions` + RVA fallback documentada.
4. Si el prólogo es `48 8B C4`, el HookManager aplica MovRaxRspFix automáticamente.
5. Marcar `critical=true` solo si su ausencia debe abortar el hook (entity/AI/time).
6. Validar con `.pdata` (alineación / inicio de función) antes de confiar.
</content>
</invoke>
