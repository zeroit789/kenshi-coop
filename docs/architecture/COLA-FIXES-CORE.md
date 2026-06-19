# Cola de fixes pendientes en KenshiMP.Core (de uno en uno, no se pisan)

> Todos tocan Core → se hacen SECUENCIALES (un agente a la vez) para no corromper el código.
> Orden por prioridad. Marcar ✅ al completar.

## 1. 🔧 Combate + tareas congeladas (EN CURSO — agente ad7b85e)
La pausa ya está resuelta (subsysCache=0, officialSetter=yes) PERO sigue sin funcionar:
- Atacar (la orden no llega a StartAttack 0x7A1650, el hook no se dispara).
- Levantarse de la cama, recuperarse de KO (tareas de personaje congeladas).
- NPCs huyen / no atacan.
Investigando: ruta de órdenes, los otros 2 subsistemas de pausa (GameWorld+0x8C0 + 3º), AI tick, tracker.

## 2. 🔇 Spam del HUD "Looking for characters... (tracked: 0)" (PETICIÓN DE ZERO)
El chat/HUD se llena sin parar con `[System] Looking for characters... (tracked: 0)` — molesto.
Fix: throttlear MUCHO ese mensaje (o quitarlo del HUD; dejarlo solo en el log de archivo). Ligado al fix del tracking (#1) — si el tracker encuentra personajes, deja de spamear; pero igualmente quitar el spam del HUD.

## 3. 🔇 Cliente intenta conectar al master server (162.248.94.149) en el HUD
En el log del cliente aparece `[NET] Connecting to 162.248.94.149:27800`. Es el CLIENTE (distinto del server, que ya silenciamos). Silenciar/condicionar esa conexión del cliente al master en local.

## 4. 🏳️ Selector de facciones — parte CLIENTE (Zero lo quiere en la pantalla de conexión)
Ver `audit-05-selector-faccion-core-pendiente.md`. Que en la pantalla de CONNECT (IP/Port/Player Name) salga la ELECCIÓN de facción. + `shared_save_sync.cpp` debe reconocer las 6 facciones (derivar nombre del slot que llega en el paquete). + paquete C2S_FactionRequest.

## 5. 🐛 Bugs de threading (audit-01) — estabilidad/crashes
(1) OnLoadingGapDetected core.cpp:1287 — UAF al cargar partida. (2) Reset()/disconnect sync_orchestrator.cpp:328 — UAF en reconexión. (3) dedup OnGameTick core.cpp:2269 — candado no atómico. Fix: WaitForFrameWork antes de limpiar + atómica en el tick.

## 6. ✅ Offsets corregidos (audit-02) — HECHO parcial (2026-06-18 04:07)
- ✅ `inventory.owner` +0x28→+0x88 (KenshiLib confirma). `game_types.h:151`, `inventory_hooks.cpp:92`, `offsets.json`.
- ✅ `faction.id` +0x08 era `_antiSlavery` (Faction NO tiene id plano) → puesto a `-1` (los 8 call-sites tienen fallback graceful; envía 0 en vez de basura). `game_types.h:167`. **Ligado al "huyen": los ids basura colisionaban en `C2S_FactionRelation`.** Neutralizado, pero la resolución de relaciones queda inactiva hasta el rediseño del matching por nombre (→ #4).
- 🟡 PENDIENTE (no cambiado a ciegas): health chain `+0x2B8→+0x5F8→+0x40` da FAIL porque `+0x2B8` es `_myMemory`, no salud. Ruta probable 1.0.68: `medical@Character+0x458 → leftLeg/.../@+0x80 → flesh@+0x40` (HealthPartStatus = obj 0x68B, no array). Verificar con CE en vivo. Rotation `+0x58` funciona, no tocar.

## 7. 🏳️ Matching de facciones por nombre (parte del #4, resuelve el "huyen" de relaciones)
El "huyen" tiene 2 capas posibles: (a) relaciones de facción mal por ids basura — neutralizado en #6, falta el matching por nombre/GameData en el protocolo; (b) el AI tick (combate-v2, en verificación). El rediseño del matching va con el selector de facciones (#4). Puede merecer un `architect` por el cambio de protocolo.
