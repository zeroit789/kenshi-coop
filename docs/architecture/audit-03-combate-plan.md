# Auditoría 03 — Sistema de Combate + Plan para completarlo (host)

> Auditoría READ-ONLY. No modifica código. Fecha: 2026-06-18.
> Binario objetivo: `kenshi_x64.exe` v1.0.68 Steam (App 233860), MSVC x64, ImageBase 0x140000000.
> Alcance: combate del HOST (single-player modeado). Las órdenes del jugador no se ejecutan,
> los NPCs huyen. La facción del char ya se arregló (`char+0x10`). Bloqueo activo: "pausa fantasma" de UI/input.
> Fuentes cruzadas: `02-hooks.md`, `04-sync.md`, `00-INDEX.md`, notas RE en `docs/reverse-engineering/`.

---

## 0. TL;DR — qué está roto y en qué orden arreglarlo

El combate del host falla por una **cadena de tres capas**, no por una sola causa:

1. **Capa de COMANDO (la que rompe todo).** Aunque `paused(+0x8B9)=0` y `gameSpeed(+0x700)=1.0`
   (la simulación corre), un estado de pausa de UI/input separado bloquea que el clic del jugador
   se convierta en orden. Sin orden no hay ataque. **Es el bloqueo #1.**
2. **Capa de DIAGNÓSTICO rota.** El hook "StartAttack" NO está en la función de iniciar ataque:
   está en `0x7B2A20` = "Cut/blunt damage **calculation**" (string "Cutting damage"). Por eso el
   diagnóstico nunca confirma si la orden llega. **Hay que arreglar el RVA antes de poder diagnosticar.**
3. **Capa de DAÑO desactivada.** `ApplyDamage` (`0x7A33A0`) está NO-INSTALADO por crash del wrapper
   MovRaxRsp (slots globales, no TLS). No bloquea el combate del host (el motor aplica daño solo),
   pero sí impide rehabilitarlo con seguridad y bloquea el sync de daño por intención.

**Orden de prioridad recomendado:** P0 pausa fantasma de UI → P1 RVA de StartAttack (diagnóstico) →
P2 NPCs huyen → P3 MovRaxRsp a TLS → P4 (opcional) rehabilitar ApplyDamage.

> Hallazgo transversal: **la mitad de los síntomas son de RE, no de lógica de red.** El RVA equivocado
> de StartAttack y el offset/estado de pausa explican casi todo lo observado.

---

## 1. Arreglar el RVA de StartAttack (para que el diagnóstico funcione)

### Diagnóstico del problema
- **`combat_hooks.cpp:405`** comentario dice `StartAttack (0x7A1650)`.
- **`orchestrator.cpp:169-171`** registra `StartAttack` con fallback RVA `0x007B2A20`,
  string ancla `"Cutting damage"` (14 bytes), categoría "combat", descripción **"Cut/blunt damage calculation"**.
- **`patterns.h:88-90`** (`START_ATTACK`) y **`patterns.h:342`** (anchor) → mismo string `"Cutting damage"`, RVA `0x007B2A20`.

**Conclusión:** la función actualmente hookeada como "StartAttack" **NO es** la función que inicia un
ataque. Es la rutina de **cálculo de daño cortante/contundente** (parte interna de la cadena de daño,
hermana de `ApplyDamage`/`CutDamageMod`/`HealthUpdate`). Firma real esperada: NO es
`(attacker, target, weapon)` sino un cálculo tipo `(damageCtx, ...)`. El `(attacker, target, weapon)`
del comentario es una suposición que nunca se verificó. El `0x7A1650` del comentario es residuo de
otra hipótesis (probablemente correcta, ver abajo) que quedó sin enlazar al orchestrator.

Por eso el diagnóstico es inservible: aunque dispare, está midiendo "se calculó daño cut/blunt",
no "el jugador inició un ataque". Y como el host no llega a aplicar daño (la orden se bloquea antes),
es plausible que **nunca dispare**, llevando a la conclusión errónea "la orden no llega al motor".

### Cuál es la función real
No se puede confirmar la RVA exacta de "iniciar ataque" solo con análisis estático de las notas
actuales — **requiere verificación en binario**. Hipótesis ordenadas por probabilidad:

1. **No existe una única "StartAttack" limpia hookeable.** En Kenshi el flujo de ataque va por la
   IA/behavior tree (`AIPackages`, `0x271620`) que decide atacar, y por el solver de combate
   `MartialArtsCombat` (`0x892120`, string "Martial Arts") para cuerpo a cuerpo. El "inicio de ataque"
   es un estado del behavior tree, no una función con prólogo aislado.
2. **`0x7A1650`** (el del comentario): candidato a verificar. Está pegado a `CharacterDeath` (`0x7A6200`)
   y `ApplyDamage` (`0x7A33A0`) en el mismo clúster de combate de `.text`. Verificar su `.pdata`,
   prólogo y xrefs de string antes de confiar.
3. **`MartialArtsCombat` (`0x892120`)** ya resuelto: es el handler de combate completo. Para DIAGNÓSTICO
   de "¿el host entra en combate?" es más fiable que la función inexistente de StartAttack.

### Qué cambiar
- **`KenshiMP.Scanner/include/kmp/patterns.h:90`** y **`:342`**, **`orchestrator.cpp:169-171`**:
  - **Opción A (recomendada, bajo riesgo):** RENOMBRAR el hook actual a lo que realmente es
    (`CutBluntDamageCalc`) y dejar de llamarlo "StartAttack". Re-apuntar el DIAG de combate a
    **`MartialArtsCombat` (`0x892120`)**, que sí confirma entrada en combate cuerpo a cuerpo.
  - **Opción B (si se quiere el StartAttack real):** verificar `0x7A1650` en binario (`.pdata` +
    prólogo + xref). Solo entonces crear un patrón AOB nuevo de ≥15 bytes fijos y registrarlo.
- **`combat_hooks.cpp:20-22, 211-236, 405-421`**: actualizar el comentario y la firma. La firma
  `(void*, void*, void*)` actual es una suposición no verificada para `0x7B2A20`; corregirla a la
  firma real una vez confirmada la función.

### Riesgo
- **Bajo si Opción A.** El hook ya es DIAG (solo log, llama original sin tocar). Cambiar a qué función
  apunta no afecta el combate; solo la calidad del diagnóstico.
- **Medio si Opción B sin verificar:** si `0x7A1650` es la función equivocada y el pattern falla,
  el fallback hookea una función arbitraria → crash potencial (prólogo `mov rax,rsp` aplicado a algo
  que no lo es). NO confiar en el fallback hardcodeado sin confirmar.

### Prioridad: **P1** (necesario para que el diagnóstico sea fiable, pero secundario al P0 de pausa).

---

## 2. Migrar MovRaxRsp de slots globales a TLS

### Diagnóstico del diseño actual
`mov_rax_rsp_fix.cpp` usa una **página por hook** (`AllocMovMovRaxRspPage`, `ALLOC_SIZE=0x200`) con
estos slots **globales a esa página** (compartidos por TODAS las llamadas de ese hook):

| Slot | Offset | Uso |
|---|---|---|
| `OFF_CAPTURED_RSP` | +0x00 | RSP del caller del juego en entrada |
| `OFF_STUB_RSP` | +0x08 | RSP del hook C++ antes del swap |
| `OFF_SAVED_GAME_RET` | +0x10 | Dirección de retorno del juego |
| `OFF_DEPTH` | +0x18 | Contador de reentrancia (int32) |
| `OFF_RAW_TRAMP` | +0x20 | Trampoline crudo MinHook (para bypass reentrante) |
| `OFF_BYPASS` | +0x28 | Flag de bypass software |

**Por qué crashea bajo ApplyDamage (300+/seg):**
1. **Reentrancia → degrada a passthrough crudo.** El guard `OFF_DEPTH` (líneas 305-308, 350-356):
   si `depth>1` salta `EmitJmpMemAbs(OFF_RAW_TRAMP)` → ejecuta el **trampoline crudo** que empieza
   con `mov rax,rsp` SIN el fix de stack-swap. Si la reentrada vuelve a tener el prólogo problemático,
   RAX captura el RSP equivocado → corrupción de `[rbp+XX]`. En combate continuo ApplyDamage se anida
   (daño que dispara más daño/efectos) → crash determinista "attack unprovoked".
2. **NO es thread-safe.** Comentario en `mov-rax-rsp-fix.md:65-67` lo admite: *"Uses per-hook global
   captured_rsp slot (not TLS). Safe because Kenshi game logic is single-threaded. If multi-thread
   needed later, switch to TlsAlloc."* Pero OnGameTick lo dirige el **hilo de render** (Present), no
   un hilo de lógica dedicado (ver `02-hooks.md` §9). Si una función con este prólogo se llamara
   también desde otro hilo (audio, físicas Havok, carga), los slots globales se pisan → crash aleatorio.
3. **Ventana de scan inadecuada para funciones grandes.** ApplyDamage = 6925 bytes; la ventana de
   4KB del scanner no alcanza (`patterns.cpp:207`). Esto es ortogonal al TLS pero suma al riesgo.

### Diseño del cambio a TLS
**Objetivo:** que `CAPTURED_RSP`, `STUB_RSP`, `SAVED_GAME_RET` y `DEPTH` sean **por hilo**, de modo que:
- Cada hilo tenga su propio juego de slots → no se pisan entre hilos.
- La reentrancia REAL (anidamiento en el mismo hilo) se soporte con una **pila de frames por hilo**
  en lugar de degradar a passthrough crudo.

**Esquema concreto:**

1. **Asignar un índice TLS global una vez** (`TlsAlloc()` en la init del HookManager).
   Guardarlo en `mov_rax_rsp_fix` como `static DWORD s_tlsIndex`.

2. **Estructura por hilo (heap, una por hilo, lazy):**
   ```cpp
   // Pila de frames MovRaxRsp por hilo — soporta reentrancia real (anidamiento).
   struct MovRaxRspThreadState {
       static constexpr int MAX_DEPTH = 16;   // 16 niveles de anidamiento por hilo
       struct Frame {
           uintptr_t capturedRsp;   // RSP del caller del juego
           uintptr_t stubRsp;       // RSP del hook C++
           uintptr_t savedGameRet;  // retorno del juego
       } frames[MAX_DEPTH];
       int depth = 0;               // índice de frame actual
   };
   ```

3. **El stub naked ya NO puede ser solo ASM plano** porque leer TLS desde ASM requiere
   `gs:[0x58]` + índice + deref. Dos opciones de implementación:
   - **Opción TLS-ASM (rápida, compleja):** emitir en el naked detour la secuencia
     `mov r10, gs:[0x58]; mov r10,[r10 + tlsIndex*8]; ...` para resolver el puntero del estado del
     hilo, luego indexar `frames[depth]`. Más bytes de stub, pero cero llamadas C++ en el camino
     caliente. Requiere manejar el caso "TLS slot vacío" (primer uso del hilo) — difícil sin llamar
     a C++.
   - **Opción thunk-C (recomendada, más segura):** el naked detour hace `call` a un **helper C
     `__declspec(noinline)`** que: (a) lee/crea el `MovRaxRspThreadState` del hilo vía `TlsGetValue`,
     (b) hace push del frame con el RSP capturado (pasado en registro), (c) devuelve el puntero al
     frame actual. El stub guarda ahí en vez de en la página global. El push/pop del frame se hace
     en C, el swap de RSP sigue en ASM. Coste: 1 call C por entrada de hook — aceptable salvo para
     ApplyDamage (de ahí que ApplyDamage siga necesitando además la decisión de §4/§P4).

4. **Reentrancia:** en vez de saltar al trampoline crudo (que rompe el fix), incrementar `depth`,
   usar `frames[depth]`, y al salir decrementar. Mientras `depth < MAX_DEPTH`, el fix de stack-swap
   se aplica correctamente en cada nivel. Si `depth == MAX_DEPTH` (anidamiento patológico), ahí sí
   degradar a passthrough como último recurso (mejor que corromper).

5. **`OFF_BYPASS` y `OFF_RAW_TRAMP`** siguen siendo globales del hook (no dependen del hilo): son
   configuración del hook, no estado de llamada. Correcto dejarlos en la página.

### Archivos a tocar
- **`KenshiMP.Scanner/src/mov_rax_rsp_fix.cpp`**: núcleo del cambio (slots → TLS, naked detour con
  thunk C, reentrancia por pila de frames). Líneas clave actuales: `83-92` (offsets), `297-361`
  (naked detour), `377-413` (trampoline wrapper).
- **`KenshiMP.Scanner/include/kmp/mov_rax_rsp_fix.h`**: añadir `s_tlsIndex`, struct de estado,
  prototipo del thunk helper, init/cleanup de TLS.
- **HookManager** (donde llama a `BuildMovRaxRspHookAt`): `TlsAlloc` en init, `TlsFree` en shutdown.
- **`docs/reverse-engineering/mov-rax-rsp-fix.md:65-67`**: actualizar la nota de thread-safety.

### Riesgo
- **Alto.** Es la pieza más delicada del mod: cualquier error en el cálculo de RSP/retorno crashea
  TODOS los hooks `mov rax,rsp` (CharacterCreate, CharacterDeath/KO, FactionRelation, GameFrameUpdate...).
- **Mitigación:** implementar en rama aislada (worktree), validar PRIMERO con un hook de baja frecuencia
  ya estable (p.ej. CharacterDeath), confirmar que sigue funcionando, y SOLO entonces probar ApplyDamage.
  Tener fallback: si el TLS falla, degradar a la página global actual (no romper lo que ya funciona).
- **Verificación sugerida:** test de reentrancia sintético (hook que se llame a sí mismo N veces) +
  test multihilo (disparar el hook desde 2 hilos a la vez) antes de tocar combate real.

### Prioridad: **P3** (prerequisito para P4; NO bloquea el combate del host por sí mismo).

---

## 3. Ruta de órdenes del jugador — ¿hookear `newPlayerTaskSelectedCharacters` o despausar la UI?

### Estado actual
- **`isPlayerCharacter` ya está cubierto de facto** por el fix de facción (`FixCharacterFactionTo`,
  `game_character.cpp:1200-1301`): escribe la player faction en `char+0x10`, que es lo que el motor
  compara en `Character::isPlayerCharacter()` (`char.faction(+0x10) == gameWorld.player(+0x580).faction`).
  El `/verify` ya pasa faction/name (`00-INDEX.md` punto 3). **No hace falta hookear nada más para que
  el motor te reconozca como jugador.**
- **NO existe** ninguna referencia a `newPlayerTaskSelectedCharacters` ni `PlayerTask` en el código
  del mod (grep en Core/Scanner/Common: 0 resultados). Es una función candidata mencionada en la
  consigna, no algo ya integrado.

### Análisis: ¿hace falta hookear la ruta de órdenes?
**Probablemente NO para el host.** Razonamiento:
- En el HOST (single-player modeado), el motor de Kenshi ya tiene su pipeline completo de órdenes:
  clic → selección → `PlayerTask` → IA del char → MoveTo/Attack. Si el char es reconocido como
  jugador (facción ✅) y la UI/input NO está bloqueada, **ese pipeline nativo debería funcionar solo**.
- El mod NO necesita REPLICAR la orden en el host — el host ES la autoridad local. Solo necesitaría
  hookear `newPlayerTaskSelectedCharacters` si quisiera **capturar/reenviar** las órdenes del host a
  los clientes remotos (sync de órdenes, Fase 4), que es una feature posterior, no el bloqueo actual.
- Por tanto: **el bloqueo actual NO es la ausencia del hook de órdenes, sino la pausa de UI/input.**

### Conclusión: basta despausar la UI (+ facción ya arreglada). NO hookear órdenes ahora.

**La "pausa fantasma" es el verdadero bloqueo.** `00-INDEX.md` punto 4 lo confirma: runtime muestra
`paused=0, gameSpeed=1.0` (la simulación corre) PERO la UI dice "PAUSADO" y bloquea atacar/hablar.
Hay un **estado de pausa de UI/input separado** del flag de mundo `+0x8B9`.

### Qué cambiar (lo está atacando otro agente — coordinar)
- **`core.cpp:2338-2390` (Step 0: Force unpause):** hoy solo escribe `GameWorld+0x8B9=0` y
  `gameSpeed(+0x700)=1.0`. Eso despausa la SIMULACIÓN pero NO el estado de UI/input que gobierna
  la aceptación de órdenes. Falta localizar y limpiar el segundo flag.
- **Pistas para el RE del segundo estado de pausa (a verificar en binario):**
  - Kenshi usa MyGUI; el estado "PAUSADO" visible es un overlay/flag de UI, no `GameWorld+0x8B9`.
  - Candidatos: un flag en `PlayerInterface` (`GameWorld+0x580 → +0x...`), o el `timeManager`
    (`time_hooks` captura su ptr; offsets `+0x08`=timeOfDay, `+0x10`=gameSpeed). El "pause de input"
    podría ser un gameSpeed efectivo 0 a nivel de UI o un bool de "menú modal abierto".
  - El WndProc del mod (`render_hooks.cpp`) tiene una **puerta de input modal** (`02-hooks.md` §9):
    cuando chat/menú del mod están activos CONSUME todo el input. **Verificar que esa puerta no esté
    quedándose pegada en "activa"** y tragándose los clics de orden — sería una causa trivial y muy
    probable de "no puedo dar órdenes" que NADA tiene que ver con el flag de pausa del juego.

### Riesgo
- **Bajo-medio.** Escribir un flag de pausa de UI equivocado puede desincronizar la UI (cosmético) o,
  peor, escribir sobre un puntero (si se confunde offset) → AV. Validar con SEH + rango de heap como
  hace el resto del código (`game_character.cpp` patrón `DiagIsHeap`).
- **Riesgo de coordinación:** otro agente ya trabaja en esto. Este plan recomienda que ese agente
  revise PRIMERO la puerta de input modal del WndProc (causa trivial) antes de cazar el flag de pausa
  de UI (causa compleja).

### Prioridad: **P0** (es el bloqueo #1 del combate del host).

---

## 4. Por qué los NPCs huyen aunque la facción esté bien

Tres causas posibles, ordenadas por probabilidad. NO son excluyentes.

### Causa A (más probable): es un EFECTO de la pausa fantasma, no de la facción
Si la simulación local está en un estado mixto (mundo despausado pero UI/IA en estado inconsistente),
la IA de los NPCs puede evaluar el behavior tree con datos a medio inicializar y caer en el comportamiento
por defecto de "huir" (estado de pánico/flee es el fallback seguro de muchos behavior trees cuando el
contexto de combate es inválido). **Predicción falsable:** al arreglar la pausa fantasma (P0), los NPCs
dejan de huir SIN tocar relaciones. Probar P0 primero y re-evaluar.

### Causa B: la facción está bien en el HOST pero las RELACIONES no
`FixCharacterFactionTo` arregla `char+0x10` (a qué facción perteneces), pero NO toca las **relaciones
entre facciones** (`FactionRelation`, `0x872E00`). Los NPCs huyen si su facción tiene relación
hostil/temerosa con la tuya. Considera:
- **`02-hooks.md` punto caliente #2:** el envío de `FactionRelation` depende de
  `GetOffsets().faction.id != -1`. Si el offset de id de facción no está resuelto (históricamente
  `+0x08` hardcodeado y ausente de `FactionOffsets`), las relaciones no se sincronizan ni leen bien.
- **`00-INDEX.md` punto 7:** solo hay **2 facciones de mod** (`10-`/`12-kenshi-online.mod`) para 16
  jugadores (server asigna por `slot % 2`). Si tu char del host queda asignado a una facción cuya
  relación con los NPCs locales es por defecto hostil/neutral-temerosa, huirán. Verificar la relación
  por defecto de la player faction parcheada (`'Sinnombre'`) con las facciones NPC del entorno.

### Causa C: el char es reconocido como jugador pero sin SQUAD/control real
El fix de facción te hace pasar `isPlayerCharacter`, pero si el char no está en el squad del jugador
ni marcado como controlado, la IA de los NPCs puede tratarlo como un "agente suelto sin facción
funcional". Relacionado: `WritePlayerControlled` está DESACTIVADO (`core.cpp:2990`, "can crash"),
y `AddCharacterToLocalSquad` es un exploit frágil del motor (`02-hooks.md` §12). Para el HOST esto
suele estar bien (el char del host SÍ está en su squad nativo), pero verificar que el fix de facción
no haya dejado el char en un estado de facción válida pero squad/control inconsistente.

### Qué verificar / cambiar
1. **Primero P0** (pausa). Re-evaluar si los NPCs siguen huyendo. Si paran → era Causa A, cerrado.
2. Si siguen huyendo: **confirmar `GetOffsets().faction.id`** (debe ser `+0x08` y != -1) en
   `faction_hooks.cpp` y `FactionOffsets`. Loguear la relación real player-faction ↔ NPC-faction.
3. Comparar la facción parcheada con la facción NPC del entorno: ¿la relación por defecto es hostil?
   Si sí, el host necesita que el servidor/mod fije relaciones neutrales por defecto entre la player
   faction y las facciones del mundo (decisión de diseño, no solo RE).

### Riesgo
- **Bajo para diagnosticar** (solo lectura/log). **Medio para corregir relaciones** (escribir
  relaciones de facción mal puede romper el equilibrio del mundo o causar UAF si se escribe sobre una
  facción NPC descargable — mismo patrón de riesgo que el fix de facción remoto documentado).

### Prioridad: **P2** (atacar DESPUÉS de P0; gran parte puede resolverse solo al arreglar la pausa).

---

## 5. Tabla de prioridades consolidada

| P | Tarea | Archivo:línea principal | Qué cambiar | Riesgo | Bloquea combate host |
|---|---|---|---|---|---|
| **P0** | Pausa fantasma de UI/input | `core.cpp:2338-2390` + `render_hooks.cpp` (WndProc modal) | Revisar puerta modal pegada; localizar 2º flag de pausa UI (no `+0x8B9`) | Bajo-Medio | **SÍ (#1)** |
| **P1** | RVA de StartAttack (diagnóstico) | `patterns.h:90,342` + `orchestrator.cpp:169-171` + `combat_hooks.cpp:405` | Renombrar `0x7B2A20`→CutBluntCalc; apuntar DIAG a `MartialArtsCombat 0x892120` (o verificar `0x7A1650`) | Bajo (Opción A) | No (diagnóstico) |
| **P2** | NPCs huyen | `faction_hooks.cpp` + `FactionOffsets` + server relations | Confirmar `faction.id=+0x08`; loguear relación player↔NPC; re-evaluar tras P0 | Bajo diag / Medio fix | Parcial |
| **P3** | MovRaxRsp → TLS | `mov_rax_rsp_fix.cpp:83-92,297-413` + `.h` + HookManager | TLS index + pila de frames por hilo (thunk C) en vez de slots globales | **Alto** | No (prereq P4) |
| **P4** | Rehabilitar ApplyDamage | `combat_hooks.cpp:382-390` | Solo tras P3 estable; sync de daño por intención | Alto | No (motor aplica daño solo) |

---

## 6. Notas de coordinación y verificación

- **P0 y P2 probablemente comparten causa raíz** (la pausa fantasma). Arreglar P0 primero puede cerrar
  P2 sin tocar relaciones. NO sobre-ingenierizar relaciones antes de confirmar que no es la pausa.
- **P1 antes que cualquier conclusión sobre "la orden no llega".** El diagnóstico actual está midiendo
  la función equivocada; cualquier afirmación de "StartAttack no dispara" es inválida hasta arreglarlo.
- **P3 es el único cambio de Alto riesgo en código probado.** Aislar en worktree, validar con hook de
  baja frecuencia, fallback a la página global si TLS falla. NO tocar combate hasta que P3 esté verde
  en tests sintéticos de reentrancia + multihilo.
- **Distinción confirmado vs suposición:**
  - CONFIRMADO: el hook "StartAttack" apunta a `0x7B2A20` = "Cutting damage" (no a iniciar ataque);
    el fix de facción cubre `isPlayerCharacter`; `paused/gameSpeed` ya están en 0/1.0; MovRaxRsp usa
    slots globales no-TLS y degrada a passthrough en reentrancia.
  - SUPOSICIÓN (requiere binario): que `0x7A1650` sea la "StartAttack" real; el offset exacto del 2º
    flag de pausa de UI; que la causa de "NPCs huyen" sea la pausa (Causa A) vs relaciones (Causa B).

---

## Archivos relevantes (rutas absolutas)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\hooks\combat_hooks.cpp` (hooks de combate, DIAG StartAttack)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Scanner\src\mov_rax_rsp_fix.cpp` (núcleo MovRaxRsp → migrar a TLS)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Scanner\include\kmp\mov_rax_rsp_fix.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Scanner\src\orchestrator.cpp` (registro RVA StartAttack:169-171)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Scanner\include\kmp\patterns.h` (START_ATTACK:90, anchor:342)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\core.cpp` (Step 0 Force unpause:2338-2390; fix facción host:2200-2260)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\game\game_character.cpp` (FixCharacterFactionTo:1200-1301)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\game\game_world.cpp` (paused/gameSpeed:156-223)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\hooks\faction_hooks.cpp` (FactionRelation, faction.id)
- `E:\Aplicaciones\Kenshi-Online\docs\reverse-engineering\mov-rax-rsp-fix.md` (diseño actual del fix)
