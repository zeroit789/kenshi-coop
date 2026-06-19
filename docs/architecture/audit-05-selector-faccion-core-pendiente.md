# Audit 05 — Selector de facciones: lo que falta en KenshiMP.Core (cliente)

> Documento de hand-off para el agente que trabaja en `KenshiMP.Core`.
> La parte de SERVIDOR + DATOS ya está implementada y compila (ver "Estado del server" abajo).
> Esta nota describe EXACTAMENTE qué debe cambiar el cliente para que el selector de
> facciones funcione de punta a punta (single / teams / per-player con hasta 6 facciones).
> Fecha: 2026-06-18. Rutas absolutas. NO se ha editado Core (otro agente lo hace).

---

## 0. Contexto rápido

El mod regenerado `kenshi-online-16.mod` ahora define **6 facciones de jugador**:

| Slot (0-based) | Slot (1-based, comando) | Nombre FCS | StringId         |
|----------------|-------------------------|------------|------------------|
| 0              | 1                       | Player 1   | `10-kenshi-online.mod` |
| 1              | 2                       | Player 2   | `12-kenshi-online.mod` |
| 2              | 3                       | Player 3   | `45-kenshi-online.mod` |
| 3              | 4                       | Player 4   | `46-kenshi-online.mod` |
| 4              | 5                       | Player 5   | `47-kenshi-online.mod` |
| 5              | 6                       | Player 6   | `48-kenshi-online.mod` |

Esta tabla es el **manifiesto** `faction-slots.json` (generado por ModGen, leído por el server).
Las 6 facciones tienen `fundamental type = 4` (OT_CIVILIAN, igual que la facción vanilla
"Nameless"), por lo que el aggro/combate funciona igual que para Player 1/2.

El server ya envía al cliente el StringId correcto según el modo de facciones. El cliente
HOY solo entiende `10-` y `12-`; por eso, en `per-player`/`teams` con jugador en slot 3..6,
el cliente loguea **"Unknown faction"** y no asigna personajes.

---

## 1. Qué debe reconocer el cliente (StringIds del manifiesto)

**Archivo:** `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\game\shared_save_sync.cpp`

Hoy (líneas 69-81) el mapeo es hardcodeado a 2 facciones:

```cpp
static std::string FactionToOwnName(const std::string& faction) {
    const std::string f = StripModSuffix(faction);
    if (f == "10-kenshi-online") return "Player 1";
    if (f == "12-kenshi-online") return "Player 2";
    return "";
}
static std::string FactionToOtherName(const std::string& faction) { /* inverso */ }
```

### Problema de fondo del modelo "Own/Other"
El diseño actual asume **exactamente 2 jugadores**: tu facción y "la otra". Con 6 facciones
y hasta 16 jugadores ese modelo binario ya no sirve. `FactionToOtherName` no tiene sentido
cuando hay 3+ facciones. Hay dos caminos:

**Camino A (mínimo, desbloquea per-player de 6 jugadores ya):**
Reemplazar el mapeo por una tabla que cubra los 6 StringIds del manifiesto, devolviendo el
nombre de personaje propio. Para "Player N" el patrón es directo:

```cpp
// Mapea StringId de facción -> nombre del Character a controlar.
// Debe coincidir con faction-slots.json y con los Character "Player N" del .mod.
static const std::unordered_map<std::string, std::string> kFactionToChar = {
    {"10-kenshi-online", "Player 1"},
    {"12-kenshi-online", "Player 2"},
    {"45-kenshi-online", "Player 3"},
    {"46-kenshi-online", "Player 4"},
    {"47-kenshi-online", "Player 5"},
    {"48-kenshi-online", "Player 6"},
};
static std::string FactionToOwnName(const std::string& faction) {
    auto it = kFactionToChar.find(StripModSuffix(faction));
    return it != kFactionToChar.end() ? it->second : "";
}
```

> IMPORTANTE: el cliente NO debe seguir hardcodeando estos StringIds a mano si se puede
> evitar. Lo ideal es **leer el mismo `faction-slots.json`** que el server (el archivo se
> despliega en la carpeta del juego). Así, si ModGen genera más facciones, no hay que tocar
> Core. Si se prefiere no tocar disco desde el cliente, mantener la tabla anterior pero
> dejar un comentario apuntando a `faction-slots.json` como fuente de verdad.

**Camino B (correcto a largo plazo):** eliminar el concepto "Other" y descubrir a TODOS los
personajes remotos por su entidad de red (no por nombre de facción). El sync de posiciones
ya usa `EntityRegistry`/`ownerId`; la facción solo debería usarse para decidir **qué
personaje controla el jugador local** (Own), no para enumerar a los demás. Recomendado
abrir un seguimiento aparte para esto; no es bloqueante para `per-player`.

### El slot ya viene en el paquete — úsalo
El paquete S2C incluye el `slot` (0-based) además del string. El cliente puede usar el slot
directamente: `s_ownCharName = "Player " + std::to_string(slot + 1);` — eso elimina TODA la
tabla y funciona para cualquier nº de facciones, siempre que los Character del .mod sigan el
patrón "Player N". Es la opción más limpia. (Ver §2 sobre de dónde sacar el slot.)

---

## 2. Cómo lee el cliente su facción asignada (paquete S2C ya existe)

El flujo ya está cableado y NO hay que crear paquete nuevo para la asignación servidor→cliente:

- **Paquete:** `MessageType::S2C_FactionAssignment`.
- **Formato (lo escribe el server en `SendFactionAssignment`):**
  `[header] [u16 strLen] [raw factionString] [i32 slot]`  (slot 0-based).
- **Handler del cliente:** `KenshiMP.Core\net\packet_handler.cpp:1828` `HandleFactionAssignment`.
  Ya lee `strLen`, el string y el `slot`, y llama a
  `LobbyManager::OnFactionAssigned(factionStr, slot)` (`lobby_manager.cpp:10`).
- **Persistencia en cliente:** `LobbyManager::m_factionString` + `m_hasAssignment`.
  `GetFactionString()` lo expone a `shared_save_sync`.
- **Parche en memoria:** `LobbyManager::ApplyFactionPatch()` (`lobby_manager.cpp:90`) sobrescribe
  el string `204-gamedata.base` en `.rdata` con `m_factionString` (hasta 24 bytes). Esto mete
  al host en la facción "Player N" al cargar el save. **Ya soporta strings de 24 bytes**, así
  que `45-kenshi-online.mod` (20 chars) entra sin problema. No hay que tocar esta función.

### Cambios necesarios en el cliente para la RE-asignación en caliente
Novedad del server: ahora puede **reasignar** la facción de un jugador ya conectado
(comandos `setfaction` y `factionmode`). El server reenvía `S2C_FactionAssignment` en
cualquier momento. Hoy `HandleFactionAssignment` solo aplica el parche si la fase es
`MainMenu`/`GameReady` (antes de cargar el save):

```cpp
// packet_handler.cpp:1846-1852
if (core.GetClientPhase() == ClientPhase::MainMenu ||
    core.GetClientPhase() == ClientPhase::GameReady) {
    if (core.GetLobbyManager().ApplyFactionPatch()) { ... }
}
```

- Si la reasignación llega **antes** de cargar la partida → funciona ya (re-parchea el string).
- Si llega **en partida** (`InGame`) → el cliente debería, como mínimo, actualizar
  `s_ownCharName` en `shared_save_sync` y re-ejecutar `Init()` para redescubrir su personaje.
  Cambiar la facción de personajes ya instanciados en caliente es complejo (el motor no
  relee `.rdata`); como primera versión, basta con **avisar por HUD** ("Tu facción cambia al
  reconectar/recargar") y aplicar el cambio en el próximo `Init()`. Documentar la limitación.

**Resumen del lado lectura:** el cliente YA recibe y guarda su facción correctamente. Lo único
que falta de verdad para `per-player`/`teams` es el **mapeo de §1** (reconocer 45-/46-/47-/48-).

---

## 3. Pantalla / comando de ELECCIÓN del jugador (C2S — paquete nuevo a crear)

Hoy la facción es 100% impuesta por el server. Para que el JUGADOR elija su facción hace
falta un paquete cliente→servidor que HOY NO EXISTE. Diseño propuesto:

### 3.1 Nuevo MessageType
**Archivo:** `E:\Aplicaciones\Kenshi-Online\KenshiMP.Common\include\kmp\protocol.h` (o `messages.h`).
Añadir un tipo `C2S_FactionRequest` (la enum de mensajes está compartida entre server y cliente,
así que el server también lo verá). Formato sugerido (simétrico al S2C):

```
C2S_FactionRequest:  [header] [i32 requestedSlot]   // slot 1-based o 0-based: elegir UNA convención y documentarla
```

> Recomiendo **0-based** en el paquete para alinear con el `slot` del S2C, y que la UI muestre
> 1-based al usuario. El server ya trabaja en 0-based internamente (`SendFactionAssignment`).

### 3.2 Handler en el server (cuando se implemente el cliente)
El server ya tiene toda la maquinaria: `SetPlayerFaction(PlayerID, slot1Based)` valida rango y
reenvía el S2C. El handler nuevo solo tendría que:
1. Leer `requestedSlot` del reader.
2. Decidir política: ¿se permite que el jugador elija libremente, o solo en `factionMode` ==
   `per-player`? (En `single`/`teams` probablemente se ignora o se rechaza con mensaje.)
3. Llamar a `SetPlayerFaction(player.id, slot)` (convirtiendo a 1-based si el paquete es 0-based).
   Eso ya notifica al cliente vía S2C. Opcionalmente comprobar colisiones (dos jugadores misma
   facción) según se quiera permitir o no.

> El server NO incluye este handler todavía a propósito: el paquete C2S aún no existe y lo
> define el lado cliente. Cuando se cree el MessageType, añadir el `case` en el dispatcher de
> `server.cpp` (junto a `C2S_FactionRelation`, ~línea 437) llamando a `SetPlayerFaction`.

### 3.3 UI de elección
Opciones, de menor a mayor esfuerzo:
- **Comando de consola/overlay del cliente** (más rápido): un comando tipo `/faction <1-6>` en
  el overlay de KenshiMP que envíe `C2S_FactionRequest`. Reusa la infra de input del overlay
  existente (ver `KenshiMP.Core\ui\`).
- **Pantalla de lobby**: un selector en la UI de lobby/conexión (MyGUI) antes de "Listo".
  Mostrar las 6 facciones (leídas del manifiesto que el server puede enviar, o hardcodeadas
  Player 1..6) y enviar la elegida. Más trabajo de UI (MyGUI), pero es la experiencia buena.

El server confirma la elección reenviando `S2C_FactionAssignment`, que el cliente ya sabe
procesar (§2). El bucle se cierra solo.

---

## 4. Checklist mínimo para desbloquear `per-player` (6 jugadores)

1. [ ] `shared_save_sync.cpp`: ampliar `FactionToOwnName` a los 6 StringIds **o** (mejor)
       derivar el nombre del `slot` que ya llega en el paquete (`"Player " + (slot+1)`).
2. [ ] Replantear/retirar `FactionToOtherName` (modelo binario "own/other" roto con 3+ facciones).
3. [ ] (Opcional, recomendado) Leer `faction-slots.json` en cliente en vez de hardcodear.
4. [ ] (Opcional) Manejar reasignación en caliente en `HandleFactionAssignment` (HUD + re-Init).

Para la ELECCIÓN por el jugador (fase posterior):
5. [ ] Definir `C2S_FactionRequest` en `protocol.h`/`messages.h` (enum compartida).
6. [ ] Añadir el `case` en el dispatcher de `server.cpp` → `SetPlayerFaction`.
7. [ ] UI/comando de elección en el cliente que envíe el paquete.

---

## Estado del server + datos (YA HECHO — referencia)

- **`.mod` regenerado:** `E:\Aplicaciones\Kenshi-Online\kenshi-online-16.mod` →
  6 facciones (Player 1..6), 16 personajes, game start "Multiplayer" intacto.
  Manifiesto: `E:\Aplicaciones\Kenshi-Online\faction-slots.json` (6 slots).
  **No desplegado a Steam aún** (se despliega junto con el fix de Core).
- **Server:** lee el manifiesto (`LoadFactionSlots`), asigna slot según `factionMode`
  (`ComputeFactionSlot`), envía S2C con `SendFactionAssignment`. Comandos de consola:
  `factionmode <single|teams|per-player>`, `setfaction <playerId> <slot1-6>`, `factions`.
  Persiste `factionMode` en `server.json`.
- **Binario:** `E:\Aplicaciones\Kenshi-Online\build\bin\Release\KenshiMP.Server.exe` (compila OK).
