# Mapa de Arquitectura — Kenshi Co-op (índice maestro)

> Generado 2026-06-18 por mapeo paralelo (8 agentes RE) del proyecto completo.
> Cada mapa documenta un módulo a fondo. Empieza aquí para navegar.

## Los 8 mapas

| # | Archivo | Módulo |
|---|---|---|
| 01 | [[01-core-lifecycle]] | `KenshiMP.Core/core.cpp` — ciclo de vida, OnGameTick (Steps), fix facción host, flujo de entidades |
| 02 | [[02-hooks]] | `hooks/` — los 18 hooks (qué hookean, activos/bypassed/desactivados) |
| 03 | [[03-scanner]] | `KenshiMP.Scanner/` — resolución de RVAs (orchestrator, patterns, MovRaxRsp) |
| 04 | [[04-sync]] | `sync/` — tick de sincronización, interpolación, spawn pipeline |
| 05 | [[05-network-protocol]] | `net/` + `Common/` — protocolo ENet, ~70 paquetes, TimeSync |
| 06 | [[06-game-offsets]] | `game/` + offsets del binario 1.0.68 (GameWorld, Character, Faction) |
| 07 | [[07-server]] | `KenshiMP.Server/` — servidor, lobby, world save, comandos |
| 08 | [[08-mod-data]] | `kenshi-online.mod` + ModGen — game starts, Player 1-16, facciones |

## 🔑 Hallazgos clave para el bug del combate (consolidado)

1. **GameWorld 1.0.68 = instancia embebida en `.data` (RVA `0x2134110`), NO un puntero.** Su 1er qword es la vtable (`+0x1722608`). Resuelto. Campos: player `+0x580`, gameSpeed `+0x700`, paused `+0x8B9`.
2. **Facción del jugador:** `GameWorld+0x580 (player) → PlayerInterface+0x2A0 (participant) = Faction*`. Da 'Sinnombre'. ✅
3. **Fix facción del host (APLICADO):** `FixCharacterFactionTo` escribe la player faction en `char+0x10` del personaje del host → el `/verify` ya pasa faction/name → "combate habilitado". Era necesario pero NO suficiente.
4. **Pausa fantasma (EN CURSO):** runtime confirma `paused(+0x8B9)=0, gameSpeed(+0x700)=1.0` (sim corre) PERO la UI dice "PAUSADO" y bloquea órdenes (atacar/hablar). Hay un estado de pausa de UI/input separado.
5. **`TimeUpdate` (0x214B50) está MUERTO en Steam** (el juego no la llama; OnGameTick lo dirige el hook de `Present`). La velocidad se impone por **escritura directa a `timeManager+0x10`** (o gameSpeed +0x700). Clave para la feature de velocidad autoritativa.
6. **La facción real NO se sincroniza** — el mod la parchea localmente con la del host (sync/04). Posible factor en "enemigos huyen".
7. **Solo 2 facciones (`10-`/`12-kenshi-online.mod`) para 16 jugadores** (server asigna por `slot % 2`). Sospechoso para PvP/relaciones a escala.
8. **RVA de `StartAttack` dudoso** (0x7B2A20 "Cutting damage" vs 0x7A1650 real) → el hook de diagnóstico de ataque está en la función equivocada (por eso no registraba).
9. **`tracked:0` / `CharacterCreate:0`:** apuntan a RVAs no resueltas por el scanner (`CharAnimUpdate`/`CharacterSpawn`) o jugador lejos de pueblos. Verificar logs de instalación de hooks.
10. **`ApplyDamage` (0x7A33A0) desactivado** por crash MovRaxRsp (slots globales, no TLS). No necesario para el combate del host (el motor aplica daño solo).

## Estado del proyecto (2026-06-18)
- ✅ RE de facción resuelto (RVA 1.0.68, GameWorld embebido, fix de char+0x10).
- ✅ Servidor autoritativo de velocidad (comandos `speed`/`pause`/`resume`).
- 🔧 EN CURSO: pausa fantasma de UI/input del cliente (desbloquear órdenes).
- ⏳ Pendiente: que el combate funcione end-to-end, sync de IA remota (Fase 4), PvP/facciones a escala.
