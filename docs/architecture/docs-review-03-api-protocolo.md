# docs-review-03 — API + Protocolo + Specs: ownership del char del HOST

> **Fecha:** 2026-06-18 · **Ámbito:** revisión a fondo de `docs/API.md`, `docs/PROTOCOL.md`,
> los 2 specs (clean-rebuild + remote-player-reliability) y el plan E2E, cruzada con el código
> real de `rebuild/src/`. Objetivo: diagnosticar por qué el char del host ('Dani') vive, recibe
> tick, pero NO pelea/levanta/recupera (combate+IA congelados).
> **Veredicto corto:** la causa NO es ownership/authority de red (el host es owner=1, registrado
> como local correctamente). La causa candidata firme es **char equivocado** (audit-09): el mod
> parchea `data[0]` del lektor en vez del `controlledChar` real `PI+0x2A8`.

---

## 1. EntityRegistry y ownership — cómo se registra el char del HOST

### 1.1 `Register()` (local) — `rebuild/src/sync/entity_registry.cpp:7`
Cuando se registra un game object LOCAL, SIEMPRE queda marcado como propio:
```cpp
info.ownerPlayerId = owner;            // = m_localPlayerId del host
info.isRemote      = false;
info.state         = EntityState::Active;
info.authority     = AuthorityType::Player;          // Player-owned local
info.localState    = LocalAuthorityState::LocalOwned; // ← clave: LocalOwned
```
No hay forma de que `Register()` deje un char local como remoto: `isRemote=false` y
`localState=LocalOwned` están hardcodeados. El único riesgo sería owner=0, pero ver §1.3.

### 1.2 `RegisterRemote()` — `entity_registry.cpp:41`
La ruta remota es la única que pone `isRemote=true`, `state=Spawning`,
`localState=RemoteOwned`. Solo se invoca al recibir `S2C_EntitySpawn` de OTRO jugador. El char
del host NO pasa por aquí.

### 1.3 ¿Owner del host = 0 (riesgo "server-owned")? — DESCARTADO
- El char del host se registra en `Core::FindAndClaimModCharacters()` (`core.cpp:2026`) y en
  `SendExistingEntitiesToServer()` con `owner = m_localPlayerId`.
- `m_localPlayerId` se fija en `HandleHandshakeAck` → `SetLocalPlayerId(msg.playerId)`
  (`packet_handler.cpp:390`). El host también hace handshake contra su propio server.
- El server asigna IDs con `m_nextPlayerId = 1` (`server.h:150`, `NextPlayerId()` post-incrementa).
  **El host es player ID = 1**, no 0. `m_hostPlayerId` se setea al primer que entra (`server.cpp:633`).
- Por tanto el char del host queda **owner=1, LocalOwned, isRemote=false**. NO colisiona con la
  convención "owner==0 = server-owned" de `ownership.cpp:32-36`.

> ⚠️ Matiz de timing: si `FindAndClaimModCharacters()` corriera ANTES del HandshakeAck,
> `m_localPlayerId` valdría 0 y el char se registraría con owner=0 → lo trataría `OwnershipManager`
> como server-owned. Verificar en logs que el claim ocurre DESPUÉS del ack (en el flujo normal
> sí: el claim se dispara desde OnGameLoaded/reintentos `core.cpp:2231-2293`, posteriores al
> handshake). Es el único hueco por el que la sospecha de Zero podría materializarse — comprobar
> orden real en runtime. Aun así no explicaría por sí solo el combate congelado (la IA del char
> propio no depende del registry del mod).

### 1.4 Helpers de autoridad (`IsLocalOwned`, `IsRemoteOwned`, `IsServerOwned`)
- `IsLocalOwned(netId, myId)` = `owner==myId && localState==LocalOwned`.
- `UpdateOwner()` solo cambia `ownerPlayerId`, NO toca `localState` ni `isRemote` → si alguna vez
  se reasignara owner del host, quedaría incoherente (owner nuevo pero localState viejo). No es el
  caso aquí pero es deuda latente.
- **Conclusión:** a nivel de EntityRegistry el char del host está bien clasificado. El registry
  del mod es un mapa net-id↔game-object para SYNC; no gobierna la IA/combate del motor de Kenshi.
  Que esté "bien" o "mal" en el registry NO congela el combate por sí mismo. Esto reorienta el
  diagnóstico hacia el char equivocado (§4).

---

## 2. Protocolo — spawn / host-assignment / faction / ownership

Mensajes relevantes al problema (de `PROTOCOL.md` + `API.md` §7):

| Mensaje | Valor | Qué lleva | Relevancia al bug |
|---|---|---|---|
| `S2C_HandshakeAck` | 0x02 | `playerId` asignado, tick, time | Fija `m_localPlayerId` del host (=1) |
| `S2C_HostAssignment` | 0x92 | `newHostPlayerId` | Identidad de host; NO controla autoría del char |
| `S2C_EntitySpawn` | 0x20 | `entityId, generation, type, ownerId (0=server NPC), templateId, pos, quat, factionId, name, equip` | Solo para chars REMOTOS. El del host nunca se spawnea por aquí |
| `C2S_EntitySpawnReq` | 0x22 | El host envía SU char (netId, type, owner=localId, faction) | El host se anuncia a sí mismo al server |
| `S2C_FactionAssignment` | 0xF0 | string de facción por slot | Lobby: asigna identidad de facción pre-load |

- **Owner en el wire:** `MsgEntitySpawn.ownerId` define propiedad. `ownerId=0` ⇒ NPC server-owned.
  Para el char del host el owner viaja como `m_localPlayerId` (=1) en `C2S_EntitySpawnReq`
  (`core.cpp:2044`). Coherente.
- **Generation counter:** anti-ghost-control. No afecta al char propio del host (no se le aplican
  posiciones remotas).
- **Server-authoritative combat (PROTOCOL §5.4 y §9.4):** combate, KO, muerte se resuelven en el
  server (`C2S_AttackIntent` → `S2C_CombatHit/KO/Death`). PERO el spec de clean-rebuild marca el
  server `ResolveCombat`/`HandleAttackIntent` como **código MUERTO/inalcanzable** (ver §3). Es
  decir: hoy NADIE resuelve el combate autoritativamente. Esto es coherente con "el char no
  pelea": si el flujo de ataque del char propio se enruta a un resolver server que no existe/está
  desconectado, o si `WritePlayerControlled` deja al char en estado intermedio, el combate no
  progresa.

---

## 3. Specs clean-rebuild y remote-player-reliability — arquitectura prevista

### 3.1 Clean-rebuild (`2026-06-03`)
- **Raíz documentada del bloqueo histórico:** `OnGameLoaded()` no disparaba porque
  `GameWorldSingleton` resolvía UNRESOLVED → `PacketRouter` tiraba todos los `S2C_EntitySpawn` en
  un `// TODO`. Fix = LoadGate por polling del iterador + buffer diferido. (Ya tratado en el
  rebuild; no es el bug actual del host, que SÍ está vivo y en sim.)
- **Combate es secundario** en el MVP. El server `combat_resolver`/`HandleAttackIntent` está en la
  lista de **DELETE** ("unreachable") → confirma que la resolución autoritativa de combate NO
  está operativa en la rama actual. Cualquier expectativa de que el char propio pelee "porque el
  server lo resuelve" es falsa hoy.
- **Una sola autoridad de sync:** SyncPipeline (un read→send + un write por entidad/tick). El char
  del host es leído y enviado, no sobre-escrito (es local). Recibir tick ✓ no implica IA activa.

### 3.2 Remote-player-reliability (`2026-04-19`) — el más cargado de pistas
Aunque es sobre chars REMOTOS, contiene EL mecanismo exacto que congela un char:

- **Tres rutas de spawn (A mod-link / B direct-factory / C fallback)** con post-spawn divergente.
- **El punto clave (tabla del spec, fila Path C):** los pasos `WritePlayerControlled(true)` y
  `AddCharacterToLocalSquad` estaban **DESHABILITADOS** en Path C (`core.cpp:2737-2738` y
  `2733-2735` del árbol legacy).
  > *"The disabled steps in Path C are the root reason remote characters appear but behave wrong:
  > **AI keeps driving them (no WritePlayerControlled), they never join the squad panel.**"*
- **Traducción al síntoma del host:** un char con `playerControlled=false` queda **conducido por
  la IA** y no responde a comandos del jugador. Si el char de 'Dani' tiene el flag
  `playerControlled` mal puesto (o se le aplicó a OTRO char — ver §4), el motor lo trata como NPC
  pasivo: vivo, en sim, recibe tick, pero **no pelea / no se levanta / no recupera** porque su IA
  de jugador no está activa sobre él. Esto encaja al 100% con la descripción de Zero.
- `EntitySnapshot.playerControlled` (API.md §6) existe como campo leído por el SDK → hay forma de
  comprobarlo en runtime sobre el char correcto.

---

## 4. Causa raíz candidata (cruce con audit-09) — CHAR EQUIVOCADO

`audit-09-primary-char.md` (RE confirmado por bytes) demuestra que Kenshi guarda el char
controlado en un campo SEPARADO del que lee el mod:

| Quién | Resuelve "char del jugador" como | Offset |
|---|---|---|
| **El mod (hoy)** | `data[0]` del lektor `playerCharacters` | `PI+0x2B0 → +0x2C0[0]` |
| **El motor (real)** | `controlledChar` (sujeto de las acciones) | **`PI+0x2A8`** |

- `PlayerController::GetPrimaryCharacter()` (`player_controller.cpp:46`) devuelve el **primer
  game object del EntityRegistry del mod**, no `PI+0x2A8`.
- `GetPlayerPrimaryCharacterDirect()` lee `data[0]` del lektor (orden de inserción, sin concepto
  de "principal").
- **Consecuencia:** los fixes del mod (faction `+0x10`, `playerControlled`, seed `+0xD0`) se
  aplican a un char que **el jugador NO controla**, y el char real (`PI+0x2A8`) nunca se toca →
  combate/IA del char real bloqueados. Divergen cuando hay escuadra o cuando un NPC contratado
  entra antes en el lektor del mod (con 1 solo char, coinciden y "parece" ir bien).
- **El `/verify` confirma el sesgo del diagnóstico:** `verify` (`builtin_commands.cpp:787`) corre
  sobre `GetPrimaryCharacter()` = `data[0]`. El FAIL que ve Zero (faction / writePos chain) está
  **midiendo el char equivocado**, no necesariamente un offset malo.

**Fix propuesto en audit-09 (NO aplicado, pendiente de confirmar con `[DIAG-PRIMARY]`):**
anteponer `PI+0x2A8` a `data[0]` en la resolución del char primario, con fallback
`Faction(PI+0x2A0)+0x218[memberCount-1]` → `data[0]`.

---

## 5. Comandos de debug útiles (rebuild, `sys/builtin_commands.cpp`)

| Comando | Línea | Qué hace / utilidad para este bug |
|---|---|---|
| `/verify` | 787 | Vuelca PASS/FAIL de offsets (position, rotation, faction, name, health chain, writePos/AnimClass chain) **sobre `GetPrimaryCharacter()`**. ⚠️ Opera sobre `data[0]` → si el char es el equivocado, los FAIL son engañosos. Cruzar con `[DIAG-PRIMARY]`. |
| `/forcespawn` | 1529 | Force-spawn de chars remotos pendientes, saltándose los gates. Útil para descartar el deadlock de LoadGate (no es el bug del host). |
| (claim) | 1789 | `core.FindAndClaimModCharacters()` + cuenta de entidades locales. Re-ejecuta el claim del char del host. |
| (resend) | 190, 239 | `core.SendExistingEntitiesToServer()` — reenvía el char local al server. |
| `[DIAG-PRIMARY]` | (en `core.cpp OnGameTick`, throttled) | Loguea `MOD-usa` vs `data[0]` vs `ctrlChar(PI+0x2A8)` vs `facLast`. **Es la prueba directa** de si el mod agarra el char correcto. Veredicto `data[0] != ctrlChar` ⇒ CHAR EQUIVOCADO. |

---

## 6. Plan E2E (`2026-03-13`) — pistas adicionales

- Documenta la cadena de escritura de posición remota vía `AnimClass(+0xC0)→CharMovement→pos`
  (`+0x320+0x20`). Es la ruta que el `/verify` valida como "writePos chain".
- Su Lobby/FactionAssignment (parchear `.rdata` faction pre-load) es lo que hoy produce facciones
  por slot. Si el char real (`PI+0x2A8`) no es el parcheado, su facción/relaciones quedan sin
  arreglar → puede contribuir a "enemigos huyen" / comportamiento no hostil correcto.
- Reafirma combate server-authoritative como diseño, pero el clean-rebuild lo marca como código
  muerto hoy → **no esperar resolución de combate del server en la rama actual.**

---

## 7. Síntesis para el problema (combate/IA del host congelados)

1. **Ownership/authority de red está bien:** host=owner 1, LocalOwned, isRemote=false. Descarta la
   sospecha "registrado como remoto / owner=0". (Único hueco: que el claim corra antes del ack →
   verificar en logs; aun así no explica el combate.)
2. **El registry del mod no gobierna la IA del motor.** Que el char esté en el registry y reciba
   tick de sync NO activa su IA de jugador.
3. **Causa raíz firme (audit-09): char equivocado.** El mod parchea `data[0]`, el motor controla
   `PI+0x2A8`. Si divergen, el char real nunca recibe los fixes (incl. `playerControlled`) → la IA
   lo conduce como NPC pasivo: vivo, en sim, sin pelear/levantarse.
4. **Mecanismo exacto del "no pelea" está documentado** en remote-player-reliability: char con
   `playerControlled=false` ⇒ AI-driven, no responde a comandos. Mismo síntoma.
5. **El `/verify` engaña** porque mide el char equivocado — los FAIL de faction/writePos no prueban
   offsets malos.
6. **No esperar combate del server:** `combat_resolver`/`HandleAttackIntent` están marcados
   DELETE/unreachable en el clean-rebuild.
