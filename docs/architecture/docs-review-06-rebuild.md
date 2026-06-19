# Docs Review 06 — `rebuild/` vs `KenshiMP.Core` (control del char del host inerte)

**Fecha:** 2026-06-18
**Pregunta:** ¿El `rebuild/` implementa el control de personajes / `playerControlled` / órdenes / char del host de forma DISTINTA (mejor o funcional)? ¿Tiene algo que desbloquee el combate congelado del host que `KenshiMP.Core` no tenga?

## TL;DR (respuesta corta)

**NO. `rebuild/` NO tiene nada útil para desbloquear el combate del host. Es una versión ANTERIOR e INFERIOR de `KenshiMP.Core`.**

- `rebuild/` y `KenshiMP.Core` son el **mismo proyecto en dos momentos**. `rebuild/` es un snapshot más antiguo (core.cpp mtime **2026-06-16 23:35**); `KenshiMP.Core` es la versión viva (core.cpp mtime **2026-06-18 19:42**, hoy) y es la que **compila el binario en uso** (CMakeLists raíz línea 44: `add_subdirectory(KenshiMP.Core)`; `rebuild/` NO se compila).
- El control de personajes (`player_controller.cpp`, `movement_hooks.cpp`, `WritePlayerControlled`/`SetPlayerControlled`, `isPlayerControlled`) es **idéntico o casi idéntico** en ambos. No hay enfoque alternativo en rebuild.
- **Toda la investigación del combate congelado del host vive SOLO en `KenshiMP.Core`.** `rebuild/` ni siquiera tiene el diagnóstico, y encima conserva el bug ingenuo que Core ya descartó.

## Cómo se relacionan las dos carpetas

Misma estructura de árbol, mismos nombres de archivo, mismas rutas (`game/`, `hooks/`, `net/`, `sync/`, `sdk/`, `sys/`, `ui/`). La diferencia es de **madurez**, no de diseño:

| Archivo | rebuild | Core | Qué pasó |
|---|---|---|---|
| `core.cpp` | 181 KB / 3844 líneas | 280 KB / 5358 líneas | Core +1500 líneas: todo diagnóstico del combate congelado + `[FIX-SIM]` del gate |
| `game/game_character.cpp` | 40 KB | 71 KB | Core añade cadena robusta `GameWorld→player→playerCharacters`, `GetPlayerFactionDirect`, `GetPlayerPrimaryCharacterDirect`, sondas de facción del host |
| `hooks/combat_hooks.cpp` | 13.7 KB | 21.7 KB | Core añade todo el bloque DIAG-COMBAT (hooks de `StartAttack`/`IssueOrder`, ring buffer de eventos) |
| `hooks/ai_hooks.cpp/.h` | sin `RemoteControlledCount()` | con `RemoteControlledCount()` | Core añade diagnóstico DIAG-REMOTE |
| `game/game_world.cpp` | 5.3 KB | 10.4 KB | Core añade `ResolveWorldObject()` (instancia directa vs puntero) |
| `sync/authority_validator.*` | **no existe** | existe | Solo Core |
| `sync/deferred_spawn_queue.*` | **no existe** | existe | Solo Core |
| `sync/pending_snapshot_queue.*` | **no existe** | existe | Solo Core |

Archivos **idénticos** verificados con `diff`: `player_controller.cpp`, `movement_hooks.cpp`. Esto demuestra que el mecanismo de control de personaje no cambió: rebuild no aporta una vía distinta.

## Control de personajes / `playerControlled` — ¿hay enfoque distinto en rebuild?

No. En ambos:

- `CharacterAccessor::IsPlayerControlled()` / `SetPlayerControlled()` leen/escriben `offsets.character.isPlayerControlled` (descubierto por differential probing player-char vs NPC, en `game_offset_prober.cpp`). Idéntico.
- `WritePlayerControlled(charPtr, controlled)` existe en los dos con el mismo cuerpo.
- **En los dos, `WritePlayerControlled` está DESHABILITADO en el flujo principal** (core.cpp: `// 5. WritePlayerControlled DISABLED — not needed for visibility, and can crash`). Rebuild no lo usa de forma distinta ni "mejor".
- El flujo de órdenes/tareas (`IssueOrder`, `StartAttack`, Tasker/GOAPTaskMgr) **solo está instrumentado en Core** (DIAG-COMBAT). Rebuild no toca este flujo en absoluto.

## El punto clave: el char del host inerte

**`rebuild/` tiene el bug; `KenshiMP.Core` tiene el diagnóstico y el fix.**

### Lo que hace rebuild (ingenuo, descartado por Core)
`rebuild/src/core.cpp` (~línea 2201): para despausar la sim escribe directamente
```
GameWorld.paused = 0   // Memory::Write(gwPtr + offsets.world.paused, 0)
```
Eso es exactamente el enfoque que Core probó y **demostró insuficiente**. Rebuild no tiene `0x8B9`, ni `FIX-SIM`, ni `DiagTickPump`, ni `gatePause`, ni `SEH_ReadPauseGate` (grep = 0 ocurrencias).

### Lo que descubrió Core (y rebuild no sabe)
`KenshiMP.Core/core.cpp` (bloque `[FIX-SIM]`, ~línea 3590), confirmado por RE de bytes el 2026-06-18:

- El gate REAL que decide si corre el AI tick de personajes es **`GW+0x8B9`**, no `offsets.world.paused`.
- `mainLoop` (0x788A00) en `0x788FF5` hace `cmp byte[GW+0x8B9],0 ; jne paused`. Con ese byte ≠ 0 → **se salta `updateCharacters` → ningún char recibe el AI tick `[vtbl+0xE8]`** (combate / Jobs / levantarse / recuperar KO). El reloj y el movimiento van por otra rama (`[vtbl+0x270]`) → de ahí el síntoma exacto **"reloj corre, personajes inertes"**.
- Por qué se queda pegado: el setter oficial `0x787D40` hace `GW+0x8B9 = argBool OR (gameSpeed[GW+0x700] == 0.0f)`. Si el host pausó con barra espaciadora (`gameSpeed=0.0`), llamar `SetPaused(false)` NO basta: el OR re-pega el flag a 1 mientras `gameSpeed` siga en 0.0.
- **FIX de Core:** restaurar `gameSpeed=1.0` ANTES de llamar al setter de despausa, para que el OR no dispare. Solo cuando `needsUnpause` (pausa fantasma `ghostPause` o multiplayer activo); en host solo con pausa intencional no toca nada.

Core además tiene DIAG-REMOTE (`[DIAG-REMOTE] host=... IsRemoteControlled=...`) para verificar si el mod marcó por error al host como remote-controlled (otra hipótesis del char inerte), apoyado en `ai_hooks::RemoteControlledCount()` — que rebuild tampoco tiene.

## Conclusión

1. **rebuild = Core viejo (16 jun) sin compilar.** No es una reimplementación alternativa; es el ancestro del código actual. Cualquier "idea" suya ya está en Core, mejorada o descartada con motivo documentado.
2. **No hay ninguna solución al char del host inerte en rebuild.** Al contrario: rebuild conserva el enfoque ingenuo (`paused=0` directo) que Core ya probó y demostró que NO arregla el combate.
3. **El conocimiento útil va en sentido contrario:** Core → (no hay nada que traer de) rebuild. La pista buena para desbloquear el combate ya está EN Core, en el bloque `[FIX-SIM]` (gate `GW+0x8B9` + OR con `gameSpeed==0.0`) y en DIAG-REMOTE.
4. **Recomendación:** ignorar `rebuild/` para este problema. El siguiente paso está en `KenshiMP.Core`: confirmar en runtime que el `[FIX-SIM]` del gate `GW+0x8B9` realmente reactiva `updateCharacters`, y si el char sigue inerte con el gate ya despejado, seguir por las hipótesis H3/H4 que el propio Core deja apuntadas (AITaskSystem nulo / cola de Jobs vacía, ~líneas 2919 y 3475 de core.cpp).
