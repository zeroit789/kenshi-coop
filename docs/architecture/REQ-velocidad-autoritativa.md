# REQUISITO — Velocidad/pausa 100% autoritativa del servidor

> Requisito explícito de Zero (2026-06-18). Esto es OBLIGATORIO, no opcional.
> Conecta con el fix de la "pausa fantasma" del cliente.

## Comportamiento exigido

1. **El SERVIDOR es la ÚNICA autoridad de velocidad y pausa.** Se controla SIEMPRE desde la **consola del servidor** (`speed <valor>`, `pause`, `resume`). Ya implementado en `KenshiMP.Server` (server.cpp).

2. **El CLIENTE NO puede cambiar la velocidad ni pausar localmente.** Aunque el usuario pulse:
   - **Pausa** (barra espaciadora / botón de pausa) → NO debe pasar nada.
   - **Rápido** / **Muy rápido** (botones de velocidad del HUD) → NO deben funcionar.
   - El juego del cliente **siempre va a la velocidad que dicta el servidor**, ni más ni menos.

3. **NO debe aparecer el cartel "PAUSADO"** en el cliente. Ni siquiera si el usuario intenta pausar. La capa de UI/input NO debe quedar en estado pausado (esto es justo la "pausa fantasma" que rompe el combate/diálogo).

## Implicación técnica (para el fix)

- El cliente debe **aplicar la velocidad del server** cada frame: el `TimeSync` del server llega con `gameSpeed` → escribir directo a `timeManager+0x10` (o `GameWorld+0x700`) y forzar `paused(+0x8B9)=0` cada tick, usando la resolución correcta del GameWorld embebido (no el deref ciego — ese bug ya está corregido).
- **Interceptar/anular los inputs locales de pausa y cambio de velocidad** del cliente (las teclas/botones que el usuario pulsa). Candidato: el WndProc/input hooks del mod (render_hooks) — la "puerta modal pegada" que detectó el agente del plan de combate. Hay que asegurarse de que esos inputs no lleguen al sistema de pausa del juego, o de revertir inmediatamente el estado de pausa que generen.
- **Ocultar/suprimir el indicador "PAUSED"** del HUD del cliente (o evitar que el estado que lo dispara se active).
- **Coherencia con el server:** la velocidad efectiva del cliente = la del último `TimeSync` (0 si el server hizo `pause`). Si el server pausa, el cliente sí se pausa (es autoritativo); si el server va a 1x/2x/5x, el cliente va a eso, sin que el usuario lo pueda alterar.

## Estado
🔧 EN CURSO — el agente de la "pausa fantasma" del cliente debe cumplir TODO esto. Verificar tras su fix que: (a) el usuario no puede pausar ni cambiar velocidad, (b) no sale "PAUSADO", (c) el cliente sigue exactamente la velocidad del server.
