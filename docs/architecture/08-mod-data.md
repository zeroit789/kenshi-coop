# 08 — Datos del juego: el archivo `.mod` de Kenshi Co-op

> Mapa de los DATOS de juego que consume el mod. Cubre qué hay dentro de
> `kenshi-online.mod` / `kenshi-online-16.mod`, cómo los genera `tools/ModGen`,
> el flujo de uso en runtime (game start → squad → Character → facción) y la
> relación con `LobbyManager` (parche del string `204-gamedata.base` → `10-kenshi-online.mod`).
>
> Generado por el agente de RE (game-reverse-engineer). Solo documentación; no toca código fuente.
> Verificado empíricamente ejecutando `dotnet run -c Release explore` y `dump` sobre el `.mod` real (2026-06-18).

---

## 0. Resumen ejecutivo (5 líneas)

1. El `.mod` es un archivo de datos FCS/OCS tipo `MergeMod` con **44 registros**: facciones, squads, game starts, 16 personajes "Player N", un perro, diálogo y una raza.
2. Define el game start **"Multiplayer"**, que arranca con el squad **"Player 1 squad"** cuyo líder es el Character **"Player 1"**, dentro de la facción **"Player 1"**.
3. `ModGen` lee el `.mod` probado (read-modify-write fiel, conserva todo) y **clona "Player 1" → "Player 3..16"**, aplicando dos fixes: el del perro (squad 22→11) y el de la facción (`fundamental type` 9→4).
4. En runtime el servidor asigna a cada slot un string de facción (`10-kenshi-online.mod`, `12-...`) y el cliente **parchea en memoria** el literal `.rdata` `204-gamedata.base` para meter al host en la facción "Player N".
5. Resultado: cada jugador conectado crea un personaje jugable normal (no el perro), todos en facciones civiles que no huyen del combate.

---

## 1. Anatomía del `.mod`

### 1.1 Formato y ubicación

- **Formato**: archivo de datos nativo de Kenshi (FCS / Forgotten Construction Set), leído y escrito vía la librería **OpenConstructionSet (OCS)** — clases `ModFile`, `DataFile`, `Item`, `ReferenceCategory`, `Reference`, `Instance`.
- **Tipo de mod**: `MergeMod` (`data.Type`). `LastId = 44`.
- **Ubicaciones en el repo**:
  | Ruta | Rol |
  |------|-----|
  | `E:\Aplicaciones\Kenshi-Online\kenshi-online.mod` | **FUENTE** probada (`SrcPath` de ModGen). 44 items, base del clonado. |
  | `E:\Aplicaciones\Kenshi-Online\kenshi-online-16.mod` | **SALIDA** candidata de ModGen (`OutPath`). Autocontenida, 16 players + fixes. |
  | `E:\Aplicaciones\Kenshi-Online\dist\kenshi-online.mod` | Copia de distribución. |
  | `...\kenshi-online.mod.orig-backup-2026-06-03` | Backup histórico. |
- Tamaño ~31 KB. El `-16.mod` y el `.mod` fuente tienen el mismo tamaño porque el clonado de los players añade pocos bytes (los Character son registros pequeños, casi todo referencias).

### 1.2 Inventario completo de registros (44 items)

Conteo verificado (`types:` del flujo principal de ModGen):

| `ItemType` | Nº | Contenido |
|-----------|----|-----------|
| `Character` | 17 | "Player 1".."Player 16" (16) + 1 más (ver nota). En el `.mod` FUENTE solo existe "Player 1" y "Player 2" como reales; ModGen completa hasta 16. |
| `NewGameStartoff` | 14 | Inicios de partida. El propio del mod = **"Multiplayer"**; el resto son externos referenciados (`gamedata.base`, `rebirth.mod`). |
| `SquadTemplate` | 4 | "Player 1 squad", "Player 2 squad", "startoff- Wanderer dead", "startoff- Wanderer squad copy". |
| `Faction` | 2 | **"Player 1"** (`10-kenshi-online.mod`) y **"Player 2"** (`12-kenshi-online.mod`). |
| `Building` | 1 | (referencia/soporte). |
| `Town` | 1 | (referencia/soporte; "The Hub" es externo de `Newwworld.mod`). |
| `AnimalCharacter` | 1 | **"Bonedog dead"** (`24-kenshi-online.mod`) — el famoso perro. |
| `Race` | 1 | raza de soporte. |
| `DialoguePackage` | 1 | **"TALKING_TEST"** (`27-kenshi-online.mod`). |
| `Dialogue` | 1 | nodo de diálogo. |
| `DialogueLine` | 1 | línea de diálogo. |

> Nota sobre "17 Character": tras correr ModGen son los 16 "Player N" + el Character líder embebido del Wanderer; el `.mod` fuente real solo trae "Player 1" como plantilla clonable (los demás los crea ModGen). Por eso `VERIFY` reporta `Character=1` en "preserved non-player" (el único Character que NO es "Player N").

### 1.3 StringIds clave (relocatables, NO cambian entre versiones de Kenshi)

Los IDs de los registros del mod son **estables** porque viven en el `.mod`, no en el binario:

| StringId | Registro | Tipo |
|----------|----------|------|
| `10-kenshi-online.mod` | Facción "Player 1" | `Faction` |
| `12-kenshi-online.mod` | Facción "Player 2" | `Faction` |
| `11-kenshi-online.mod` | "Player 1 squad" | `SquadTemplate` |
| `13-kenshi-online.mod` | "Player 2 squad" | `SquadTemplate` |
| `19-kenshi-online.mod` | Character "Player 1" (plantilla clonada) | `Character` |
| `20-kenshi-online.mod` | Character "Player 2" | `Character` |
| `21-kenshi-online.mod` | Game start "Multiplayer" | `NewGameStartoff` |
| `22-kenshi-online.mod` | "startoff- Wanderer dead" (squad solo-perro) | `SquadTemplate` |
| `24-kenshi-online.mod` | "Bonedog dead" (el perro) | `AnimalCharacter` |
| `27-kenshi-online.mod` | "TALKING_TEST" | `DialoguePackage` |
| `30-kenshi-online.mod` | "startoff- Wanderer squad copy" | `SquadTemplate` |
| Players 3..16 | `38-..` etc. | generados por ModGen (`{id}-kenshi-online.mod`) |

---

## 2. Los registros principales en detalle

### 2.1 Game start "Multiplayer" (`21-kenshi-online.mod`)

```
[NewGameStartoff] "Multiplayer"  (21-kenshi-online.mod)
    squad -> "Player 1 squad" [SquadTemplate]  (11-kenshi-online.mod)   ← tras el FIX del perro
    town  -> "The Hub" [Town]   (18919-Newwworld.mod)
```

- **`squad`**: define con qué escuadrón arranca el jugador al elegir este inicio.
  - **Estado bugueado (original)**: apuntaba a `22-kenshi-online.mod` ("startoff- Wanderer dead"), un squad SIN líder Character que solo contiene un animal → el creador de personaje solo ofrecía el **perro**.
  - **Estado corregido (ModGen)**: re-cableado a `11-kenshi-online.mod` ("Player 1 squad"), que SÍ tiene un líder Character jugable → el jugador crea un personaje normal.
- **`town`**: punto de aparición = "The Hub" (externo de `Newwworld.mod`).

### 2.2 SquadTemplates relevantes

```
[SquadTemplate] "Player 1 squad"  (11-kenshi-online.mod)
    faction -> "Player 1" [Faction]    (10-kenshi-online.mod)   [v0=0]
    leader  -> "Player 1" [Character]  (19-kenshi-online.mod)   [v0=1]   ← líder jugable

[SquadTemplate] "Player 2 squad"  (13-kenshi-online.mod)
    faction -> "Player 2" [Faction]    (12-kenshi-online.mod)
    leader  -> "Player 2" [Character]  (20-kenshi-online.mod)

[SquadTemplate] "startoff- Wanderer dead"  (22-kenshi-online.mod)   ← el squad del bug
    animals -> "Bonedog dead" [AnimalCharacter]  (24-kenshi-online.mod)   [v0=1 v1=1]   ← solo el perro, sin leader
```

El squad "Player 1 squad" amarra los tres conceptos: **facción** (a quién pertenece), **leader** (qué Character controla el jugador). El squad bugueado solo tiene `animals` y ningún `leader`, de ahí que el juego ofreciera el perro.

### 2.3 Character "Player 1" (`19-kenshi-online.mod`) — la plantilla clonada

Plantilla de personaje jugable. **30 campos** (`Values`) + 6 categorías de referencias.

Campos (`Values`) relevantes:
```
NPC class = 9            faction importance = 1     combat stats = 1
female chance = 50       wages = 2500               armour grade = 1
body = FileValue         mesh = FileValue           (resto: bounty/money/stats = 0/False)
```

Categorías de referencia (`refCats = [clothing, dialogue package, dialogue package player, personality, weapon level, weapons]`):
```
clothing -> 550/551/564/557/2149/556-gamedata.base + 2308-clothes_v1.mod   [ropa de inicio]
dialogue package        -> "TALKING_TEST"  (27-kenshi-online.mod)
dialogue package player -> 5369-gamedata.base
personality             -> 17196-Dialogue.mod
weapon level            -> 912-gamedata.base
weapons                 -> 52295-rebirth.mod
```

> Importante: las referencias apuntan a `gamedata.base`, `clothes_v1.mod`, `Dialogue.mod`, `rebirth.mod`. Por eso **clonar "Player 1" garantiza referencias válidas**: son las mismas que un personaje que el juego ya carga sin errores. ModGen hace deep-copy (`new ReferenceCategory(c)`, `new Instance(ins)`) para que los clones no aliasen entre sí.

### 2.4 Facciones "Player 1" / "Player 2" (`10-` / `12-kenshi-online.mod`)

Ambas idénticas en estructura. Campos clave para el comportamiento de combate:

```
[Faction] "Player 1"  (10-kenshi-online.mod)
    val anti slavery = True
    val enemy classification = -10
    val fundamental type = 0     ← EN EL .mod FUENTE (ver fix abajo); ModGen lo deja en 4
    val num ranks = 1
    val squad formation = 0
    AI Goals   -> 957-gamedata.base
    dialog default -> 5369-gamedata.base
    relations  -> 200-gamedata.base   [v0=-100 v1=100]
```

- **`fundamental type`** (también llamado en RE `fundamentalNPCType`, `Faction.h:64`, offset `+0x34`, enum `CharacterTypeEnum`):
  - `9` = `OT_ADVENTURER` → comportamiento original buggeado: los enemigos huyen del jugador.
  - `0` = `OT_NONE` → primer fix erróneo: deja la facción sin clase de comportamiento válida; enemigos siguen huyendo.
  - `4` = `OT_CIVILIAN` → **valor correcto**, igual que la facción vanilla del jugador "Nameless" (`204-gamedata.base`), verificado con el modo `basefac` de ModGen.

> El explore actual reporta `fundamental type = 0` porque lee el `.mod` FUENTE (que aún tiene el valor sin corregir); ModGen lo reescribe a `4` al generar el `-16.mod`.

---

## 3. Cómo ModGen genera el `.mod`

Código: `tools/ModGen/ModGen/Program.cs` (.NET 10, usa OpenConstructionSet).
Ejecutar: `cd tools/ModGen/ModGen && dotnet run -c Release <modo>`.

### 3.1 Modos disponibles

| Modo | Qué hace |
|------|----------|
| (sin args) | **Genera** `kenshi-online-16.mod`: clona players + aplica fixes + verifica. |
| `explore [ruta]` | Vuelca game starts, squads, facciones y characters con su cableado de referencias. Acepta ruta opcional para inspeccionar el candidato. |
| `dump` | Lista las propiedades reflexivas de `Item`/`ReferenceCategory`/`Reference` (modelo de datos OCS). |
| `basefac` | Carga `gamedata.base` del juego y vuelca la facción "Nameless" (`204-gamedata.base`) — sirvió para verificar `fundamental type = 4`. |

### 3.2 Flujo de generación (modo sin args)

1. **Lee** `kenshi-online.mod` vía `ModFile.ReadDataAsync()` (read-modify-write fiel → conserva TODOS los registros originales: Multiplayer, facciones, squads, Player 1/2, perro, diálogo).
2. Localiza el Character `Player 1` (plantilla). Si no existe → aborta.
3. **Clona** "Player 1" a "Player N" para `N = 1..16` que no existan ya:
   - `id = nextId++`, `stringId = "{id}-kenshi-online.mod"`.
   - Deep-copy de `Values`, `ReferenceCategories` y `Instances` (clones independientes).
   - `new Item(ItemType.Character, id, name, stringId, player1.SaveData, values, refCats, instances)`.
4. **FIX del perro**: en el game start "Multiplayer", reescribe la referencia `squad` de `22-kenshi-online.mod` (Bonedog) → `11-kenshi-online.mod` (Player 1 squad).
5. **FIX de facciones**: en "Player 1" y "Player 2", pone `Values["fundamental type"] = 4` (OT_CIVILIAN).
6. **Escribe** `kenshi-online-16.mod` (`WriteDataAsync`). Nunca sobrescribe el `.mod` fuente.
7. **Verifica** por re-lectura fiel: 44 items, 16 Player characters, resto preservado.

### 3.3 Modelo de datos OCS (del modo `dump`)

```
Item:               SaveData(rw), Id(ro), Instances, Name(rw), ReferenceCategories,
                    StringId(ro), Type(rw), Values(OrderedDictionary)
ReferenceCategory:  Name(ro), References
Reference:          TargetId(rw), Value0(rw), Value1(rw), Value2(rw)
```

`TargetId` es reescribible → así el FIX del perro re-cablea referencias. `Values` es un `OrderedDictionary<string,object>` → de ahí los `fac.Values["fundamental type"]` con `Convert.ChangeType` para respetar el tipo runtime del valor.

---

## 4. Flujo de uso en runtime (datos → jugador en partida)

Cadena completa de cómo el mod usa estos datos, de extremo a extremo:

```
Servidor (KenshiMP.Server\server.cpp:682-716)
  └─ asigna a cada slot un string de facción del .mod:
       slot 0 (Player 1) → "10-kenshi-online.mod"
       slot 1 (Player 2) → "12-kenshi-online.mod"
     envía S2C_FactionAssignment { strLen, factionStr, slot } por ENet
        │
        ▼
Cliente (KenshiMP.Core\net\packet_handler.cpp:1828 HandleFactionAssignment)
  └─ lee factionStr + slot (validación: 0 < strLen <= 32)
  └─ LobbyManager::OnFactionAssigned(factionStr, slot)   (lobby_manager.cpp:10)
  └─ si fase == MainMenu/GameReady → ApplyFactionPatch()  (patch inmediato)
        │
        ▼
LobbyManager::ApplyFactionPatch (lobby_manager.cpp:90)
  └─ FindFactionStringAddress()  → localiza el literal "204-gamedata.base" en .rdata
  └─ VirtualProtect RWX → memcpy(addr, "10-kenshi-online.mod\0") → restaura protección
        │
        ▼
Juego carga el game start "Multiplayer" (21-kenshi-online.mod)
  └─ squad "Player 1 squad" (11-) → leader Character "Player 1" (19-) → facción "Player 1" (10-)
  └─ El host queda DENTRO de la facción "Player N" (fundamental type=4 OT_CIVILIAN)
     → personaje jugable normal, enemigos no huyen.
```

### Diagrama de relación de datos (dentro del `.mod`)

```
NewGameStartoff "Multiplayer" (21-)
        │ squad
        ▼
SquadTemplate "Player 1 squad" (11-)
        ├── faction ──► Faction "Player 1" (10-)   [fundamental type = 4]
        └── leader  ──► Character "Player 1" (19-)  ── clonado por ModGen ──► Player 2..16
                              │
                              ├── dialogue package ──► "TALKING_TEST" (27-)
                              ├── clothing/weapons ──► gamedata.base / rebirth.mod (externos válidos)
                              └── personality ──────► Dialogue.mod (externo)
```

---

## 5. Relación con LobbyManager — el parche del string `.rdata`

**Por qué existe**: el motor de Kenshi hardcodea en `.rdata` el literal de la facción por defecto del jugador, `"204-gamedata.base"` (la facción vanilla "Nameless"). El mod no puede cambiar eso desde el `.mod`; lo cambia **en memoria** en runtime.

**`LobbyManager::FindFactionStringAddress`** (`lobby_manager.cpp:37`) usa dos estrategias:

1. **Offsets conocidos** (rápido, dependiente de versión):
   | Offset (sobre `moduleBase`) | Plataforma |
   |-----------------------------|-----------|
   | `+0x16C4258` | Kenshi Steam v1.0.68 |
   | `+0x16C2F68` | Kenshi GOG v1.0.68 |
   - Valida que el candidato tenga forma `NNN-nombre.base` (contiene `-` y `.`), leyendo 17 bytes bajo SEH.
2. **Escaneo de `.rdata`** (resiliente a versión): recorre la sección buscando el patrón de bytes `"204-gamedata.base"` (17 bytes) bajo SEH. Fallback si los offsets fijos no matchean.

**Escritura del parche** (`ApplyFactionPatch`, `lobby_manager.cpp:90`):
- Original `"204-gamedata.base"` = 17 chars + null = 18 bytes.
- Strings de facción del mod (`"10-kenshi-online.mod"`, hasta 24 chars) son MÁS LARGOS → escribe `writeLen+1` bytes (con null), sobrescribiendo algunos bytes adyacentes del pool de literales `.rdata` (declarado seguro: bytes adyacentes no críticos).
- `VirtualProtect → PAGE_EXECUTE_READWRITE → memcpy → restaura`.

### Seguridad del hook (RE / thread-safety)

- Todo el acceso a memoria de Kenshi va bajo **SEH** (`__try/__except`), en wrappers POD aparte porque MSVC prohíbe `__try` en funciones con objetos C++ (spdlog, std::string). Fallo elegante → devuelve `0`/`false`, sin crash.
- El patch se aplica en fase `MainMenu`/`GameReady` (antes de cargar el save), evitando carreras con el hilo de lógica del juego una vez la partida está corriendo.
- Offsets de FUNCIÓN/literal (`+0x16C4258`) **cambian por versión/plataforma**; el escaneo de `.rdata` es el plan B resiliente. Los StringIds del `.mod` (`10-kenshi-online.mod`) son estables.

---

## 6. Cabos sueltos / verificación pendiente

- **`fundamental type` en el `.mod` fuente**: hoy está en `0`. El `-16.mod` generado lo corrige a `4`. Confirmar que el `.mod` que se distribuye (`dist/`) sea el `-16` regenerado y no el fuente con `0`. (Sugerencia: el flujo de release debería partir de `kenshi-online-16.mod`.)
- **Offsets del string de facción**: solo cubren v1.0.68 Steam/GOG. Si Kenshi se actualiza, regenerar offsets o confiar en el escaneo `.rdata` (ya implementado).
- **"17 Character" vs 16 players**: confirmar qué es el Character #17 preservado (probablemente el líder Wanderer embebido en `30-kenshi-online.mod`). No afecta al funcionamiento.
- El `explore` actual NO vuelca DialoguePackage/AnimalCharacter/Race/Town como secciones propias (el filtro `Where` solo incluye NewGameStartoff/SquadTemplate/Faction/Character); aparecen solo como destinos de referencias. Para volcarlos habría que ampliar el filtro (no se hizo: no editar código).

---

## 7. Referencias de archivos

| Archivo | Rol |
|---------|-----|
| `tools/ModGen/ModGen/Program.cs` | Generador del `.mod` (clonado + fixes + verify). |
| `kenshi-online.mod` | Datos fuente probados (44 items). |
| `kenshi-online-16.mod` | Salida autocontenida (16 players + fixes). |
| `KenshiMP.Server/server.cpp:682-716` | Asignación de facción por slot (S2C_FactionAssignment). |
| `KenshiMP.Core/net/packet_handler.cpp:1828` | `HandleFactionAssignment` (recibe y aplica el patch). |
| `KenshiMP.Core/game/lobby_manager.cpp` | `FindFactionStringAddress` + `ApplyFactionPatch` (parche `.rdata`). |
| `KenshiMP.Core/game/lobby_manager.h` | Interfaz de `LobbyManager`. |
| `KenshiMP.Core/game/spawn_manager.cpp` | Localiza las plantillas GameData "Player N" en heap al cargar. |
