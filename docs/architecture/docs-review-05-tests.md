# docs-review-05 — Revisión a fondo de los TESTS (Unit / Integration / Live / TestClient)

> Objetivo: entender qué prueban los tests sobre el **control y el combate del char del host**,
> qué se ESPERA que funcione, y qué pistas dan sobre `playerControlled` / órdenes / IA.
> Contexto del bug: el char del host está vivo, recibe AI tick, pero **no actúa ni responde a órdenes**;
> la simulación de personajes está congelada (reloj avanza, personajes no) = sistema de sincronización (Fase 4).

---

## TL;DR — Lo más relevante

1. **NINGÚN test cubre el control ni el combate del char del HOST.** Los 4 proyectos de test prueban
   exclusivamente **red, serialización y registro de entidades remotas**. No hay un solo test que
   verifique que el personaje local del host se mueve por órdenes, pelea, hace AI tick, o que el flag
   `playerControlled` esté bien.

2. **`playerControlled` / `IsPlayerControlled` / `SetPlayerControlled` / `WritePlayerControlled` NO se
   prueban en ningún test.** Estas funciones existen y son críticas en producción
   (`KenshiMP.Core/game/game_character.cpp`), pero el UnitTest solo ejercita
   `GetPosition/WritePosition/GetName/WriteName/GetFactionPtr/WriteFaction`. La cobertura del
   `CharacterAccessor` deja fuera justo el campo sospechoso.

3. **No existe ningún concepto de "órdenes" (Tasker / GOAP / AddTask) en los tests.** El movimiento que
   prueban es 100% por **escritura directa de posición** (`WritePosition` / `CharacterPosition`), nunca por
   dar una orden al motor para que el personaje camine/pelee. Es decir: los tests asumen el modelo
   **"puppet"** (teletransporte de coordenadas), no el modelo **"el motor mueve al personaje"**. Esto
   encaja con que el combate del host esté congelado: nada en la suite valida que el AI/Tasker del char
   local procese órdenes.

4. **El "host" en los tests es un fake de red, no el char real.** En el IntegrationTest "Host" y "Joiner"
   son dos `TestClient` ENet sin Kenshi detrás. En el TestClient, "host position" es solo la primera
   posición ajena que llega por red, para spawnear al lado. Ninguno toca memoria de un Kenshi real.

5. **El único test que toca un Kenshi real (LiveTest) verifica el control del host SOLO por logs,
   y de forma muy débil.** El milestone `P1: Position Sync` se da por bueno con que aparezca el string
   `"Core::OnGameTick:"` en el log — es decir, que el tick **se ejecute**, no que el personaje **se mueva
   o pelee**. No hay milestone de combate, ni de "host se movió por orden", ni de AI tick efectivo.

**Conclusión:** los tests describen cómo debería funcionar la **capa de red y el espejo de entidades
remotas**, pero **NO** describen cómo debería controlarse/pelear el char local del host. El bug del host
congelado cae justo en el hueco no testeado. La suite ni siquiera fallaría aunque el char del host
estuviera 100% congelado, porque nadie comprueba su simulación.

---

## Inventario de los 4 proyectos

| Proyecto | Qué es | Toca Kenshi real | Toca char del host |
|---|---|---|---|
| **KenshiMP.UnitTest** | Lógica pura con memoria FALSA (`FakeCharacter` de 4KB). Sin red. | No | No (chars fake) |
| **KenshiMP.IntegrationTest** | Arranca `KenshiMP.Server.exe` real + 2 clientes ENet fake. Prueba protocolo end-to-end. | No (server sí, juego no) | No |
| **KenshiMP.TestClient** | Cliente ENet de consola que se hace pasar por un jugador (camina en patrulla por red). | No | No |
| **KenshiMP.LiveTest** | Orquestador: lanza Server + TestClient + Kenshi real vía Steam. Vigila milestones por log/pipe. | Sí (P1=Kenshi) | Solo observa por log, no verifica simulación |

---

## 1) KenshiMP.UnitTest — qué se ESPERA del `CharacterAccessor`

Es el único test que ejercita el `CharacterAccessor` (el código que de verdad lee/escribe el char en
Kenshi). Usa `FakeCharacter` (buffer de 4KB con valores escritos en los offsets reales).

Cobertura real del accessor (asserts):
- `GetPosition` lee Vec3 en `offsets.character.position`. **Esperado:** devuelve exactamente lo escrito.
- `GetName` / `WriteName` con strings SSO de MSVC (≤15 chars). **Esperado:** round-trip correcto.
- `WritePosition` — comentado como "fallback mode": escribe a "posición cacheada". Round-trip OK.
- `GetFactionPtr` / `WriteFaction` — puntero de facción. Round-trip OK.

**Lo que NO cubre (clave para el bug):**
- ❌ `IsPlayerControlled()` — existe en producción (`game_character.cpp:238`), **0 tests**.
- ❌ `SetPlayerControlled(bool)` (`game_character.cpp:1396`) — **0 tests**.
- ❌ `WritePlayerControlled(charPtr, bool)` (`game_character.cpp:1547`) — **0 tests**.
- ❌ `ProbePlayerControlledOffset()` (descubre el offset comparando player=1 vs npc=0) — **0 tests**.
- ❌ Cualquier forma de **orden / Tasker / GOAP / AddTask / AI tick / animación efectiva**.
- ❌ `moveSpeed` y `animStateId` se transportan en el packet, pero **nunca se valida que muevan o animen**
  un personaje; solo que el byte sobrevive al round-trip de serialización (Test_PacketRoundTrip).

El resto del UnitTest (tests 6-13) es `EntityRegistry` + `Interpolation` + serialización de packets +
simulación de sesión de 4 jugadores. **Toda la "sesión" usa chars fake y escribe posiciones a mano**; es
un modelo puppet puro. El "Full Spawn Flow" (test 12) documenta el flujo que el equipo cree correcto para
un **remoto**: spawn → register → link gameObject → `WriteName` → `WriteFaction` → `WritePosition`.
Nótese que **ni siquiera en el flujo "completo" se llama a `SetPlayerControlled`** — los remotos se mueven
por `WritePosition`, no por control de IA.

> Pista fuerte: si el modelo mental del proyecto (reflejado en los tests) es "remoto = WritePosition
> directo", entonces para el **host** no hay un modelo testeado en absoluto. El host no es remoto: debería
> moverse/pelear por su propia simulación de Kenshi (órdenes del jugador → Tasker/GOAP → AI tick). Nada de
> eso está modelado ni verificado.

---

## 2) KenshiMP.IntegrationTest — protocolo end-to-end (sin juego)

Arranca el server real y conecta 2 `TestClient` ENet. Prueba el **pipeline de red**, no el control del char.

Qué se ESPERA que funcione (asserts):
- Conexión + handshake + asignación de PlayerID.
- `PlayerJoined` / `PlayerLeft` se propagan.
- **EntitySpawn**: cliente pide spawn (`C2S_EntitySpawnReq`) → recibe su propia entidad con ID asignado por
  server → el otro cliente ve el broadcast. Verifica autoría (cada uno solo posee sus entidades).
- **PositionSync**: cliente 1 manda 5 `C2S_PositionUpdate` → cliente 2 los recibe. **Solo cuenta packets
  recibidos** (`posUpdatesReceived > before`); NO valida que muevan ningún personaje.
- Chat, TimeSync, Inventory, Trade, Squad, FactionRelation, Building place/dismantle: todos son
  **round-trips de broadcast** (mando mensaje → el otro lo recibe). Lógica de juego cero.
- DisconnectCleanup: al irse un cliente, el otro recibe `EntityDespawn` + `PlayerLeft`.

El "Full Multiplayer Session" (Host/Joiner) es el más completo, pero "Host" es un `TestClient` ENet:
no hay char de Kenshi, no hay AI, no hay órdenes. Verifica que los mensajes fluyen, nada más.

**Relevancia para el bug:** confirma que la **capa de transporte y el server están sanos**. El problema
del host congelado NO está en la red (esta suite lo cubre y pasa); está en la **aplicación de la
simulación al char local**, que esta suite no toca.

---

## 3) KenshiMP.TestClient — el "jugador" automatizado

Cliente de consola que simula a un jugador. **Camina por una ruta de patrulla generando coordenadas y
mandándolas como `C2S_PositionUpdate`** (`SendPositionUpdate`, líneas 106-154). Es decir, el movimiento es
matemática local + envío de posición; **nunca da una orden a un motor**. Refuerza el modelo puppet.

Detalle "host": `g_hostPosKnown` / `HOST POSITION FOUND` (líneas 232-253) = simplemente toma la primera
entidad ajena con posición no nula para spawnear a 10 unidades de ella. No tiene nada que ver con el char
del host real ni con su control.

Sí define handlers para `S2C_CombatHit`, `S2C_CombatDeath`, `S2C_HealthUpdate` (líneas 321-344): **solo los
imprime**. No hay assert ni lógica; el TestClient nunca origina combate ni verifica que el host pelee.

---

## 4) KenshiMP.LiveTest — único contacto con Kenshi real

Lanza Server + TestClient + Kenshi (P1 = juego real con el DLL). Vigila milestones por **string matching**
en el log de Kenshi (P1) y en el stdout del TestClient (P2).

Milestones de P1 (host real) — TODOS por presencia de string en log:
```
"P1: Game Loaded"        -> "=== Core::OnGameLoaded() COMPLETE ==="
"P1: Entities Scanned"   -> "Scanning local squad characters"
"P1: Remote Entity Spawn"-> "Remote player spawned"
"P1: Position Sync"      -> "Core::OnGameTick:"        <-- el "control" del host se reduce a ESTO
"P1: Spawn Ready"        -> "Spawn system ready"
```

**Aquí está la debilidad crítica para el bug:**
- El único milestone relacionado con que el host "haga algo" es `P1: Position Sync`, y se cumple con que
  aparezca `"Core::OnGameTick:"` en el log. Eso solo prueba que **el tick se ejecuta**, exactamente el
  síntoma que ya tienes (reloj avanza). NO prueba que el char se mueva, ni que procese una orden, ni que
  pelee, ni que su simulación no esté congelada.
- **No hay milestone de combate. No hay milestone de "host se movió por orden del jugador". No hay
  milestone de AI tick efectivo ni de `playerControlled` correcto.**
- La "Authority Verification" del dashboard mira si P2 tiene entidad, si P1 ve a P2, si P2 ve a P1 y si
  llegan posiciones — todo **visibilidad de red**, nada de simulación del char local.
- `PrintFinalReport` da el test por OK con `p1Passed >= 6` (de 13). Es decir, **el LiveTest puede reportar
  PASS con el char del host completamente congelado**, siempre que cargue el juego, instale hooks y se
  conecte.

Por tanto el LiveTest NO revela cómo debería controlarse/pelear el host; solo confirma que el DLL carga,
hookea y conecta.

---

## Contraste: lo que SÍ existe en producción y los tests ignoran

En `KenshiMP.Core/game/game_character.cpp` el accessor real expone control de IA/jugador que **ningún test
ejercita**:

- `IsPlayerControlled()` (`:238`) — lee `offsets.character.isPlayerControlled`.
- `SetPlayerControlled(bool)` (`:1396`) y `WritePlayerControlled(charPtr, bool)` (`:1547`) — escriben el
  flag (byte 0/1).
- `ProbePlayerControlledOffset(player, npc)` (`:1505`) — descubre el offset asumiendo **player char = 1,
  NPC = 0**. Esta es la semántica que el equipo cree correcta: el char del host TIENE que tener
  `playerControlled=1`.
- Diagnóstico de combate `[DIAG-COMBAT]` (`game_character.cpp` ~`:1188-1197`) que loguea
  `isPlayerControlled@+off`.

`game_tick_hooks.cpp` hookea `GameFrameUpdate` en **modo trampolín** (sin suspender hilos, `:124`). El hook
cuenta ticks y dispara spawns, pero **no hay aquí ninguna lógica de control del char local** — el control
debe venir por otro lado (player_controller / sync_orchestrator), y **nada de eso está bajo test**.

---

## Implicaciones para depurar el host congelado

1. **Los tests no te van a ayudar a reproducir el bug** y, peor, **te dan falsa confianza**: la suite pasa
   con el host congelado. Cualquier "todos los tests en verde" NO significa que el control del host
   funcione.

2. **El modelo testeado es puppet (WritePosition), no control por IA.** Si el host debe moverse/pelear por
   su propia simulación (lo normal para el jugador local), ese camino **nunca se diseñó como testeable** y
   probablemente nunca se validó. Sospecha primaria reforzada: el host está siendo tratado como si fuera un
   remoto-puppet en algún punto, o su `playerControlled`/Tasker quedó deshabilitado.

3. **`playerControlled` es el candidato nº1 y está sin cubrir.** Acción concreta sugerida:
   - Añadir al **UnitTest** un caso `Test_PlayerControlledReadWrite` análogo al de faction: escribir 1,
     leer 1; escribir 0, leer 0; y verificar que `ProbePlayerControlledOffset` distingue player(1)/npc(0).
     Esto al menos blinda el accessor del flag.
   - En el **LiveTest**, añadir un milestone real que loguee y verifique
     `isPlayerControlled` del primary char del host **después** del game load (debería ser 1). Si en runtime
     sale 0 o el offset no se descubrió (`offset < 0`), ahí está la causa: `SetPlayerControlled`/`Write*`
     devuelven `false` silenciosamente cuando `offset < 0` (`:1398`, `:1549`), dejando al char sin control.

4. **Falta por completo cobertura de "orden → Tasker/GOAP → movimiento/combate".** Ningún test demuestra
   que el motor procese una orden para el char local. Si el bug es "las órdenes no llegan al host", la suite
   no lo detectaría. Hay que instrumentar el camino real (player_controller / AddTask) y verificarlo en
   LiveTest, porque hoy no existe.

5. **El AI tick "se ejecuta" no es lo mismo que "el char actúa".** El milestone `"Core::OnGameTick:"` es
   exactamente el espejismo que ya observas. La simulación congelada (reloj avanza, personajes no) sugiere
   que el tick del motor corre pero la **simulación de los Character está suspendida/no-avanzada** — esto
   apunta a Fase 4 (sincronización) y a algo que congela el avance de los personajes, no a la red ni a la
   serialización (ambas testeadas y verdes).

---

## Veredicto sobre las preguntas planteadas

- **¿Los tests revelan cómo DEBERÍA funcionar el control y combate del char local del host?**
  **No.** Revelan el modelo de red + espejo de remotos (puppet por WritePosition). El control por
  IA/órdenes del char local no está modelado ni verificado en ningún punto.

- **¿Hay un test del char del host que pelee o se mueva por órdenes?**
  **No.** Cero. El movimiento testeado es siempre escritura directa de coordenadas; el combate solo se
  imprime al recibirlo por red, nunca se origina ni se verifica en el host.

- **¿Apuntan los tests al sospechoso (`playerControlled` / órdenes que no llegan)?**
  **Por omisión, sí, con fuerza.** El hecho de que `IsPlayerControlled/SetPlayerControlled/
  WritePlayerControlled` existan en producción y NO tengan ni una sola línea de test, sumado a que
  `Set/Write` fallan en silencio cuando el offset no se descubrió (`offset < 0`), convierte a
  `playerControlled` en el primer sitio donde mirar: verificar en runtime que el offset se descubrió y que
  el primary char del host tiene el flag = 1 tras el game load.
