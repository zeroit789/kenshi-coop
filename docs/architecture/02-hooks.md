# Mapa de Hooks — Kenshi Co-op (KenshiMP.Core/hooks/)

> Mapa de ingeniería inversa de TODOS los detours del mod.
> Binario objetivo: `kenshi_x64.exe` v1.0.68 Steam (App 233860), MSVC x64.
> ImageBase 0x140000000. Las RVAs son del orchestrator (`KenshiMP.Scanner/src/orchestrator.cpp`).
> RVA de FUNCION = cambia por version/plataforma (se resuelve en runtime por pattern/string xref).
> Offsets de CAMPO de struct = iguales Steam/GOG.
>
> Convencion de llamada de todos los detours del juego: `__fastcall` (RCX, RDX, R8, R9 + stack).
> El fix `MovRaxRsp` (HookManager) se auto-aplica a CUALQUIER funcion cuyo prologo sea `48 8B C4` (`mov rax, rsp`).
> Detalle del fix: `docs/reverse-engineering/mov-rax-rsp-fix.md`.

Leyenda de estado:
- **ACTIVO** — hook instalado y haciendo trabajo de sync.
- **DIAG** — instalado pero solo loguea (no altera juego).
- **PASSTHROUGH** — instalado pero llama al original sin tocar nada (placeholder/seguridad).
- **NO-INSTALADO** — la funcion se resuelve pero NUNCA se hookea (riesgo de crash).
- **DESACTIVADO** — explicitamente saltado / sin pattern resuelto.

---

## Tabla resumen

| Hook | Funcion juego | RVA (1.0.68) | Archivo | Estado | Motivo / nota clave |
|---|---|---|---|---|---|
| CharacterCreate | RootObjectFactory::process | 0x581770 | entity_hooks.cpp | **ACTIVO** (passthrough en loading) | Nucleo del spawn. `mov rax,rsp` → MovRaxRsp. |
| CharacterDestroy | ~Character / destroy | 0x86... (no resuelto en orch) | entity_hooks.cpp | **NO-INSTALADO** | `s_destroyHookInstalled=false`. Limpieza via otros caminos. |
| CharacterDeath | Death from blood loss | 0x7A6200 | combat_hooks.cpp | **ACTIVO** | `mov rax,rsp`. Deferred ring → C2S_CombatDeath. |
| CharacterKO | Knockout handler | 0x345C10 | combat_hooks.cpp | **ACTIVO** | Deferred ring → C2S_CombatKO + health. |
| StartAttack | Cut/blunt damage calc | 0x7B2A20 (\*) | combat_hooks.cpp | **DIAG** | Solo log; ver nota de discrepancia RVA. |
| ApplyDamage | Attack damage effect | 0x7A33A0 | combat_hooks.cpp | **NO-INSTALADO** | 300+/seg + `mov rax,rsp` → crash. |
| FactionRelation | Faction relation handler | 0x872E00 | faction_hooks.cpp | **ACTIVO** | → C2S_FactionRelation. |
| CharacterSetPosition | HavokCharacter::setPosition | 0x145E50 | movement_hooks.cpp | **NO-INSTALADO** | `mov rax,rsp` + cientos/frame → polling. |
| CharacterMoveTo | Pathfinding move command | 0x2EF4E3 | movement_hooks.cpp | **NO-INSTALADO** | `mov rax,rsp` + 5º param en stack → crash. |
| GameFrameUpdate | Main game frame tick | 0x123A10 | game_tick_hooks.cpp | **ACTIVO** (diag) | `mov rax,rsp`. Trampoline mode. |
| TimeUpdate | Time scale handler | 0x214B50 | time_hooks.cpp | **ACTIVO** (nunca dispara) | Steam no llama esta fn; Present mueve OnGameTick. |
| AICreate | AI controller creation | 0x622110 | ai_hooks.cpp | **ACTIVO** | Marca remote-controlled, NO devuelve null. |
| AIPackages | AI behavior package loader | 0x271620 | ai_hooks.cpp | **ACTIVO** (diag) | Deja cargar el behavior tree siempre. |
| CharAnimUpdate | Char anim update tick | 0x65F6C7 | char_tracker_hooks.cpp | **ACTIVO** (inline manual) | Hook inline 14 bytes (no MinHook). |
| SquadSpawnBypass | Squad spawn check | 0x4FF47C | squad_spawn_hooks.cpp | **ACTIVO** | Flipea flags de activePlatoon para forzar spawn. |
| SquadCreate | Squad pos reset / mgmt | 0x480B50 | squad_hooks.cpp | **NO-INSTALADO** | `mov rax,rsp` + burst zone-load → crash. |
| SquadAddMember | Delayed spawning / member add | 0x928423 | squad_hooks.cpp | **NO-INSTALADO** | Mid-function (no alineado). Raw ptr para inyeccion. |
| ZoneLoad | Zone loading | 0x377710 | world_hooks.cpp | **ACTIVO** | Deferred → C2S_ZoneRequest. |
| ZoneUnload | Zone unload / navmesh teardown | 0x2EF1F0 | world_hooks.cpp | **ACTIVO** | Deferred → limpieza registro + interp. |
| ItemPickup | Inventory addItem | 0x74C8B0 | inventory_hooks.cpp | **ACTIVO** | → C2S_ItemPickup. Bug owner (resuelto, ver nota). |
| ItemDrop | Inventory removeItem | 0x745DE0 | inventory_hooks.cpp | **ACTIVO** | → C2S_ItemDrop. |
| BuyItem | Shop purchase | 0x74A630 | inventory_hooks.cpp | **ACTIVO** | → C2S_TradeRequest. |
| BuildingPlace | RootObjectFactory::createBuilding | 0x57CC70 | building_hooks.cpp | **ACTIVO** (con auto-disable) | DISTINTO del de world_hooks (ese DESACTIVADO). |
| BuildingDestroyed/Dismantle/Construct/Repair | varios | (no fijados en orch) | building_hooks.cpp | **ACTIVO si resuelto** | Auto-disable tras 10 crashes. |
| DXGI Present | IDXGISwapChain::Present (vt idx 8) | (DXGI dll, no RVA juego) | render_hooks.cpp | **ACTIVO** | Driver real de OnGameTick + WndProc + UI. |
| WndProc | SetWindowLongPtr (Win32) | (no es del juego) | render_hooks.cpp | **ACTIVO** | Input: F1/Tab/Chat/clicks. Puerta modal. |
| Ogre Mesh/Texture/Material load | Ogre::ResourceManager::load | (sin implementar) | resource_hooks.cpp | **DESACTIVADO** | Discovery no implementado → degrada a burst-timing. |
| SaveGame / LoadGame | SaveManager save/load | (no fijado) | save_hooks.cpp | **DESACTIVADO** | Pass-through; firmas no verificadas. |

(\*) **Discrepancia RVA StartAttack**: el comentario de `combat_hooks.cpp:405` dice `0x7A1650`, el orchestrator registra `0x7B2A20`. La direccion real la resuelve el scanner por string `"Cutting damage"`; la RVA hardcodeada es solo fallback. **Verificar**: cual de las dos es el fallback correcto en 1.0.68 (riesgo: si el fallback es erroneo y el pattern falla, hookea funcion equivocada).

---

## 1. entity_hooks.cpp — Ciclo de vida de personajes (EL MAS GRANDE, 1582 lineas)

### Hook_CharacterCreate → RootObjectFactory::process @ 0x581770
- **Firma**: `void* __fastcall(void* factory, void* templateData)`.
- **Prologo**: `48 8B C4` (mov rax,rsp) → MovRaxRsp auto-aplicado. CRITICO para spawn.
- **Estado**: ACTIVO. Es el hook mas complejo del mod. Tres modos:
  1. **Loading passthrough** (`s_loadingPassthrough`): los 130+ creates de carga de save van por la ruta ultra-ligera (solo timestamp/contador + captura de factory + lectura SEH del nombre para detectar plantillas "Player 1".."Player 16"). Cero lecturas pesadas. Evita corromper el hueco de pila de MovRaxRsp en el burst.
  2. **No conectado (captura)**: captura `s_preCallStruct` (1024B) + factory + **votacion de faccion multi-fuente** (ventana de 8 chars, bonus por match de nombre con `config.playerName`, +chequeo flag `isPlayerFaction`). Elige la mejor faccion y se autodesactiva a passthrough.
  3. **Conectado**: cuerpo completo — deteccion de offset de posicion/GameData en el struct, **NPC Hijack** (toma un NPC recien creado en vez de crear uno: lo registra, teletransporta, renombra, desactiva IA — cero riesgo de faccion), registro en EntityRegistry, envio de C2S_EntitySpawnReq.
- **Reentrancia**: guard `thread_local s_hookDepth`; reentradas usan `s_rawCreateTrampoline`.
- **Detalle MovRaxRsp**: tres punteros de llamada distintos:
  - `s_origCreate` = wrapper MovRaxRsp (restaura RSP del juego). Preferido desde dentro del hook.
  - `s_rawCreateTrampoline` = trampoline crudo de MinHook (empieza con `mov rax,rsp`). Para reentradas.
  - `s_directCallStub` = stub propio `mov rax,rsp; jmp rawTramp+3` (17 bytes, RWX→RX). Para `CallFactoryDirect` desde C++.
- **Funciones directas (NO hookeadas, llamadas por puntero)**:
  - `FactoryCreate` = RootObjectFactory::create @ **0x583400** — dispatcher de alto nivel `(factory, GameData*)` que construye un request struct fresco (evita punteros stale). Validado por `.pdata`.
  - `CreateRandomChar` = RootObjectFactory::createRandomChar @ **0x5836E0** — NPC aleatorio (fallback).
- **TODO/bugs**:
  - Appearance/equipo NO se escriben en chars remotos (SEH_HijackNPC solo teletransporta+renombra+IA off). Sync de equipo incompleto (audit gap).
  - Cap de spawns por jugador `MAX_SPAWNS_PER_PLAYER=4`.

### Hook_CharacterDestroy → ~Character/destroy
- **Firma**: `void __fastcall(void* character)`.
- **Estado**: **NO-INSTALADO** (`s_destroyHookInstalled = false`). El cuerpo existe (envia C2S_EntityDespawnReq, decrementa cap, limpia interpolacion, desregistra) pero `Install()` NO lo engancha. La limpieza de despawn ocurre via ZoneUnload + otros caminos.

---

## 2. combat_hooks.cpp — Combate (DESTACADO)

Patron comun: el cuerpo del hook hace trabajo MINIMO (llama original + empuja IDs a un ring buffer lock-free single-producer/single-consumer). El trabajo pesado (spdlog, PacketWriter, SendReliable, lecturas de memoria) se difiere a `ProcessDeferredEvents()` desde `OnGameTick` (contexto seguro). Razon: estos hooks corren dentro de naked detours MovRaxRsp donde heap-alloc corrompe el hueco de 4KB y el mutex de ENet puede deadlockear.

### Hook_CharacterDeath → 0x7A6200
- **Firma**: `void __fastcall(void* character, void* killer)`. Prologo `mov rax,rsp`.
- **Estado**: ACTIVO. Llama original (SEH) → captura entityId/killerId → ring → diferido envia C2S_CombatDeath + snapshot de salud de 7 miembros.
- **Echo suppression**: `s_serverSourcedDeath` (set por packet_handler al aplicar S2C_CombatDeath) evita bucle C2S→S2C→C2S.

### Hook_CharacterKO → 0x345C10
- **Firma**: `void __fastcall(void* character, void* attacker, int reason)`.
- **Estado**: ACTIVO. Igual patron → C2S_CombatKO + salud de pecho + 7 miembros. Echo suppression `s_serverSourcedKO`.

### Hook_StartAttack → 0x7B2A20 (orch) / 0x7A1650 (comentario) — VERIFICAR
- **Firma**: `void __fastcall(void* attacker, void* target, void* weapon)`. Prologo `mov rax,rsp`.
- **Estado**: **DIAG** (solo log). NO modifica combate. Cuerpo minimo: `OutputDebugStringA` (buffer de pila) + ring diferido. Objetivo del diagnostico: comprobar si la orden de ataque del jugador (clic en NPC) LLEGA al motor. Se llama 1 vez al INICIAR ataque (no por tick), por eso es seguro hookear (a diferencia de ApplyDamage).
- **Interpretacion del diag**: si aparece `[DIAG-COMBAT] StartAttack called` al atacar → la orden llega y el problema esta mas abajo (ApplyDamage no hookeado). Si NO aparece nada → la orden se pierde antes (ruta de ordenes/IA/input).

### ApplyDamage → 0x7A33A0 — DESACTIVADO (raiz de crash)
- **Firma esperada**: `void __fastcall(void*, void*, int, float, float, float)`.
- **Estado**: **NO-INSTALADO** deliberadamente. Razon: prologo `mov rax,rsp` Y se dispara cientos de veces por tick de combate. Los slots RSP globales del wrapper MovRaxRsp se corrompen bajo llamadas rapidas → crash determinista en "attack unprovoked". Funcion enorme (6925 bytes), la ventana de scan de 4KB es inadecuada.
- **Consecuencia (audit gap)**: C2S_AttackIntent NUNCA se envia, asi que server ResolveCombat/HandleAttackIntent son inalcanzables. La visibilidad de daño es solo por eventos (death/KO) + polling de salud, no por daño. Sync de daño = death/KO hooks + health polling.

---

## 3. faction_hooks.cpp — Relaciones de faccion (DESTACADO)

### Hook_FactionRelation → 0x872E00
- **Firma**: `void __fastcall(void* factionA, void* factionB, float relation)`.
- **Estado**: ACTIVO. Llama original (SEH) → lee `factionId` en ambas (offset `GetOffsets().faction.id`) → C2S_FactionRelation con factionIdA/B + relation.
- **Gates**: `s_loading` (durante carga de save) y `s_serverSourced` (eventos del servidor) suprimen el envio para evitar bucles.
- **Bug/nota**: si `faction.id` offset es -1 (no resuelto), salta el paquete. El offset de id de faccion estaba hardcodeado historicamente (+0x08) y faltaba en FactionOffsets — verificar que `GetOffsets().faction.id` lo provee ahora. Relevante para el bug conocido de facciones (enemigos huyen) del proyecto.

---

## 4. movement_hooks.cpp — Posicion / movimiento (DESTACADO)

**Ambos hooks NO-INSTALADOS por crash de MovRaxRsp.** El sync de movimiento va por POLLING desde `OnGameTick` (cada `KMP_TICK_INTERVAL_MS`), no por hook. Los cuerpos existen y son funcionales pero `Install()` solo loguea y no engancha.

### Hook_SetPosition → CharacterSetPosition @ 0x145E50
- **Firma**: `void __fastcall(void* character, float x, float y, float z)`.
- **Estado**: **NO-INSTALADO**. Prologo `mov rax,rsp` (rax captura RSP equivocado en trampoline) + se llama cientos de veces/frame (HookBypass demasiado caro).
- **Cuerpo (si se activase)**: guard de char remoto (no deja que physics/IA pisen posiciones de remotos), throttle a tick rate, umbral de cambio de pos, lee rotacion/velocidad/animState, envia C2S_PositionUpdate (unreliable).

### Hook_MoveTo → CharacterMoveTo @ 0x2EF4E3
- **Firma**: `void __fastcall(void* character, float x, float y, float z, int moveType)`.
- **Estado**: **NO-INSTALADO**. `mov rax,rsp` + un **5º parametro en stack** (moveType) que el naked detour MovRaxRsp no puede reenviar → crash en CADA llamada (rompe el click-to-move).
- **Logica de interes (si activo)**: BLOQUEA MoveTo para chars remotos (`ai_hooks::IsRemoteControlled`) — NO llama original, suprime la decision de la IA para que no pelee con la interpolacion de red. Para chars locales propios envia C2S_MoveCommand.

> Relacion con IA/ordenes: el bloqueo de movimiento de remotos se mueve a `ai_hooks` (MoveTo no instalado). La supresion de decisiones IA de remotos depende de `MarkRemoteControlled`.

---

## 5. ai_hooks.cpp — Control de IA (afecta IA/ordenes)

### Hook_AICreate → 0x622110
- **Firma**: `void* __fastcall(void* character, void* faction)`.
- **Estado**: ACTIVO. SIEMPRE llama al original (SEH) — devolver null era la causa raiz de crashes (codigo downstream deref el AI controller sin null-checks: combate, pathfinding, anim, seleccion UI). Si el char es de un jugador remoto (`info->isRemote`), lo marca `MarkRemoteControlled` — el controller IA queda VALIDO, solo se sobreescriben las DECISIONES.

### Hook_AIPackages → 0x271620
- **Firma**: `void __fastcall(void* character, void* aiPackage)`.
- **Estado**: ACTIVO (diagnostico). Siempre deja cargar los AI packages (el behavior tree debe existir o el motor crashea al consultarlo). Solo loguea para chars remotos.

### Estado remote-controlled (registro compartido)
- `s_remoteControlled` (unordered_set + mutex). `MarkRemoteControlled`/`UnmarkRemoteControlled`/`IsRemoteControlled`. Consumido por movement_hooks (bloqueo MoveTo) y SEH_HijackNPC.

---

## 6. game_tick_hooks.cpp — Tick principal

### Hook_GameFrameUpdate → 0x123A10
- **Firma**: `void __fastcall(void* rcx, void* rdx)`. Prologo `mov rax,rsp` → MovRaxRsp (trampoline mode, sin suspension de hilos).
- **Estado**: ACTIVO pero principalmente DIAGNOSTICO. Loguea via `OutputDebugStringA` (NO spdlog en detour). Diag de spawn cada 3000 ticks. Llama original via wrapper MovRaxRsp con SEH.
- **Nota**: spawn fallback se maneja SOLO en `Core::HandleSpawnQueue` (timeout 10s); el spawn directo de 3s fue retirado para no competir con el in-place replay de entity_hooks. Probes diferidos (AnimClass/PlayerControlled) DESACTIVADOS (flood + nunca acertaban).

---

## 7. time_hooks.cpp — Tiempo / velocidad (afecta pausa/velocidad)

### Hook_TimeUpdate → 0x214B50
- **Firma**: `void __fastcall(void* timeManager, float deltaTime)`. Prologo `mov rax,rsp`.
- **Estado**: ACTIVO pero **NUNCA DISPARA en Steam** — la funcion en 0x214B50 no la llama el juego en builds Steam. El driver real de OnGameTick es `render_hooks` Present. Si empezase a disparar, el guard de dedup de 4ms en OnGameTick evita doble-procesado.
- **Funcion util**: captura el puntero `timeManager` en primera llamada. Offsets de campo: `+0x08` = timeOfDay, `+0x10` = gameSpeed. `SetServerTime` escribe directo esos offsets (sync de hora/velocidad del servidor). Cliente no-host multiplica deltaTime por `s_serverGameSpeed`.

---

## 8. input_hooks.cpp — Input (afecta input/ordenes)

- **Estado**: NO hookea nada del juego directamente. El input real se maneja en el WndProc de render_hooks. `Install()` solo pone `s_installed=true`. Documenta los keybinds (Tab/Enter/F1/Escape/Tilde) implementados realmente en render_hooks.

---

## 9. render_hooks.cpp — Render / Present / Input modal (DESTACADO para input/pausa)

### HookPresent → IDXGISwapChain::Present (vtable idx 8)
- **Firma**: `HRESULT __stdcall(IDXGISwapChain*, UINT syncInterval, UINT flags)`.
- **Estado**: ACTIVO. NO renderiza ImGui (conflicto Ogre3D/DX11 → crash). Es passthrough + responsabilidades clave:
  - **Driver de OnGameTick** (la fuente real, no TimeUpdate). dt acotado a (0, 0.5).
  - Maquina de fases por timing de Present: Startup→MainMenu (5s), MainMenu→Loading (gap >2s), Loading→GameReady (8s frames suaves → PollForGameLoad), deteccion de save in-game (gap >10s en GameReady).
  - Descubre el HWND del swapchain e instala el WndProc hook (una vez).
  - Update de Overlay + NativeHud (cada uno con SEH).

### HookWndProc → SetWindowLongPtrA(GWLP_WNDPROC) (Win32, no del juego)
- **Estado**: ACTIVO. Gestiona TODO el input del mod con SEH:
  - F1 = menu nativo; Tab = player list (consume, evita accion Tab de Kenshi); Insert = panel log; Tilde/OEM_3 = debug info; Escape = cierra chat/menu; Enter = toggle chat.
  - **Puerta de input modal**: cuando chat o menu estan activos, CONSUME todo WM_CHAR/WM_KEYDOWN/WM_KEYUP para que el juego (OIS/MyGUI/DirectInput) no procese tambien las teclas. Arregla doble-tecleo y evita acciones del juego con UI abierta.
  - Click en boton MULTIPLAYER del menu principal (coords normalizadas de `Kenshi_MainMenu.layout`).
- **Nota de pausa/ordenes**: el mod NO hookea la pausa del juego directamente; el control de input modal es la unica intervencion. La velocidad/pausa se sincroniza via time_hooks `SetServerTime` (gameSpeed), no por hook de input.
- **Thread safety**: WndProc corre en el hilo de mensajes de ventana; Present en el de render. OnGameTick lo dispara Present (hilo render) — la logica de juego del mod vive en el hilo de render, no en uno separado.

---

## 10. char_tracker_hooks.cpp — Descubrimiento de personajes por animacion

### Inline hook → CharAnimUpdate @ 0x65F6C7
- **Firma efectiva**: el AnimationClassHuman* esta en RBX en el sitio del hook.
- **Estado**: ACTIVO. **NO usa MinHook** — es un hook INLINE manual de 14 bytes (`FF 25` jmp absoluto a trampoline RWX propio). Razon: se dispara 300+/seg; necesita guardar/restaurar todos los registros y ejecutar los 14 bytes originales.
- **Cuerpo**: lee char* en `animClass+0x2D8`. Fast-path (ya trackeado): actualiza timestamp con `try_lock` (sin stall). Slow-path (nuevo): empuja a ring buffer lock-free; `ProcessDeferredDiscovery()` (desde OnGameTick) hace la lectura cara de nombre (CharacterAccessor) + insercion en el mapa.
- **Uso**: resuelve animClass de jugador local/remoto por nombre. Cero heap/mutex/spdlog en el cuerpo del hook.

---

## 11. squad_spawn_hooks.cpp — Bypass de spawn de squad (afecta spawn/IA)

### Hook_SquadSpawnCheck → SquadSpawnBypass @ 0x4FF47C
- **Firma**: `void __fastcall(void* context, void* activePlatoon)`.
- **Estado**: ACTIVO. Cuando hay spawns pendientes (spawn_manager o cola local), FUERZA la creacion de NPC que el juego saltaria. Lee flags de `activePlatoon` (offsets de research mod):
  - `+0xF0` = skip check 1 (bool), `+0x58` = skip check 2 (bool), `+0x250` = ptr (debe ser 0 para bypass), `+0x78` = squad ptr, `+0xA0` = leader.
  - Condicion research: `check3==0 && check1==true` → flipea check1/check2 a false, llama original (spawnea), restaura valores.
- **entity_hooks NPC Hijack** luego captura el NPC resultante de la cola de spawn_manager. Cuerpo sin spdlog (OutputDebugStringA + SEH reads/writes).

---

## 12. squad_hooks.cpp — Squads e inyeccion (afecta ordenes de grupo)

### SquadCreate → 0x480B50 — DESACTIVADO
- **Firma**: `void* __fastcall(void* squadManager, void* templateData)`. Prologo `mov rax,rsp`.
- **Estado**: **NO-INSTALADO**. El trampoline crudo parecia seguro pero causaba crashes silenciosos en carga de zona cuando se crean 100+ squads NPC rapido. No se necesita para el host.

### SquadAddMember → 0x928423 — DESACTIVADO como hook, raw ptr usado
- **Firma**: `void __fastcall(void* squad, void* character)`.
- **Estado**: **NO-INSTALADO como hook**. Se dispara 30-40+ veces en carga de zona → corrupcion acumulada → crash ~10s despues. Ademas 0x928423 NO esta alineado a 16 bytes y puede ser entrada mid-function (descubierta por vtable). `Install()` valida via `.pdata` (RtlLookupFunctionEntry): si es mid-function, desactiva; si es entry valido, guarda el raw ptr.
- **Uso del raw ptr — `AddCharacterToLocalSquad` (EXPLOIT del motor)**: añade un char remoto al squad del jugador local llamando directamente a la fn del motor. Resuelve `activePlatoon` (NO platoon) escaneando el struct del char (0x600..0x780; GOG=0x658) y validando contra la vtable conocida por RTTI (SquadAddMember en slot 2 = +0x10). Hace que el char aparezca en el panel de squad, sea seleccionable y responda a ordenes de grupo.
- **TODO**: offsets de Steam difieren de GOG; identificacion por match de vtable RTTI. Si la inyeccion crashea (AV), se autodesactiva el resto de la sesion.

---

## 13. world_hooks.cpp — Zonas (afecta carga/posicion)

Patron deferred igual que combat (ring buffer → OnGameTick).

### Hook_ZoneLoad → 0x377710
- **Firma**: `void __fastcall(void* zoneMgr, int zoneX, int zoneY)`.
- **Estado**: ACTIVO. Llama original (SEH) → encola evento → diferido envia C2S_ZoneRequest.

### Hook_ZoneUnload → 0x2EF1F0
- **Firma**: `void __fastcall(void* zoneMgr, int zoneX, int zoneY)`.
- **Estado**: ACTIVO. Llama original (SEH, ANTES de quitar entidades) → diferido: para cada entidad remota de la zona decrementa cap de spawn + limpia interpolacion, luego `RemoveEntitiesInZone` (fix de bugs 2+3 de cap saturado y estado interp huerfano).

### BuildingPlace (en world_hooks) — DESACTIVADO
- **Estado**: **DESACTIVADO**. Firma no verificada, crashes en carga de zona. (El BuildingPlace ACTIVO esta en building_hooks.cpp con otra RVA/proposito.)

---

## 14. inventory_hooks.cpp — Inventario / comercio

Usa `SafeCall_*` (maneja trampolines MovRaxRsp con HookHealth que auto-desactiva tras crash).

### Hook_ItemPickup → 0x74C8B0
- **Firma**: `void __fastcall(void* inventory, void* item, int quantity)`.
- **Estado**: ACTIVO. Lee el OWNER del inventario en `inventory + GetOffsets().inventory.owner` (el registry mapea CHARACTER, no inventory). → C2S_ItemPickup.
- **Bug historico (RESUELTO en este archivo)**: antes pasaba el ptr de inventory (no el char owner) a GetNetId. Ahora deref el owner primero. Si `inventory.owner` offset es -1, retorna sin enviar.

### Hook_ItemDrop → 0x745DE0
- **Firma**: `void __fastcall(void* inventory, void* item)`. Mismo patron owner-deref → C2S_ItemDrop.

### Hook_BuyItem → 0x74A630
- **Firma**: `void __fastcall(void* buyer, void* seller, void* item, int quantity)`. → C2S_TradeRequest. (Aqui buyer YA es el char, GetNetId directo.)

---

## 15. building_hooks.cpp — Edificios

5 hooks, cada uno con SEH wrapper + **auto-disable tras 10 crashes** y recuperacion tras 60s sin crash (por si el scanner emparejo la funcion equivocada).

| Hook | Firma | Envio |
|---|---|---|
| Hook_BuildingPlace | `void __fastcall(void* world, void* building, float x,y,z)` | C2S_BuildRequest (templateId de `building+0x28→+0x08`, rotacion si offset>=0) |
| Hook_BuildingDestroyed | `void __fastcall(void* building)` | C2S_EntityDespawnReq (reason=1) |
| Hook_BuildingDismantle | `void __fastcall(void* building)` | C2S_BuildDismantle |
| Hook_BuildingConstruct | `void __fastcall(void* building, float progress)` | solo log cada 50 |
| Hook_BuildingRepair | `void __fastcall(void* building, float amount)` | solo log cada 50 |

- **Estado**: ACTIVOS si la funcion correspondiente se resuelve (RVAs no fijadas en orchestrator; resueltas por pattern/string). Gate `s_loading`.
- **Nota**: `BuildingPlace` aqui (createBuilding, string `[RootObjectFactory::createBuilding]`, RVA ~0x57CC70) es DISTINTO y SEPARADO del BuildingPlace DESACTIVADO en world_hooks.cpp.

---

## 16. resource_hooks.cpp — Recursos Ogre — DESACTIVADO

- **Objetivo**: hookear Ogre::Mesh/Texture/MaterialManager::load (virtuales en singletons) para timing de carga preciso.
- **Estado**: **DESACTIVADO / sin implementar**. `TryDiscoverOgreManagers()` solo comprueba si OgreMain.dll esta cargado (esta linkeado estatico en kenshi_x64.exe) y retorna false. Degrada con gracia → LoadingOrchestrator usa burst-detection timing (comportamiento previo). Framework listo, faltan los indices de vtable Ogre (requiere analisis runtime).

---

## 17. save_hooks.cpp — Guardado/Carga — DESACTIVADO (pass-through)

- **SaveGame** `void __fastcall(void* saveManager, const char* saveName)` — permite save local siempre (checkpoint del jugador; el servidor persiste el estado autoritativo aparte).
- **LoadGame** `void __fastcall(void* saveManager, const char* saveName)` — pone `s_loading=true` para que entity/combat hooks salten ops de red durante la carga.
- **Estado**: **DESACTIVADO**. `Install()` no engancha (firmas no verificadas, trampolines malos crashean en load). Los cuerpos existen pero pasan en pass-through. `IsLoading()` consultado por otros modulos (gate de carga).

---

## Resumen de DESACTIVADOS / NO-INSTALADOS y por que

| Funcion | Razon |
|---|---|
| ApplyDamage (0x7A33A0) | `mov rax,rsp` + 300+/seg → corrupcion slots MovRaxRsp → crash. Sin alternativa de attack-intent. |
| CharacterSetPosition (0x145E50) | `mov rax,rsp` + cientos/frame → polling en su lugar. |
| CharacterMoveTo (0x2EF4E3) | `mov rax,rsp` + 5º param stack que el naked detour no reenvia → crash click-to-move. |
| CharacterDestroy | Decision: `s_destroyHookInstalled=false`; limpieza por ZoneUnload. |
| SquadCreate (0x480B50) | `mov rax,rsp` + burst 100+ en zona → crash silencioso. |
| SquadAddMember (0x928423) | Mid-function no alineado + burst 30-40 en zona. Raw ptr para inyeccion directa. |
| world_hooks BuildingPlace | Firma no verificada, crash en zona. (El de building_hooks SI activo.) |
| Ogre resource hooks | Discovery de vtable no implementado → burst-timing fallback. |
| SaveGame/LoadGame | Firmas no verificadas → pass-through. |
| TimeUpdate (0x214B50) | Instalado pero el juego Steam NUNCA lo llama → Present mueve el tick. |

## Puntos calientes de verificacion (incertidumbre real)

1. **StartAttack RVA**: 0x7B2A20 (orch) vs 0x7A1650 (comentario combat_hooks). Confirmar cual es el fallback real en 1.0.68; el pattern `"Cutting damage"` deberia ganar, pero si falla el fallback erroneo hookea otra fn.
2. **faction.id offset**: el envio de FactionRelation depende de `GetOffsets().faction.id != -1`. Verificar que esta resuelto (historicamente +0x08 hardcodeado y ausente de FactionOffsets). Liga con el bug conocido de facciones.
3. **building_hooks BuildingPlace** comparte string/proposito con `[RootObjectFactory::createBuilding]` — confirmar que NO colisiona con CharacterCreate (mismo RootObjectFactory) en el scanner.
4. **CharacterDestroy** intencionalmente no instalado — confirmar que ZoneUnload cubre TODOS los despawns (chars que mueren fuera de unload de zona podrian no limpiarse).
