# Docs Review 02 — Hooks, combate y sistema puppet (¿por qué el host no actúa?)

**Fecha:** 2026-06-18
**Documentos revisados:** `docs/HOOKS.md`, `docs/MULTIPLAYER-FIXES-2026-06-04.md`,
`docs/plans/2026-03-10-clean-puppet-implementation.md`,
`docs/plans/2026-03-10-clean-puppet-remote-player-design.md`
**Verificado contra código vivo:** `KenshiMP.Core/hooks/{ai,combat,movement,entity}_hooks.cpp`,
`KenshiMP.Core/sync/entity_registry.cpp`, `KenshiMP.Core/core.cpp`

> ⚠️ Los dos planes `clean-puppet-*` describen un diseño APROBADO, NO lo que terminó
> en el binario. Hay desviaciones grandes entre el plan y el código que shippeó.
> Este documento prioriza lo que el código REALMENTE hace.

---

## 1. Cómo el mod marca y bloquea los chars REMOTOS (puppets)

### El set `s_remoteControlled` (ai_hooks.cpp)
- Es un `std::unordered_set<void*>` por VALOR de puntero, con mutex. NO derefencia memoria del juego.
- API: `MarkRemoteControlled(char)`, `UnmarkRemoteControlled(char)`, `IsRemoteControlled(char)`,
  y el diagnóstico de solo lectura `RemoteControlledCount()`.

### QUIÉN se marca como remote — **gate doble** (ai_hooks.cpp:77-87)
Dentro de `Hook_AICreate`, un char se marca SOLO si se cumplen LAS DOS:
1. `registry.GetNetId(character) != INVALID_ENTITY` (el char está registrado en la red), Y
2. `info->isRemote == true`.

`isRemote=true` se pone EXCLUSIVAMENTE en `EntityRegistry::RegisterRemote` (entity_registry.cpp:61),
que SOLO se llama desde `packet_handler.cpp` cuando el SERVIDOR envía el spawn de OTRO jugador
(`S2C_EntitySpawn` / deferred spawn). El char local del host se registra por la ruta de
`entity_hooks` con `isRemote=false` (entity_registry.cpp:30, `Register()`).

**Conclusión:** en operación normal de host en solitario (sin peers conectados), el char del
host NO debería marcarse remote. El gate es correcto en el papel.

### QUÉ se bloquea a un char remote (lo que el código REALMENTE hace)
| Acción | Hook | ¿Bloquea al remoto? | ¿Toca al host? |
|---|---|---|---|
| Movimiento AI (`MoveTo`) | movement_hooks `Hook_MoveTo` | SÍ (`return` sin llamar original) si `IsRemoteControlled` | NO — pero el hook **NO está instalado** (mov rax,rsp) |
| Posición física (`SetPosition`) | movement_hooks `Hook_SetPosition` | NO bloquea de verdad (solo loguea, sigue llamando original) | NO — y tampoco está instalado |
| Daño AI (`ApplyDamage`) | combat_hooks | **El plan pedía bloquear si atacante es remote — NUNCA se implementó** | NO |
| Crear controlador AI (`AICreate`) | ai_hooks `Hook_AICreate` | NO bloquea, solo marca | El host pasa por aquí, pero solo se marca si isRemote |
| Cargar behavior tree (`AIPackages`) | ai_hooks `Hook_AIPackages` | NO bloquea (siempre carga) | NO |

**Hallazgo importante:** la supresión de movimiento de remotos depende de `Hook_MoveTo`, pero
ese hook **NO se instala** (prólogo `mov rax, rsp` + 5º parámetro en pila → crash garantizado;
ver movement_hooks.cpp:214-224 e Install()). Es decir, la supresión "real" de movimiento de los
puppets se reduce a sobrescribir su posición cada frame vía physics-chain (per el design doc),
NO a bloquear sus órdenes. El único hook activo que distingue remoto del set es el cuerpo de
`Hook_MoveTo`/`Hook_SetPosition`, que están desinstalados.

---

## 2. Los 14 hooks — quién toca combate/IA/movimiento/spawn

(De HOOKS.md, 17 módulos: 14 "activos" pero varios no hookean nada.)

**Tocan SPAWN:**
- `entity_hooks` (CharacterSpawn 0x581770) — **CRÍTICO**, activo. Registra chars locales, hijack de NPC para remotos.
- `squad_spawn_hooks` (SquadSpawnBypass) — activo. Fuerza creación de NPC para el hijack remoto.
- `squad_hooks` — instala pero **no hookea nada** (SquadCreate/AddMember DESACTIVADOS por corrupción). Solo guarda el puntero raw para inyección.

**Tocan IA:**
- `ai_hooks` (AICreate 0x622110, AIPackages) — activo. SIEMPRE llama al original (el host conserva su controlador AI). Marca remotos. **No bloquea nada del host.**

**Tocan COMBATE:**
- `combat_hooks` (CharacterDeath 0x7A6200, CharacterKO) — activo, pero SOLO sincroniza muerte/KO (push a ring buffer diferido).
- `ApplyDamage` (0x7A33A0) — **DESACTIVADO** (mov rax,rsp, 300+/s → crash "attack unprovoked").
- `StartAttack`/`IssueOrder` (DIAG) — **DESACTIVADO**. El RE (2026-06-18) demostró que 0x722EF0 que resolvía `funcs.IssueOrder` es **MyGUI/UString = UI**, NO el dispatcher de órdenes del jugador. La orden real entra por **Tasker/GOAPTaskMgr → Task_MeleeAttack/Task_GetUp**, cuya RVA sigue **SIN resolver**.

**Tocan MOVIMIENTO:**
- `movement_hooks` (SetPosition, MoveTo) — **AMBOS DESACTIVADOS** (mov rax,rsp). El módulo instala pero no hookea nada. Posición por polling en OnGameTick.

**Otros activos:** world_hooks (zonas), inventory_hooks, faction_hooks, building_hooks (crash-prone),
time_hooks (nunca dispara en Steam), game_tick_hooks, render_hooks (driver de tick real en Steam + WndProc),
char_tracker_hooks. Pasivos/no-op: save_hooks, input_hooks, resource_hooks.

**¿Algún hook activo bloquea acciones del host?** Revisando los cuerpos: NO de forma directa.
Todos los hooks que distinguen "remoto" (MoveTo, SetPosition, el ApplyDamage del plan) o están
DESINSTALADOS o no llegaron a implementarse. El único bloqueo real que existe (`Hook_MoveTo return`)
ni siquiera está activo. **Para que el host quedase bloqueado haría falta que su puntero entrase en
`s_remoteControlled` Y que el hook de bloqueo estuviese activo — la segunda condición no se da hoy.**

---

## 3. MULTIPLAYER-FIXES-2026-06-04 — qué se arregló y qué NO

**Arreglado:**
- DeferredSpawnQueue (late-join): spawns que llegaban antes de `GameReady` se descartaban; ahora se encolan.
- Timeout duro 90s en `PollForGameLoad` (deadlock de Steam si `GameWorldSingleton` no resuelve).

**Known issues NO resueltos (relevantes al combate):**
- **Combat client-authoritative / `ApplyDamage` hook DESACTIVADO** → "solo death/KO sync, no daño intermedio". Catalogado LOW/aceptable.
- "damage bars don't sync — known limitation".
- ReconcileLocal stub (sin predicción Fase 7).

**Sobre el combate LOCAL del host (no sync):** el documento NO afirma en ningún punto que el combate
local del host esté roto. Todas las limitaciones de combate listadas son de SINCRONIZACIÓN entre
clientes (daño no se propaga, death/KO sí). El combate local single-player del host se asumía
funcional. Que ahora el host NO pelee/levante/recupere es una regresión NUEVA no cubierta por este doc.

---

## 4. Flujo de spawn del puppet vs. el del host

- **Puppet (remoto):** `S2C_EntitySpawn` → `RegisterRemote(isRemote=true, gameObject=null)` →
  `FactoryCreate` (RVA 0x583400, único path tras la limpieza del plan) o NPC hijack →
  `SetGameObject(netId, charPtr)` mapea el puntero → `AICreate` lo ve isRemote → `MarkRemoteControlled`.
- **Host (local):** char creado por el juego → `entity_hooks` → `EntityRegistry::Register()` con
  `isRemote=false`, `authority=Player`, `localState=LocalOwned`. NUNCA pasa por `RegisterRemote`.

**Vector de riesgo (colisión de identidad):** `RegisterRemote` crea la entrada con `gameObject=nullptr`
y luego `SetGameObject` asocia el puntero del char. Si por error de la ruta de hijack/claim el
puntero del char del HOST se mapease a un netId remoto (p.ej. el hijack agarra el char equivocado,
o reuso de puntero tras un despawn sin `UnmarkRemoteControlled`), entonces `GetNetId(hostPtr)`
devolvería un netId con `isRemote=true` → en el siguiente `AICreate` el host se marcaría remote.
Esto es exactamente la hipótesis que el diagnóstico `[DIAG-REMOTE]` está montado para confirmar/descartar.

---

## 5. El diagnóstico que YA existe para esta pregunta exacta

En `core.cpp:3518-3567` hay un bloque `[DIAG-REMOTE]` construido específicamente para responder
"¿el mod marcó al host como remote-controlled?". Compara host vs NPC vecino y vuelca `setSize`:

- `host=remote-controlled SÍ` → **CAUSA CONFIRMADA** (desmarcar el char del host).
- `host=NO + setSize=0` → el host NO es puppet; la causa está en OTRA capa.
- `host=NO + setSize>0` → el host no es puppet (el set tiene peers/NPC hijack); causa en otra capa.

Justo encima (`core.cpp:3479-3516`) está `[DIAG-AITASK]`, que vuelca el sistema de tareas del host:
`AITaskSytem` en `char+0x20`, cola de jobs en `aiTask+0x2E8` (size `+0x2F0`). Sus veredictos apuntan
a la pista alternativa más fuerte: **host con AITaskSytem válido pero cola de jobs VACÍA mientras el
NPC vecino tiene jobs>0 → al char del host NO le llegan órdenes/tareas (el motor no le encola nada).**

---

## 6. Veredicto orientado a la causa

La cadena de evidencia apunta a que el char del host inerte NO es (principalmente) por estar en
`s_remoteControlled`, porque:
1. El gate de marcado exige `isRemote=true`, que solo lo pone la ruta de red para peers.
2. Aunque estuviera marcado, el hook que bloquearía su movimiento (`Hook_MoveTo`) NO está instalado,
   y el `ApplyDamage` que bloquearía su daño NUNCA se implementó.

La causa más probable según `[DIAG-AITASK]` y el RE de 2026-06-18 es la **capa de órdenes/tareas**:
la orden real del jugador (atacar, levantarse, recuperarse) entra por **Tasker/GOAPTaskMgr**
(Task_MeleeAttack / Task_GetUp), cuya RVA sigue sin resolver, y `IssueOrder` que se creía el
dispatcher era UI (MyGUI). Si la cola de jobs del host está vacía, el `think` no tiene nada que
ejecutar → char vivo, con tick, pero inerte.

**Próximo paso de verificación (no asumir):** correr y leer `[DIAG-REMOTE]` y `[DIAG-AITASK]` en vivo:
- Si `[DIAG-REMOTE]` da host=remote-controlled → es el set; desmarcar al host.
- Si da host=NO + setSize=0 (lo esperado) → descartar puppet y atacar la capa de órdenes/jobs (resolver Tasker/GOAPTaskMgr).
