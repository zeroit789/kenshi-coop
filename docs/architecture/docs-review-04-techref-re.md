# Revisión docs 04 — Technical Reference + Reverse Engineering (foco: IA / control de chars)

**Fecha:** 2026-06-18
**Objetivo:** Extraer todo offset/flag de IA, control local/remoto, faction, squad/platoon útil
para el problema: *el char del host está vivo, recibe el AI tick, entra a la rama viva, pero NO
actúa*. Hipótesis: su AI package (AITaskSytem en char+0x20) es NULL, o el char está marcado como
remoto.

**Fuentes leídas:**
- `docs/Kenshi-Online-Technical-Reference.md`
- `docs/REVERSE_ENGINEERING.md`
- `docs/reverse-engineering/character-class-analysis.md`
- `docs/reverse-engineering/class-analysis-building-inventory-faction-squad.md`
- `docs/TESTING.md`
- `docs/superpowers/specs/2026-04-19-host-authoritative-game-speed-design.md`
- (verificación en código) `KenshiMP.Core/game/game_types.h`, `KenshiMP.Core/hooks/ai_hooks.cpp`

> ⚠ Importante: el código en `game_types.h` y `ai_hooks.cpp` tiene hallazgos RE de **2026-06-18**
> MÁS NUEVOS que los .md de docs/. Donde hay conflicto, manda el código. Lo marco abajo.

---

## 1. La clase Character — offsets relevantes para IA / control

### El "AI package" = AITaskSytem en char+0x20  ⭐ CLAVE
- **`char+0x20` = `AITaskSytem*`** (RTTI `.?AVAITaskSytem@@`, vtable RVA `0x16E3F30`).
  CONFIRMADO por RE de bytes 2026-06-18 sobre **AI::create (0x622110)**:
  - `AI::create` crea un objeto de **0x3B8 bytes** y lo escribe con `mov [rbx+0x20], rax` en **`0x6221AF`**.
  - La cola de tareas es un `lektor<Tasker*>` **inline en AITaskSytem+0x2E8**:
    `size@+0x2F0 (uint32)`, `cap@+0x2F4`, `data@+0x2F8 (Tasker**)`.
  - **Un char con `char+0x20 == NULL` NO tiene IA y queda INERTE**: el "think" pesado
    `[char_vtbl+0x1D8] → 0x5CE020` no tiene manager que procesar.
  - Diagnóstico activo en código: `[DIAG-AITASK]` en core.cpp.
- ⚠ Los docs (`character-class-analysis.md`) decían `aiPackage = -1` (no encontrado). **Obsoleto.**
  El valor real verificado es **0x20**.

> **Esto es exactamente la hipótesis del problema.** Si el char del host entra al tick vivo pero
> no actúa, lo primero a comprobar es `char+0x20`: si es NULL no hay AITaskSytem y no actúa;
> si no es NULL, mirar si su cola de tareas (`+0x2E8`/`+0x2F0`) está vacía o congelada.

### isPlayerControlled — NO EXISTE como campo  ⭐ CLAVE
- **No hay flag booleano `isPlayerControlled` en Character** (`= -1`, marcado `-2 = N/A`).
  RE 2026-06-17 (KenshiLib Character.h + binario 1.0.68): el campo no existe.
- La distinción player/NPC se **deriva por facción**:
  `char.faction(+0x10) == gameWorld.player(+0x580).faction` → es `Character::isPlayerCharacter()`.
- El probe diferencial quedó **neutralizado** en `game_offset_prober.cpp`.

> Implica: no se puede "marcar char como local/remoto" tocando un flag en el Character. El control
> remoto se gestiona en el mod por un `std::unordered_set<void*> s_remoteControlled` (ver §3), NO
> por memoria del juego.

### Otros offsets de Character (verificados 1.0.68)
| Offset | Campo | Estado |
|--------|-------|--------|
| `+0x00` | vtable | (`[vtbl+0x1D8]` = think pesado → 0x5CE020) |
| `+0x10` | `Faction*` faction | ✅ verificado (clave para player vs NPC) |
| `+0x18` | name (std::string SSO) | ✅ |
| `+0x20` | **`AITaskSytem*` (AI package)** | ✅ **2026-06-18** |
| `+0x40` | `GameData*` template | ✅ |
| `+0x48` | Vec3 position (cached, read-only) | ✅ |
| `+0x58` | Quat rotation | ✅ |
| `+0x2E8` | `Inventory*` | ✅ |
| `+0x450` | Stats base | ✅ |
| `currentTask` | — | `-1` (no encontrado) |
| `isAlive` | — | `-1` (usar cadena de salud) |
| `moveSpeed`, `animState`, `sceneNode`, `squad` | — | `-1` (no encontrados) |

### Salud — cadena CE NO casa con 1.0.68 (audit-02)
- La cadena histórica `+0x2B8 → +0x5F8 → +0x40` da **FAIL en /verify**.
- En KenshiLib 1.0.68: `+0x2B8` = `CharacterMemory* _myMemory` (NO salud). Por eso /verify lee un float arbitrario.
- Ruta CORRECTA pendiente de verificar: `Character+0x458` (MedicalSystem inline) →
  `leftLeg/rightLeg/leftArm/rightArm (+0x80..+0x98)` → `HealthPartStatus` → `flesh@+0x40`.
- Cada `HealthPartStatus` es objeto de **0x68 bytes** (`flesh@+0x40`, `fleshStun@+0x44`) → `healthStride=8` NO aplica.

### Posición escribible (mueve de verdad al char)
`char + animClassOffset` (probe) → `AnimClass+0xC0` (CharMovement*) → `+0x320` → `+0x20` (x float).

---

## 2. Faction / Squad / Platoon / PlayerInterface — relación char↔control

### PlayerInterface (apuntado por GameWorld+0x580)  ⭐ CLAVE
RE 2026-06-18 (audit-09), bytes de **SetControlledChar (RVA 0x802520)** + runtime:
| Offset | Campo | Nota |
|--------|-------|------|
| `+0x2A0` | `participant` (Faction*) | facción del jugador |
| **`+0x2A8`** | **`controlledChar` (Character*)** | **el char que el jugador controla DE VERDAD** |
| `+0x2B0` | `playerCharacters` (`lektor<Character*>`) | lista de personajes del jugador |

- **`controlledChar` (+0x2A8) ≠ `playerCharacters` data[0]`.** El motor aplica las órdenes sobre
  `+0x2A8`. `SetControlledChar` lo calcula como `Faction(+0x2A0)+0x218[memberCount-1]` (último
  miembro) y lo escribe; se consume en `0x50E9CF` (`mov rcx,[rcx+0x2A8]`).
- **El mod DEBE resolver el char primario del host por `+0x2A8`, NO por `data[0]`** (que daba
  'Dani' en vez de 'Sinnombre_0'). Esto ya estaba ligado al RE de facción resuelto en sesión 06-17.

### Faction — el "id" plano NO existe (audit-02)  ⭐ CLAVE bug facciones
- `FactionOffsets.id` ahora es **`-1` (NO RESUELTO)**. El histórico `+0x08` era `bool _antiSlavery`,
  NO un id. Enviar ese bool como `factionId` de red producía ids basura (0/1) que colisionan →
  matching cliente/servidor empareja facciones equivocadas. **Ligado directamente al bug conocido
  de facciones (enemigos huyen)**: `C2S_FactionRelation` no podía resolver punteros correctos.
- Identificador estable de facción = `name` (std::string @**+0x1A8**, ✅) o `GameData*` (@+0x240).
- `Faction+0x250` = `isPlayer` (PlayerInterface*) — KenshiLib Faction.h:158.
- **Lista REAL de miembros de la facción** (de donde el motor saca `controlledChar`):
  `memberCountReal @+0x210 (uint32)`, `memberArrayReal @+0x218 (Character**)`. ✅ confirmado RE.
  Los antiguos `members@+0x30 / memberCount@+0x38` son **dudosos** (chocan con KenshiLib).

### Squad (offsets, UNVERIFIED salvo nota)
`+0x10 name`, `+0x28 memberList`, `+0x30 memberCount`, `+0x38 faction`, `+0x40 isPlayerSquad`.
- `SquadCreate (0x480B50)` **DISABLED** (crash por prólogo `48 8B C4`).
- `SquadAddMember (0x928423)` activo pero mid-function, no 16-byte aligned (hook frágil).
- char→squad backpointer solo por heurística (frágil). El string de squad está hardcodeado "Squad".

---

## 3. Lo que el mod YA hace con IA / control remoto (ai_hooks.cpp)

- **AI::create (0x622110)** `void*(char, faction)` — HOOKED. El hook **SIEMPRE llama al original**
  (cada char necesita un AI controller válido). Devolver NULL fue causa raíz de crashes (combate,
  pathfinding, transiciones de animación, selección UI deref el controller sin null-check).
- **AIPackages (0x271620)** `void(char, aiPackage)` — HOOKED. SIEMPRE deja cargar los paquetes
  (behavior trees deben existir o crashea). Solo log para remotos, **sin supresión**.
- **Control remoto NO toca memoria del juego**: se lleva en `std::unordered_set<void*>
  s_remoteControlled` + `MarkRemoteControlled/UnmarkRemoteControlled/IsRemoteControlled`.
  La filosofía documentada: "AI controller se mantiene VÁLIDO, solo se overridean las DECISIONES".
- **Diagnóstico `RemoteControlledCount()`** (solo lectura): *"si el host está vivo y solo (sin
  peers) este valor debería ser 0; >0 con el host marcado = anomalía que bloquearía su
  combate/IA"*. → **Comprobar esto directamente para el problema actual.**

> El char se marca remoto en `Hook_AICreate` SOLO si `registry.GetInfo(netId)->isRemote == true`.
> Si el host quedara marcado remoto por error (netId mal mapeado / isRemote mal puesto), su IA
> quedaría overrideada y NO actuaría — encaja con el síntoma.

---

## 4. Funciones RE relevantes (RVAs verificados 1.0.68 Steam)

| Función | RVA | Firma | Relevancia IA/combate |
|---------|-----|-------|----------------------|
| **AI::create** | `0x622110` | `void*(char, faction)` | crea AITaskSytem en char+0x20 |
| **AIPackages** | `0x271620` | `void(char, aiPackage)` | carga behavior trees |
| **SetControlledChar** | `0x802520` | — | escribe PlayerInterface+0x2A8 |
| (consumo controlledChar) | `0x50E9CF` | `mov rcx,[rcx+0x2A8]` | el motor usa el char activo |
| char think pesado | `0x5CE020` | vía `[char_vtbl+0x1D8]` | el tick de IA del char |
| CharacterSpawn | `0x581770` | `void*(factory, req)` | MovRaxRsp |
| ApplyDamage | `0x7A33A0` | `void(target,att,bodyPart,cut,blunt,pierce)` | MovRaxRsp; crashea +0x178 |
| StartAttack | `0x7B2A20` | — | MovRaxRsp |
| CharacterDeath | `0x7A6200` | `void(char, killer)` | MovRaxRsp |
| CharacterKO | `0x345C10` | `void(char, attacker, reason)` | |
| FactionRelation | `0x872E00` | — | ligado a bug facciones |

String anchors útiles: `[AI::create] No faction for` (27), `AI packages` (11), `timeScale` (9).

---

## 5. TESTING — cómo se prueba

- **Integration suite** (15 tests): `KenshiMP.IntegrationTest.exe` (auto-localiza el server).
  Cubre handshake, spawn+broadcast, position sync, chat, disconnect, time sync, multi-entity,
  inventory, trade, squad, faction relation, building, server browser, full E2E.
- **TestClient bot**: `KenshiMP.TestClient.exe [ip] [port] [name]`. Camina patrón, manda pos 20Hz.
  Comandos: `c <msg>`, `s` (status), `h`, `q`.
- **LiveTest**: lanza Kenshi real + server + bot, monitoriza 20 hitos.
- **Posiciones de test (cerca de The Hub)**: Hub Center `-51200, 1600, 2700`.
- Plantillas válidas: `Greenlander`, `Scorchlander`, `Skeleton`, `Shek`, `Hive Worker`.
- **Issue ABIERTO relevante**: combate no sincroniza barras de daño → ApplyDamage hook crashea
  por NULL pointer en **+0x178** (Combat stats base) → hook desactivado, server manda HealthUpdate.
- Logs: `Kenshi/KenshiOnline_<PID>.log` (cliente), `KenshiOnline_Server.log`, `KenshiOnline_CRASH.log`.

---

## 6. Spec game-speed (contexto sobre congelación de simulación)

- `gameSpeed` se escribe en **GameWorld+0x700** y **TimeManager+0x10** vía `time_hooks::SetServerTime`.
- `GameWorld+0x08B9` = bool **paused**. `GameWorld+0x08B0` = ZoneManager.
- Si el reloj avanza pero los personajes no se mueven (síntoma de Fase 4 / sync), conviene
  descartar que el problema sea gameSpeed/paused — esos están en GameWorld, NO en el char.
  La congelación del char apunta más a AITaskSytem (char+0x20) o a la cola de tareas vacía.
