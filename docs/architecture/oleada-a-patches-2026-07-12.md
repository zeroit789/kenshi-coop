# Oleada A — Patches exactos para aplicar (sesión 11/12-jul-2026)

Documento de referencia para APLICAR, no para investigar de nuevo. Toda la investigación (RVAs, AOBs,
causas raíz) ya está confirmada con evidencia real — ver
`C:\Users\Zero\Documents\Claude-Memory-Wiki\wiki\proyectos\KENSHI\Sesion-2026-07-11-combate-spawn-investigacion-fable.md`
si hace falta contexto adicional, pero NO repetir la investigación.

**Regla de oro para quien aplique esto:** antes de cada Edit, releer el bloque real del fichero actual
(los números de línea citados pueden tener drift de ±unas pocas líneas desde que se escribió este
documento — el contenido/snippet "antes" es la referencia fiable, no el número de línea).

**Orden de aplicación obligatorio:** Paso 1 → Paso 2 → Paso 3 → Paso 4. Dentro de `packet_handler.cpp`
(tocado por el Paso 3 y el Paso 4), aplicar primero las ediciones del Paso 3 (mover a CommandQueue +
resolución por ID) y DESPUÉS las del Paso 4 (corregir la cadena de offsets de salud dentro de esas
mismas lambdas ya migradas).

Tras aplicar TODO: compilar (`cmake --build build --config Release` desde
`E:\Aplicaciones\Kenshi-Online`) y confirmar 0 errores. Dejar warnings nuevos anotados si aparecen.

---

## PASO 1 — Fix de hostilidad (`KenshiMP.Core\core.cpp`)

### Cambio (b) — CRÍTICO: refrescar el caché del host cada tick, sin condición

Buscar el bloque dentro de `OnGameTick` que instala el hook de combate y resuelve Nameless (cerca de
donde se llama `InstallCombatGateHookOnce` y `ResolveNamelessFactionOnce`, dentro de `if (m_gameLoaded)`).

`old_string` (bloque a localizar, contenido de referencia):
```cpp
                if (m_gameLoaded) {
                    uintptr_t hookModBase = Memory::GetModuleBase();
                    if (hookModBase != 0) {
                        InstallCombatGateHookOnce(hookModBase);   // instala el detour una vez
                        ResolveNamelessFactionOnce(hookModBase);  // localiza Nameless (Opción A) o marca C
                    }
                }
```
`new_string`:
```cpp
                if (m_gameLoaded) {
                    uintptr_t hookModBase = Memory::GetModuleBase();
                    if (hookModBase != 0) {
                        InstallCombatGateHookOnce(hookModBase);   // instala el detour una vez
                        ResolveNamelessFactionOnce(hookModBase);  // localiza Nameless (Opción A) o marca C
                    }

                    // (b) CAMBIO CRÍTICO: refrescar el cache del host para el hook CADA tick, SIN
                    // condición. Antes solo se refrescaba dentro del bloque con tope de 600 reintentos:
                    // durante la carga OnGameTick corre a ~230 ticks/s y el tope se agotaba en ~2.6s,
                    // ANTES de que la facción del host se resolviera (~7s) -> ABORT permanente ->
                    // s_cachedHostFR=0 para siempre -> Hook_CombatGate quedaba inerte. También cubre el
                    // caso de éxito: si SetControlledChar resetea Faction+0x78 DESPUÉS de que el fix
                    // marque done, aquí el cache se re-sincroniza igualmente.
                    uintptr_t cacheFaction = game::GetPlayerFactionDirect();          // Opción B
                    if (cacheFaction == 0) {
                        void* pc = m_playerController.GetPrimaryCharacter();          // Opción A
                        if (pc != nullptr) {
                            const int facOff = game::GetOffsets().character.faction;  // +0x10
                            uintptr_t fac = 0;
                            if (Memory::Read(reinterpret_cast<uintptr_t>(pc) + facOff, fac))
                                cacheFaction = fac;
                        }
                    }
                    RefreshHostFactionCache(cacheFaction);  // no-op interno si cacheFaction==0
                }
```
(Confirmar que `m_playerController` y `game::GetPlayerFactionDirect()` existen tal cual — ya se usan
con el mismo patrón unas líneas más abajo en el bloque FIX-HOSTILITY. `RefreshHostFactionCache` ya
valida internamente con `GateIsHeap` antes de escribir el atomic, así que la llamada es segura incluso
si `cacheFaction` resulta inválido.)

### Cambio (a) — no quemar reintentos mientras la facción no exista

Localizar el bloque `FIX-HOSTILITY` dentro de `OnGameTick` (busca `s_hostilityFixTries`,
`kMaxHostilityFixTries = 600`).

**Edit a.1** — donde se incrementa `s_hostilityFixTries` justo tras el guard `!s_hostilityFixDone`:
`old_string` (referencia):
```cpp
                if (!s_hostilityFixDone.load(std::memory_order_acquire) && m_gameLoaded) {
                    s_hostilityFixTries++;
```
`new_string`:
```cpp
                if (!s_hostilityFixDone.load(std::memory_order_acquire) && m_gameLoaded) {
                    // (a) el incremento de s_hostilityFixTries se movió DENTRO de la rama
                    // hostFaction != 0: esperar a que la facción exista NO consume el tope.
```

**Edit a.2** — dentro de la rama `if (hostFaction != 0) {`:
`old_string` (referencia):
```cpp
                    if (hostFaction != 0) {
                        const auto& off  = game::GetOffsets().faction;
```
`new_string`:
```cpp
                    if (hostFaction != 0) {
                        s_hostilityFixTries++;  // (a) solo cuentan intentos REALES (facción resuelta)
                        const auto& off  = game::GetOffsets().faction;
```

**Edit a.3** — la rama `else` de "facción del host aún no resuelta":
`old_string` (referencia):
```cpp
                    } else {
                        // Facción del host aún no resuelta (carga a medias / cadena GW+0x580 no lista).
                        s_hostilityConfirms = 0;
                        if (s_hostilityFixTries <= 10 || s_hostilityFixTries % 120 == 0) {
                            spdlog::info(
                                "[FIX-HOSTILITY] facción del host no resuelta aún "
                                "(GetPlayerFactionDirect=0 y char+0x10 no legible) — reintento #{}",
                                s_hostilityFixTries);
                        }
                    }
```
`new_string`:
```cpp
                    } else {
                        // Facción del host aún no resuelta (carga a medias / cadena GW+0x580 no lista).
                        // (a) NO quemamos el tope: durante la carga van ~230 ticks/s y los 600
                        // intentos se agotaban en ~2.6s, antes de los ~7s que tarda la facción.
                        s_hostilityConfirms = 0;
                        static int s_hostilityWaitTicks = 0;
                        ++s_hostilityWaitTicks;
                        if (s_hostilityWaitTicks <= 10 || s_hostilityWaitTicks % 600 == 0) {
                            spdlog::info(
                                "[FIX-HOSTILITY] facción del host no resuelta aún "
                                "(GetPlayerFactionDirect=0 y char+0x10 no legible) — esperando "
                                "(tick de espera #{}, intentos reales consumidos={})",
                                s_hostilityWaitTicks, s_hostilityFixTries);
                        }
                    }
```
Con esto, el ABORT permanente solo puede saltar tras 600 ticks CON la facción ya resuelta pero el FR no
aplicable — no hace falta tocar el bloque de abort en sí.

### Cambio (c) — log throttled cuando `ResolveNamelessFactionOnce` da n=0

Localizar `ResolveNamelessFactionOnce`, el `return` tras `SEH_CollectFactionGameDatas` con `n==0`.
`old_string` (referencia):
```cpp
    int n = 0;
    SEH_CollectFactionGameDatas(factionMgr, facs, gds, &n, kMax);
    if (n == 0) return;  // FactionManager aún no poblado → reintentar en el próximo tick
```
`new_string`:
```cpp
    int n = 0;
    SEH_CollectFactionGameDatas(factionMgr, facs, gds, &n, kMax);
    if (n == 0) {
        // (c) Log THROTTLED: antes este retorno era 100% silencioso.
        static int s_namelessEmptyTicks = 0;
        ++s_namelessEmptyTicks;
        if (s_namelessEmptyTicks == 1 || s_namelessEmptyTicks % 1800 == 0) {
            spdlog::info("[FIX-HOSTILITY-HOOK] ResolveNamelessFactionOnce: FactionManager "
                         "(modBase+0x21345B8) devuelve n=0 facciones — aún no poblado, "
                         "reintentando cada tick (tick #{})", s_namelessEmptyTicks);
        }
        return;  // FactionManager aún no poblado → reintentar en el próximo tick
    }
```

**Verificación de polaridad (NO cambiar nada):** la polaridad de `isAlly` en `Hook_CombatGate`/
`SEH_DecideCombatGate` ya está confirmada correcta (whitelist propia → ALIADO bloquea; bandido/
esclavista → NO-aliado ataca). No tocar esa lógica.

---

## PASO 2 — Fix MURO 1 (`KenshiMP.Core\game\spawn_manager.cpp`)

**Causa confirmada:** los checks #4/#4b de la lambda `isPlausibleFactory` (cerca de `CaptureFactoryFromGameWorld`,
función que valida candidatos a puntero de factory) asumen que un puntero de heap x64 nunca tiene los
32 bits altos a cero. El heap propio de Kenshi en esta máquina SÍ vive bajo 4GB — por eso el factory
real (que SÍ se puebla en runtime en `moduleBase+0x21345B0`) se rechaza siempre (2 millones de
rechazos, 0 aciertos en los logs de la sesión de investigación).

**Cambios a aplicar (localizar `isPlausibleFactory` y leer los ~6 checks numerados que contiene):**
1. **Eliminar check#4 y check#4b** (los que rechazan por "32 bits altos == 0" / exigen
   `p >= 0x10000000000`). El check#1 (`p > 0x10000`) ya cubre el rango bajo razonable.
2. **Hacer OBLIGATORIO el check de vtable** (probablemente el check#6, hoy "defensivo" — no rechaza si
   la lectura falla): exigir que la primera qword del candidato (la vtable) esté en el rango de
   `.rdata` del módulo, Y que sus 2-3 primeras entradas apunten a `.text` del módulo. Esto es lo que de
   verdad distingue un objeto C++ real de basura de heap o de un float reinterpretado como puntero
   (el caso `0x3F000000` ya lo mata este check porque `Memory::Read(0x3F000000)` falla, memoria no
   mapeada).
3. **Mantener** el check que rechaza si el puntero cae DENTRO del propio módulo (el slot contiene un
   puntero de heap que cambia entre sesiones, no debe apuntar al binario).
4. **Al aceptar un candidato**, loguear `vt - moduleBase` (RVA de la vtable) con `spdlog::info` — sirve
   para pinnear la vtable real de `RootObjectFactory` en Steam 1.0.68 en la próxima sesión.
5. **Throttle de la inundación de logs**: buscar dónde se llama `CaptureFactoryFromGameWorld` dentro de
   `OnGameTick` en `core.cpp` — si hay una llamada SIN guard de intervalo (ejecutándose cada tick sin
   límite, ~160 veces/segundo), o bien quitarla si es redundante con un retry que sí tiene guard (cada
   ~150 ticks), o ponerle el mismo guard. El volcado de diagnóstico del slot (`DIAG-FACTORY-SLOT` o
   similar, si existe) debe ejecutarse UNA sola vez (`static bool`), no en cada intento.

**Reordenar `ScanGameDataHeap`** (misma clase, función que contiene "Estrategia 1" con offsets GOG
hardcodeados y "Estrategia 2" con `GameWorld+0x20`): hacer que la **Estrategia 2 se intente PRIMERO**
(ya funciona en builds Steam) y la Estrategia 1 (offsets GOG) quede como ÚLTIMO recurso, solo si la 2
falla. Además, **añadir un gate `IsGameLoaded()`** (o el helper equivalente ya usado en otras partes
del proyecto) antes de intentar la Estrategia 1 — sin este gate, en los primeros ticks de carga (antes
de que el mundo esté listo) la Estrategia 1 sigue disparando el aviso de excepción benigno
(`0x3F000000`, capturado por SEH mas genera ruido de log).

No hace falta ningún check nuevo de "mitad alta nula" — ese enfoque causaría el MISMO falso negativo
que el bug que se está arreglando.

---

## PASO 3 — Destroy hook + 11 handlers de red + robustez CommandQueue

### 3.1 `KenshiMP.Core\hooks\entity_hooks.cpp` — conectar el destroy hook

**Contexto confirmado por RE (RTTI, alta confianza):** el patrón "CharacterDestroy" que resuelve el
scanner apunta a la función EQUIVOCADA (`NodeList::destroyNodesByBuilding`, firma de 2 args distinta).
El destructor REAL de `Character` se resuelve en RUNTIME vía `vtable[0]` (scalar deleting destructor
MSVC, firma `void*(void* this, unsigned int flags)`) — confirmado con doble ancla cruzada (otros slots
de la misma vtable coinciden con offsets ya conocidos del proyecto). **Importante:** `vtable[0]` en el
binario real NO apunta directo a la función — apunta a un THUNK de 5 bytes (`jmp`). Si al leer
`vtable[0]` el primer byte es `0xE9`, hay que SEGUIR el salto (destino = dirección+5+rel32) hasta la
función real (que tiene prólogo completo, ~10 bytes limpios) y hookear AHÍ — o, más robusto, hacer un
VMT-hook (reemplazar el puntero del slot 0 en la propia vtable con `VirtualProtect` temporal a RW, en
vez de un detour por bytes sobre un `jmp` de solo 5 bytes).

**1a. Firma del tipo** — localizar `using CharacterDestroyFn = ...`:
`old_string`:
```cpp
using CharacterDestroyFn = void(__fastcall*)(void* character);
```
`new_string`:
```cpp
// Destructor virtual MSVC (vtable slot 0): "scalar deleting destructor".
// Firma real: void*(this, flags). flags bit0 = liberar memoria tras destruir.
// OJO: NO confundir con funcs.CharacterDestroy — ese RVA resuelve a
// NodeList::destroyNodesByBuilding, función equivocada (confirmado en pattern-verification.md).
using CharacterDestroyFn = void*(__fastcall*)(void* character, unsigned int flags);
```

**1b. Forward declaration** — justo tras `static bool s_destroyHookInstalled = false;`:
`old_string`:
```cpp
// Whether CharacterDestroy hook is actually installed
static bool s_destroyHookInstalled = false;
```
`new_string`:
```cpp
// Whether CharacterDestroy hook is actually installed
static bool s_destroyHookInstalled = false;

// Instalación diferida del destroy hook (definida tras Hook_CharacterDestroy).
// Necesita un Character* VIVO para leer su vtable — por eso no puede ir en Install().
static void TryInstallDestroyHookFromCharacter(void* character);
```

**1c. Cuerpo del hook** — localizar `Hook_CharacterDestroy`:
`old_string` (apertura de la función):
```cpp
static void __fastcall Hook_CharacterDestroy(void* character) {
    int destroyNum = s_totalDestroys.fetch_add(1) + 1;

    uintptr_t charAddr = reinterpret_cast<uintptr_t>(character);
    if (charAddr < 0x10000 || charAddr > 0x00007FFFFFFFFFFF) {
        s_origDestroy(character);
        return;
    }
```
`new_string`:
```cpp
// Hook sobre el scalar deleting destructor de Character (resuelto en runtime vía vtable[0]).
// Corre en el hilo de juego (quien destruye es el motor). Toda la limpieza del registro va
// ANTES de llamar al original: después, la memoria del Character ya no existe.
static void* __fastcall Hook_CharacterDestroy(void* character, unsigned int flags) {
    int destroyNum = s_totalDestroys.fetch_add(1) + 1;

    uintptr_t charAddr = reinterpret_cast<uintptr_t>(character);
    if (charAddr < 0x10000 || charAddr > 0x00007FFFFFFFFFFF) {
        return s_origDestroy(character, flags);
    }
```
Y el cierre de la función:
`old_string`:
```cpp
    s_origDestroy(character);
}
```
`new_string`:
```cpp
    // Destruir de verdad AL FINAL — la limpieza de arriba necesita el puntero vivo.
    return s_origDestroy(character, flags);
}
```
(El cuerpo intermedio — envío de despawn, DecrementSpawnCount, RemoveEntity, Unregister — queda
intacto, ya corre en el hilo correcto.)

**1d. Nueva función de instalación diferida** — insertar justo tras el cierre del hook:
```cpp
// ── Instalación diferida del destroy hook vía vtable slot 0 ──
// El patrón "CharacterDestroy" del scanner resuelve a la función equivocada. El RVA del
// destructor real de Character se resuelve leyendo vtable[0] del primer Character vivo,
// siguiendo el thunk de 5 bytes si aplica, y validando con .pdata que es inicio de función real
// (mismo criterio que validateFactoryFunc en este mismo archivo).
static void TryInstallDestroyHookFromCharacter(void* character) {
    if (s_destroyHookInstalled || !character) return;

    uintptr_t charAddr = reinterpret_cast<uintptr_t>(character);
    if (charAddr < 0x10000 || charAddr > 0x00007FFFFFFFFFFF || (charAddr & 0x7) != 0) return;

    uintptr_t modBase = Memory::GetModuleBase();

    uintptr_t vtable = 0;
    if (!Memory::Read(charAddr, vtable)) return;
    if (vtable < modBase || vtable >= modBase + 0x4000000) return;

    uintptr_t dtorAddr = 0;
    if (!Memory::Read(vtable, dtorAddr)) return;
    if (dtorAddr < modBase || dtorAddr >= modBase + 0x4000000) return;

    // Seguir el thunk de 5 bytes (jmp rel32) si vtable[0] no es la función real directamente.
    uint8_t firstByte = 0;
    if (Memory::Read(dtorAddr, firstByte) && firstByte == 0xE9) {
        int32_t rel32 = 0;
        if (Memory::Read(dtorAddr + 1, rel32)) {
            uintptr_t target = dtorAddr + 5 + static_cast<intptr_t>(rel32);
            if (target >= modBase && target < modBase + 0x4000000) {
                dtorAddr = target;
            }
        }
    }

    // Validación .pdata: tiene que ser un INICIO de función real
    DWORD64 imageBase = 0;
    auto* rtFunc = RtlLookupFunctionEntry(static_cast<DWORD64>(dtorAddr), &imageBase, nullptr);
    if (!rtFunc || static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress != dtorAddr) {
        spdlog::warn("entity_hooks: destroy-hook — vtable[0]=0x{:X} no es inicio de función — NO instalado", dtorAddr);
        return;
    }

    if (!HookManager::Get().InstallAt("CharacterDestroy", dtorAddr,
                                      &Hook_CharacterDestroy, &s_origDestroy)) {
        spdlog::error("entity_hooks: destroy-hook — InstallAt FALLÓ en 0x{:X} (RVA 0x{:X})",
                      dtorAddr, dtorAddr - modBase);
        return;
    }
    HookManager::Get().Enable("CharacterDestroy");
    s_destroyHookInstalled = true;
    spdlog::info("entity_hooks: destroy-hook INSTALADO vía vtable[0] — dtor=0x{:X} (RVA 0x{:X})",
                 dtorAddr, dtorAddr - modBase);
}
```

**1e. Puntos de llamada** (dos, redundantes a propósito):

Sitio 1 — dentro de `Hook_CharacterCreate`, justo tras el check `if (!character) { ... return nullptr; }`:
añadir inmediatamente después:
```cpp
    // ── Instalación diferida del destroy hook (una sola vez) ──
    if (!s_destroyHookInstalled) {
        TryInstallDestroyHookFromCharacter(character);
    }
```

Sitio 2 — dentro de `ResumeForNetwork()`, tras el bloque que hace `HookManager::Get().Enable("CharacterCreate")`:
añadir:
```cpp
    // ── Instalación diferida del destroy hook con un mod template capturado ──
    if (!s_destroyHookInstalled) {
        void* tmpl[1] = {};
        if (GetCapturedModTemplates(tmpl, 1) > 0) {
            TryInstallDestroyHookFromCharacter(tmpl[0]);
        }
    }
```

**1f. `Install()`** — corregir el comentario junto a `s_destroyHookInstalled = false;`:
`old_string`:
```cpp
    // CharacterDestroy hook NOT installed
    s_destroyHookInstalled = false;
```
`new_string`:
```cpp
    // CharacterDestroy: NO se instala aquí a propósito. El patrón del scanner resuelve a
    // NodeList::destroyNodesByBuilding — función equivocada. El hook real se instala DIFERIDO vía
    // TryInstallDestroyHookFromCharacter() (vtable slot 0 del primer Character vivo, siguiendo el
    // thunk si aplica), llamado desde Hook_CharacterCreate (conectado) y ResumeForNetwork().
    s_destroyHookInstalled = false;
```

### 3.2 `KenshiMP.Core\net\packet_handler.cpp` — 11 handlers vía CommandQueue con resolución por ID

Patrón canónico a aplicar en TODOS: mover el trabajo que toca memoria del motor a
`core.GetCommandQueue().Push({[msg]() { ... }})`, capturando SOLO structs POD por valor, y
RESOLVIENDO el `Character*`/`void*` por ID (`core.GetEntityRegistry().GetGameObject(id)`) DENTRO de la
lambda — nunca capturar el puntero crudo desde fuera.

Aplicar este patrón a los handlers: `HandlePlayerLeft`, `HandleEntityDespawn`, `HandleStatUpdate`,
`HandleCombatKO`, `HandleHealthUpdate`, `HandleLimbHealth`, `HandleEquipmentUpdate`,
`HandleInventoryUpdate`, `HandleDoorState` (los 9 que hoy tocan memoria directamente sin cola), y
corregir la captura en `HandleCombatHit`/`HandleCombatDeath` (que ya usan CommandQueue pero capturan el
puntero POR VALOR desde fuera de la lambda en vez de resolverlo dentro — cambiar a resolver por ID
dentro).

**IMPORTANTE — orden con el Paso 4:** para `HandleHealthUpdate`, `HandleLimbHealth`, y los fallbacks de
salud dentro de `HandleCombatHit`/`HandleCombatDeath`/`HandleCombatKO`, aplicar aquí SOLO la
reestructuración (mover a CommandQueue + resolver por ID) manteniendo la lógica de escritura de salud
TAL CUAL esté hoy — el Paso 4 la reemplazará por la cadena de offsets correcta inmediatamente después,
dentro de la lambda ya migrada.

**Además — migrar los 5 sitios de CommandQueue que YA EXISTEN hoy** (no solo los 11 nuevos): grep por
`GetCommandQueue().Push` en `packet_handler.cpp` para localizarlos (aprox. líneas 697, 728, 995, 1069,
1960 — verificar con grep, no fiarse del número). Todos deben resolver por ID DENTRO de la lambda, no
capturar el puntero desde fuera. Esto es necesario porque hay una carrera real confirmada: en
desconexión, `SetConnected(false)` hace `m_commandQueue.Clear()` desde el hilo de UI mientras el hilo
de red puede estar a mitad de un `Push` — con punteros capturados por valor eso es un
use-after-free potencial; con resolución por ID dentro de la lambda, un registro ya vaciado
simplemente hace que `GetGameObject` devuelva null y el comando se salte limpio.

### 3.3 `KenshiMP.Core\game_command_queue.h` + `KenshiMP.Core\core.cpp:7153` — robustez del drenaje

**Problema confirmado:** `DrainAll` ejecuta cada comando (`cmd.execute()`) sin SEH individual — si una
lambda revienta, aborta el resto del tick entero (con `/EHsc`, el `__except` no llama destructores del
unwind, así que el vector local de `DrainAll` y sus `std::function` restantes se fugan). Además, el
parámetro `fn` que recibe `DrainAll` desde el caller se IGNORA — el bucle llama `cmd.execute()`
directamente en vez de usar `fn(cmd)`.

**Fix — en `core.cpp`, cerca de otros helpers `SEH_*` de este archivo, añadir:**
```cpp
// Ejecuta UN comando de la cola con SEH propio — un puntero malo en una lambda no debe
// tirar el resto de la ráfaga ni el resto del tick.
static bool SEH_RunGameCommand(kmp::GameCommand* cmd) {
    __try {
        cmd->execute();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
```
**En el caller de `DrainAll` (`core.cpp` ~7153, dentro de `OnGameTick` Step 1.5):**
`old_string` (referencia, adaptar al lambda real que hay ahí):
```cpp
        m_commandQueue.DrainAll([](GameCommand& cmd) {
            cmd.execute();
        });
```
`new_string`:
```cpp
        m_commandQueue.DrainAll([](GameCommand& cmd) {
            if (!SEH_RunGameCommand(&cmd)) {
                spdlog::warn("Core: comando de red descartado por excepción durante ejecución");
            }
        });
```
**En `game_command_queue.h`, dentro de `DrainAll` (~líneas 39-43), asegurar que el bucle usa el
parámetro `fn` recibido en vez de llamar `cmd.execute()` directamente:**
`old_string` (referencia):
```cpp
    for (auto& cmd : local) {
        cmd.execute();
    }
```
`new_string`:
```cpp
    for (auto& cmd : local) {
        if (cmd.execute) fn(cmd);   // usar el ejecutor pasado por el caller (con SEH), no llamar directo
    }
```
(Ajustar la firma/tipo de `fn` si `DrainAll` no lo tenía como parámetro — en ese caso, añadirlo como
`std::function<void(GameCommand&)> fn` con un valor por defecto que llame `cmd.execute()` directo, para
no romper otros callers si los hay.)

---

## PASO 4 — Fix cadena de salud (6 ficheros)

**Causa confirmada en bytes:** la cadena vieja `[char+0x2B8]→[+0x5F8]→+0x40` tiene un deref de más —
`char+0x2B8` es `CharacterMemory*` (una copia/caché), no donde el motor escribe salud real. El motor
escribe/lee salud vía `MedicalSystem`, que vive INLINE en `char+0x458` (no es puntero): su campo
`partArray` está en el offset absoluto `char+0x458+0x1A0 = char+0x5F8` (por ESO la cadena vieja
"coincidía" en el segundo salto — coincidencia numérica, no la misma ruta). Cadena real:
`partArray = [char+0x5F8]` (直接, sin el primer deref por `+0x2B8`) → `part_i = [partArray + i*8]` →
`flesh = [part_i + 0x40]`.

### 4.1 `KenshiMP.Core\game\game_types.h` — reemplazar el bloque de offsets de salud

`old_string` (localizar el bloque con `healthChain1`/`healthChain2`, buscar `[PENDIENTE 1.0.68]`):
```cpp
    int healthChain1 = 0x2B8;
    int healthChain2 = 0x5F8;
    int healthBase   = 0x40;
    int healthStride = 8;
```
(el bloque exacto puede tener más comentarios alrededor — reemplazar TODO el bloque de campos, no solo
los valores)
`new_string`:
```cpp
    // Cadena de salud CANÓNICA — verificada en BYTES sobre kenshi_x64.exe Steam 1.0.68:
    //   Character+0x458 = MedicalSystem INLINE (by-value, NO es puntero)
    //   partArray = *(void**) (char + 0x5F8)          // = medical(0x458) + 0x1A0 → HealthPartStatus*[]
    //   partCount = *(int*)   (char + 0x5F0)          // = medical(0x458) + 0x198
    //   part_i    = *(void**) (partArray + i*8)       // array de PUNTEROS, stride 8
    //   flesh     = *(float*) (part_i + 0x40)         // fleshStun@+0x44, _maxHealth@+0x54
    // La cadena vieja [char+0x2B8]→[+0x5F8]→+0x40 era INVÁLIDA: char+0x2B8 = CharacterMemory*
    // (_myMemory, una copia) — un deref de más. 0x458+0x1A0 = 0x5F8, por eso "coincidía".
    // El motor jamás escribe salud por esa ruta (confirmado: MedicalSystem::_setHealth 0x645EF0
    // usa [rcx+0x198]/[rcx+0x1A0]; MedicalSystem::applyDamage 0x64F300 escribe [part+0x40]/[+0x44]).
    int healthPartArray = 0x5F8;  // char → HealthPartStatus** (array de punteros)
    int healthPartCount = 0x5F0;  // char → int, nº de partes (humanos = 7)
    int healthBase      = 0x40;   // HealthPartStatus → flesh (float)
    int healthStride    = 8;      // stride DENTRO del array de punteros (sizeof(void*))
```
Tras este cambio, el compilador señalará todos los sitios que usaban `healthChain1`/`healthChain2` —
corregir cada uno (lista completa abajo, no hace falta "buscarlos", el build los delata).

### 4.2 `KenshiMP.Core\net\packet_handler.cpp` — `SEH_WriteLimbHealthToChar` + nuevo helper

Reemplazar el cuerpo completo de `SEH_WriteLimbHealthToChar` (la función que hoy hace el doble deref
viejo) por:
```cpp
static bool SEH_WriteLimbHealthToChar(void* character, const float health[7]) {
    __try {
        auto& offsets = game::GetOffsets().character;
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (offsets.healthPartArray < 0 || offsets.healthBase < 0) return false;
        uintptr_t partArray = 0;
        if (!Memory::Read(charPtr + offsets.healthPartArray, partArray) || partArray == 0)
            return false;
        int count = 0;
        if (!Memory::Read(charPtr + offsets.healthPartCount, count)) return false;
        if (count <= 0 || count > 32) return false;
        int n = (count < 7) ? count : 7;
        for (int i = 0; i < n; i++) {
            uintptr_t part = 0;
            if (!Memory::Read(partArray + i * offsets.healthStride, part) || part == 0)
                continue;
            Memory::Write(part + offsets.healthBase, health[i]);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Escribe la salud (flesh) de UNA sola parte por la cadena canónica. SEH-safe, solo POD.
static bool SEH_WriteOnePartHealth(void* character, int partIndex, float value) {
    __try {
        auto& offsets = game::GetOffsets().character;
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (offsets.healthPartArray < 0 || partIndex < 0) return false;
        int count = 0;
        if (!Memory::Read(charPtr + offsets.healthPartCount, count) || partIndex >= count)
            return false;
        uintptr_t partArray = 0;
        if (!Memory::Read(charPtr + offsets.healthPartArray, partArray) || partArray == 0)
            return false;
        uintptr_t part = 0;
        if (!Memory::Read(partArray + partIndex * offsets.healthStride, part) || part == 0)
            return false;
        return Memory::Write(part + offsets.healthBase, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
```

**Sitios que deben usar estos helpers en vez de escribir la cadena a mano** (dentro de las lambdas que
el Paso 3 ya migró a CommandQueue):
- `HandleCombatHit` (fallback de daño de UN body part): `SEH_WriteOnePartHealth(targetObj, static_cast<int>(msg.bodyPart), msg.resultHealth);`
- `HandleCombatDeath` (fallback: poner todas las partes a -100):
  ```cpp
  float deathHealth[7];
  for (int i = 0; i < 7; i++) deathHealth[i] = -100.f;
  SEH_WriteLimbHealthToChar(entityObj, deathHealth);
  ```
- `HandleCombatKO` (fallback, solo la parte 0/chest): `SEH_WriteOnePartHealth(entityObj, 0, msg.resultHealth);`
- `HandleHealthUpdate`: `SEH_WriteLimbHealthToChar(entityObj, msg.health);`
- `HandleLimbHealth`: `SEH_WriteLimbHealthToChar(entityObj, msg.health);`

(Todos estos sitios ya deben tener `targetObj`/`entityObj` resuelto por ID dentro de la lambda, gracias
al Paso 3 — aquí solo se sustituye la lógica INTERNA de escritura de salud.)

### 4.3 `KenshiMP.Core\core.cpp` — `SEH_WriteLimbHealthDirect`

Mismo cuerpo nuevo que 4.2 (`SEH_WriteLimbHealthToChar`) — es un duplicado literal por límites del
compilador (C2712). Localizar por el nombre de la función y aplicar la misma lógica de offsets nueva.

### 4.4 `KenshiMP.Core\game\game_character.cpp` — `CharacterAccessor::GetHealth` (lado LECTURA)

Localizar el "Method 2" dentro de `GetHealth` (la vía que usa la cadena de offsets, no la que usa
`offsets.health` directo/plano):
`new_string` (reemplazar la lógica de esa rama):
```cpp
    // Cadena canónica MedicalSystem (inline en char+0x458):
    // [char+0x5F8] = HealthPartStatus** → [array + part*8] → flesh @ +0x40
    if (offsets.healthPartArray >= 0 && offsets.healthBase >= 0) {
        uintptr_t partArray = 0;
        if (!Memory::Read(m_ptr + offsets.healthPartArray, partArray) || partArray == 0) return 0.f;
        uintptr_t partPtr = 0;
        if (!Memory::Read(partArray + static_cast<int>(part) * offsets.healthStride, partPtr) || partPtr == 0)
            return 0.f;
        float health = 0.f;
        Memory::Read(partPtr + offsets.healthBase, health);
        return health;
    }
```
Nota: si hay un "Method 1" anterior que usa un offset plano `offsets.health` con stride de floats
(nunca se activa porque su condición es siempre falsa), dejar un comentario indicando que ese modelo de
array-plano-de-floats no existe en 1.0.68, o eliminarlo si es código muerto confirmado.

### 4.5 `KenshiMP.Core\sdk\kenshi_sdk.cpp` — `KenshiSDK::WriteHealth`

Reemplazar el cuerpo por:
```cpp
bool KenshiSDK::WriteHealth(uintptr_t gamePtr, BodyPart part, float value) {
    if (gamePtr == 0) return false;
    auto& offsets = game::GetOffsets().character;
    uintptr_t partArray = 0;
    if (!Memory::Read(gamePtr + offsets.healthPartArray, partArray) || partArray == 0)
        return false;
    uintptr_t partPtr = 0;
    if (!Memory::Read(partArray + static_cast<int>(part) * offsets.healthStride, partPtr) || partPtr == 0)
        return false;
    return Memory::Write(partPtr + offsets.healthBase, value);
}
```

### 4.6 `KenshiMP.Core\sys\builtin_commands.cpp` — diagnósticos/`\`verify\``

El compilador señalará los usos de `healthChain1`/`healthChain2` en el volcado de offsets de debug y en
el comando `/verify` que recorre la cadena para comparar contra lo "descubierto". Actualizar esos
recorridos a la cadena nueva (`healthPartArray`/`healthPartCount`/`healthBase`/`healthStride`) — con
esto el `/verify` de salud debería pasar de FAIL a OK la próxima vez que se ejecute en vivo.

---

## Tras aplicar TODO

1. `cd E:\Aplicaciones\Kenshi-Online && cmake --build build --config Release` — confirmar 0 errores.
2. Si hay errores de compilación por offsets/nombres que no coincidan exactamente con este documento
   (drift esperable), corregir siguiendo la INTENCIÓN de cada patch (la cadena de offsets correcta, la
   resolución por ID, el SEH por comando), no revertir el cambio.
3. NO desplegar a la instalación real de Kenshi todavía ni tocar el `.mod` (eso es la Oleada B, aparte).
4. Reportar: qué se aplicó, qué build resultó, y cualquier desviación respecto a este documento (con
   justificación).
