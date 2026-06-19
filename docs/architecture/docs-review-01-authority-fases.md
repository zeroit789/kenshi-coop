# Revisión de docs de autoridad y fases — Kenshi Co-op

**Fecha:** 2026-06-18
**Autor:** Onyx (review de los 4 docs de diseño + verificación cruzada en código real)
**Objetivo:** Entender el modelo de autoridad/ownership y por qué el char del HOST ('Dani') está vivo
pero NO pelea, NO se levanta de cama, NO se recupera de KO.

> ⚠️ AVISO IMPORTANTE: los docs de autoridad (`AUTHORITY-IMPLEMENTATION-COMPLETE.md`,
> `authority-architecture-audit-2026-06-04.md`, `authority-progress-2026-06-04.md`) están
> **DESACTUALIZADOS / contradictorios** respecto al código real. Verifiqué la enum en
> `types.h` y NO coincide con lo que dicen los docs. No tomar esos docs como verdad sin
> contrastar con el código. Lo marco caso por caso abajo.

---

## 1. MODELO DE AUTORIDAD / OWNERSHIP (estado REAL en código)

### 1.1 Enum `AuthorityType` — qué dice cada doc vs el código

| Fuente | Valores | ¿Real? |
|--------|---------|--------|
| `AUTHORITY-IMPLEMENTATION-COMPLETE.md` (Known Limitation #2) | `None, Local, Remote, Host` | ❌ FALSO |
| `authority-architecture-audit-2026-06-04.md` (Issue 1) | `None, Local, Remote, Host` | ❌ FALSO |
| `authority-progress-2026-06-04.md` (Known Issues) | `None, Local, Remote, Host` | ❌ FALSO |
| **CÓDIGO REAL** `KenshiMP.Common/include/kmp/types.h:202` | `None=0, Server=1, Player=2, Transferring=3` | ✅ VERDAD |

**Conclusión:** los docs avisan de un "bug cosmético" de la enum (`None/Local/Remote/Host`)
que **YA NO EXISTE**. El código real ya usa `None/Server/Player/Transferring`. Los docs son
de un estado anterior del proyecto (2026-06-04) y quedaron obsoletos.

```cpp
// types.h:201 — Authority model (spec §2.3) — ESTADO REAL
enum class AuthorityType : uint8_t {
    None         = 0, // Sin owner — gestionado por servidor
    Server       = 1, // Servidor autoritativo (NPCs, objetos de mundo)
    Player       = 2, // Entidad propiedad de un jugador
    Transferring = 3, // Handoff de autoridad en curso
};
```

### 1.2 Enum `LocalAuthorityState` — cómo el CLIENTE trata updates entrantes

```cpp
// types.h:211 — ESTADO REAL
enum class LocalAuthorityState : uint8_t {
    LocalOwned   = 0, // MI personaje — predecir localmente, reconciliar con servidor
    RemoteOwned  = 1, // Entidad de otro jugador — solo interpolar
    ServerOwned  = 2, // NPC/objeto de mundo — aplicar updates del servidor directo
    PendingSpawn = 3, // Update llegó antes del paquete de spawn
    Destroyed    = 4, // Entidad eliminada, rechazar todos los updates
};
```

**Mapeo de estados al char del HOST:**
- El char del host ('Dani') DEBE ser `LocalOwned` (autoridad `Player`, owner = mi propio PlayerID).
- Un char REMOTO (otro jugador) es `RemoteOwned`.
- Un NPC es `ServerOwned`.

### 1.3 Helpers de validación (EntityRegistry)

Definidos según docs (`entity_registry.cpp`):
- `IsLocalOwned()` — entidad de este cliente
- `IsRemoteOwned()` — entidad de otro cliente
- `IsServerOwned()` — NPC/mundo
- `IsValidGeneration()` — chequeo anti-stale
- `GetOwnerPlayerId()` — devuelve el PlayerID dueño
- `IsRemote()` — atajo: ¿es de otro?

### 1.4 ⭐ EL FLAG QUE REALMENTE IMPORTA: `EntityInfo.isRemote`

Verificado en `KenshiMP.Core/hooks/ai_hooks.cpp`. El gating de IA NO usa `AuthorityType` ni
`LocalAuthorityState` directamente — usa el bool **`info->isRemote`** del `EntityInfo` del registro.

```cpp
// ai_hooks.cpp:77-87 — Hook_AICreate
EntityID netId = registry.GetNetId(character);
if (netId != INVALID_ENTITY) {
    auto info = registry.GetInfo(netId);
    if (info.has_value() && info->isRemote) {   // ← ÚNICA condición de gating
        MarkRemoteControlled(character);        // ← mete el char en s_remoteControlled
    }
}
```

**Este es el punto crítico para el bug del host.** (ver sección 3)

---

## 2. LAS FASES — DOS sistemas de numeración DISTINTOS (no confundir)

Hay DOS documentos de "fases" que numeran cosas diferentes. Es fácil confundirse.

### 2.A `PHASES.md` — fases del CICLO DE VIDA multiplayer (0–12)

Este es el doc operativo "qué funciona". Reporta **13/13 fases WORKING**. Relevantes:
- **Phase 3: Entity Spawning** — WORKING. Incluye `AI suppression for remotes (ai_hooks.cpp)`.
- **Phase 4: Position Sync** — WORKING. Interpolación, dead reckoning.
- **Phase 5: Combat Sync** — WORKING. `ApplyDamage hook → C2S_AttackIntent`, death/KO handlers.
- Nota Phase 3: *"ProcessSpawnQueue() deprecated — only in-place replay is safe. Fallback
  direct spawn works but characters may crash after 10-20s."*

### 2.B Docs de autoridad — las "8 fases" del modelo de AUTORIDAD/ownership

Esta es la numeración a la que se refiere el "6/8" del README. Estado según
`AUTHORITY-IMPLEMENTATION-COMPLETE.md` (el más reciente de los tres):

| Fase | Nombre | Estado | ¿Afecta al combate/IA del host solo? |
|------|--------|--------|--------------------------------------|
| 1 | Authority Data Model | ✅ COMPLETE | Indirecto (define isRemote/owner) |
| 2 | Client Inbound Validation | ✅ COMPLETE | No (valida paquetes entrantes) |
| 3 | Pending Spawn Queue | ✅ COMPLETE | No |
| 4 | Network Thread Safety | ✅ VERIFIED (ya existía) | No |
| 5 | Server Authority Enforcement | ✅ COMPLETE | No (es server-side anti-cheat) |
| 6 | Protocol Updates for Generation | ✅ COMPLETE | No |
| **7** | **Client Prediction & Reconciliation** | ❌ **NOT IMPLEMENTED** | **Ver abajo** |
| **8** | **Authority Stats & Logging** | ❌ **NOT IMPLEMENTED** | No (solo observabilidad) |

> ⚠️ Inconsistencia entre docs: `audit-2026-06-04` dice que Phase 5 y Phase 6 NO estaban
> implementadas y que Phase 3 NO llamaba a `FlushForEntity`. El doc `COMPLETE` (posterior)
> dice que TODO eso se arregló. Tomar `COMPLETE` como el estado más reciente, pero
> verificar en código antes de fiarse.

### 2.C Las 2 fases NO implementadas — ¿afectan al host con 1 jugador?

**Phase 7 — Client Prediction & Reconciliation (NO implementada):**
- Qué falta: clase `ClientPrediction`, cola `PendingInput` con seq numbers, `CaptureInput()`,
  `PredictOwnedEntity()` (aplicar input ya, sin interpolar), `Reconcile()` al recibir snapshot.
- La decisión `ReconcileLocal` EXISTE en el validator pero **es un stub: solo incrementa un
  contador, no hace nada** (`authority-progress`:19, `audit`:39-40).
- Impacto declarado: *"Local player currently uses interpolation → input lag"*, rubber-banding.
- **VEREDICTO sobre el bug del host:** Phase 7 afecta a la SUAVIDAD del MOVIMIENTO del jugador
  local (lag de input, rubber-band), NO bloquea combate ni levantarse de KO. Es de
  responsividad, no de gating de IA. **No es la causa raíz del "no pelea / no se levanta".**
  PERO: si el char local se trata como `RemoteOwned`/interpolado en vez de `LocalOwned`/predicho,
  ese mal-etiquetado es el MISMO problema que dispara el gating de IA (ver sección 3).

**Phase 8 — Authority Stats & Logging (NO implementada):**
- Solo observabilidad (contadores, HUD F1). Irrelevante para el bug funcional.

---

## 3. ⭐ POR QUÉ EL COMBATE/IA DEL HOST NO FUNCIONA — análisis de causa raíz

### 3.1 El mecanismo de supresión de IA (verificado en código)

`ai_hooks.cpp` mantiene un set global:

```cpp
static std::unordered_set<void*> s_remoteControlled;   // chars con IA sobreescrita por red
```

- `Hook_AICreate` SIEMPRE llama al original (el AI controller queda VÁLIDO, nunca nullptr —
  devolver nullptr fue causa de crashes antes). PERO si `info->isRemote == true`, llama a
  `MarkRemoteControlled(character)` → mete el char en `s_remoteControlled`.
- Los chars en `s_remoteControlled` tienen sus **DECISIONES de IA sobreescritas por input de
  red** (movimiento/tareas bloqueados, conducidos por la red). Comentario literal del código:
  *"Characters in this set have their AI decisions overridden by network input."*

### 3.2 La cadena del bug (hipótesis fuerte, alineada con la sospecha de Zero)

```
Si el char del host ('Dani') tiene EntityInfo.isRemote = true (ERRÓNEO)
   → Hook_AICreate lo mete en s_remoteControlled
   → sus decisiones de IA se sobreescriben con "input de red"
   → pero el host está SOLO: no llega input de red para él
   → resultado: IA congelada → NO pelea, NO se levanta de cama, NO se recupera de KO
   → el char queda "vivo y con AI tick" (el controller existe) pero sin decisiones propias
```

Esto encaja EXACTAMENTE con el síntoma: *"vivo, en simulación, recibe AI tick, pero NO pelea,
NO se levanta, NO se recupera de KO"*. El AI tick corre (controller válido) pero las decisiones
están secuestradas por un canal de red vacío.

### 3.3 Herramienta de diagnóstico que YA EXISTE en el código

```cpp
// ai_hooks.cpp:49 — RemoteControlledCount()
// "si el host está vivo y solo (sin peers) este valor debería ser 0;
//  >0 con el host marcado = anomalía que bloquearía su combate/IA."
size_t RemoteControlledCount();
```

**ACCIÓN INMEDIATA RECOMENDADA:** con el host solo, comprobar `RemoteControlledCount()`
(buscar marcador `[DIAG-REMOTE]` en logs). Si es > 0, el host está erróneamente marcado como
remoto → confirma la causa raíz. Hay que rastrear DÓNDE se pone `isRemote = true` en el
`EntityInfo` del char del host (probable culpable: registro del char propio como remoto, o
remap local→server que no distingue el char del host, o `RegisterRemote` aplicado de más).

### 3.4 Por qué los docs de autoridad NO cubren bien esto

Los docs de autoridad se centran en seguridad multi-jugador (anti-cheat, echo suppression,
generation anti-ghost). El caso "host solo, 1 jugador" NO está cubierto como escenario de test.
El gating de IA (`isRemote → s_remoteControlled`) es un sistema SEPARADO de la enum
`AuthorityType`/`LocalAuthorityState`; el bug vive en el `EntityRegistry` (quién marca isRemote),
no en el validator de autoridad.

---

## 4. DATOS TÉCNICOS ÚTILES (offsets, funciones, archivos)

### Archivos clave del gating IA / autoridad
- `KenshiMP.Core/hooks/ai_hooks.cpp` — supresión IA por `isRemote`. **EPICENTRO del bug.**
- `KenshiMP.Common/include/kmp/types.h:202-217` — enums `AuthorityType` / `LocalAuthorityState`.
- `KenshiMP.Core/sync/entity_registry.{h,cpp}` — `EntityInfo.isRemote`, `GetNetId`, `GetInfo`,
  `RegisterRemote`, `RemapEntityId`, helpers `IsLocalOwned/IsRemote/GetOwnerPlayerId`.
- `KenshiMP.Core/sync/authority_validator.{h,cpp}` — validator cliente (8 decisiones).
- `KenshiMP.Server/authority_validator.{h,cpp}` — `CanClientCommandEntity` (server anti-cheat).
- `KenshiMP.Core/hooks/combat_hooks.cpp` — Phase 5 combat sync (ApplyDamage hook).
- `KenshiMP.Core/game/player_controller.cpp` — `OnRemoteCharacterSpawned`, faction fix, rename.

### Funciones de IA hookeadas (game)
- `AICreate` (`AICreateFn = void*(__fastcall*)(void* character, void* faction)`)
- `AIPackages` (`AIPackagesFn = void(__fastcall*)(void* character, void* aiPackage)`)
- Ambas SIEMPRE llaman al original (controller válido, anti-crash con SEH `__try/__except`).

### ⚠️ DOS árboles de código paralelos (posible confusión)
Existe `KenshiMP.Core/` Y `rebuild/src/` con los MISMOS hooks (`ai_hooks.cpp`, `combat_hooks.cpp`,
`entity_hooks.cpp`, etc.). Hay un "clean rebuild" en marcha
(`docs/superpowers/specs/2026-06-03-kenshi-online-clean-rebuild-design.md`). **Verificar cuál
árbol compila el binario en uso** antes de editar, o se toca el lado equivocado.

### SnapshotDecision (8 outcomes — validator cliente)
`ApplyRemote, ReconcileLocal (stub), QueuePendingSpawn, RejectAuthorityViolation, RejectEcho
(deprecado), RejectStaleGeneration, RejectDestroyed, RejectUnknown`.

### Known issues / TODOs heredados de los docs
- Generation nunca se incrementa en server (IDs no se reusan en práctica → impacto bajo).
- `NetEntityId` definido pero se usa `uint32_t` crudo en muchos sitios.
- `ReconcileLocal` es un stub (no reconcilia, solo cuenta).
- Sin tests unitarios ni de integración para nada de autoridad.
- Phase 7 (prediction) y Phase 8 (stats) sin implementar.
- Docs de autoridad desactualizados respecto a la enum real (ver sección 1.1).

---

## 5. RESUMEN EJECUTIVO PARA EL BUG DEL HOST

La causa raíz NO está en las 2 fases sin implementar (Phase 7 prediction es de responsividad,
no de gating). Está en el **flag `EntityInfo.isRemote` del char del host**: si por error vale
`true`, `Hook_AICreate` lo mete en `s_remoteControlled` y le sobreescribe la IA con un canal de
red vacío → IA congelada (vivo, con tick, sin decisiones). Verificar con `RemoteControlledCount()`
y rastrear quién pone `isRemote=true` en el registro para el char propio del host.
