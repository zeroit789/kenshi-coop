# Auditoría 08 — La "rama viva" del AI tick (0x5CD1C0) y el campo char+0xD8

> Auditoría de RE **estático** (desensamblado de bytes con iced-x86 + pefile). Fecha: 2026-06-18.
> Binario: `kenshi_x64.exe` v1.0.68 Steam (App 233860), MSVC x64, ImageBase `0x140000000`.
> Único cambio de código: **corrección del texto/comentarios del DIAG-THINK** en `core.cpp`
> (read-only, ya bajo SEH). **NO se tocó ninguna estructura del motor. NO se desplegó.**
> Compilación verificada: `KenshiMP.Core.dll` compila limpio en Release.

---

## 0. TL;DR (lo esencial)

1. **La hipótesis de la consigna queda REFUTADA por bytes.** `char+0xD8` **NO es un acumulador
   `+= dt`**, ni un timer que deba "cruzar" 0.75, ni depende del `dt` de simulación del personaje.
   Es una **caché por-char de un valor derivado de la HORA del juego** (reloj global
   `*(modBase+0x21303D0)`), **recalculado entero** cada vez que el char recibe el AI tick. Por eso
   "el host no acumula +0xD8" es un planteamiento equivocado: no hay nada que acumular.
2. **La semántica del gate 0.75 estaba INVERTIDA** en las notas y en el DIAG-THINK. El gate real
   (`0x5CD1F4: comiss 0.75,[char+0xD8]; jbe salir`) corre el think pesado **solo si `+0xD8 < 0.75`**
   (no si lo supera). Y además se **salta por completo** si `[vtbl+0x58](char)→[ret+0x250] != 0`
   (entonces piensa igual, ignorando +0xD8).
3. **Conclusión:** `char+0xD8`/`+0xDC` **NO bloquean el combate del host**. Si el host entra a la
   rama viva (ya confirmado: `+0x5BC==0` → `je 0x5CD1C0`) y aun así no pelea, la causa está **aguas
   arriba** (que `0x5CCD90` se invoque cada frame con sentido), **no** en el gate 0.75.
4. **Causa más probable, re-priorizada (ver §6):** el dato nuevo `hostInSimList=YES` **refuta H1**.
   Con el host en la lista y recibiendo el AI tick, la pista fuerte pasa a ser **el AI tick global
   en `0x5CCD90` cayendo en la rama "cadáver/catch-up" por `char+0xD0` atrasado** (diff > 12.0h →
   rama cleanup, que **no** escribe +0xD0 → bucle infinito). Ese es exactamente el caso que ataca
   el **FIX-SIMSEED** ya presente en `core.cpp` (seed de `+0xD0 = relojSim`). **Verificar que ese
   fix se está aplicando al char correcto** es el siguiente paso, no tocar +0xD8.

---

## 1. Qué hace `0x5CD1C0` paso a paso (CONFIRMADO por bytes)

Se entra aquí desde `0x5CCE2B` (`cmp byte[rdi+0x5BC],sil(=0); je 0x5CD1C0`) → char **VIVO**.
**Dato crítico:** justo antes, en `0x5CCE1D`, `mov rcx,[0x1421303D0]` cargó el **SimClock** en
`rcx`, y el `je` salta **sin reescribir rcx**. Por tanto, al entrar a la rama viva, `rcx = reloj`.

```asm
0x5CD1C0  call 0x66CB50            ; rcx = RELOJ (no el char). Devuelve float (fracción horaria).
0x5CD1C5  movss [rdi+0xD8],xmm0    ; *** char+0xD8 = resultado (rdi = Character) ***  ← write #1
0x5CD1CD  cmp [rdi+0xDC],sil(=0)
0x5CD1D4  je  0x5CD261             ; si char+0xDC == 0 → SALE (no piensa)
0x5CD1DA  mov rax,[rdi]           ; vtable del char
0x5CD1E0  call [rax+0x58]         ; → 0x594640 (construye string faction/global; devuelve obj)
0x5CD1E3  cmp [rax+0x250],rsi(=0)
0x5CD1EA  jne 0x5CD1FD            ; si [obj+0x250] != 0 → SALTA el gate 0.75 (piensa igual)
0x5CD1EC  movss xmm0,[0x141696C88]; const = 0.75
0x5CD1F4  comiss xmm0,[rdi+0xD8]  ; compara 0.75 vs char+0xD8
0x5CD1FB  jbe 0x5CD261           ; si 0.75 <= +0xD8 → SALE (no piensa)   ← GATE HORARIO
;  ── THINK PESADO (solo si +0xD8 < 0.75, o si [obj+0x250]!=0) ──
0x5CD1FD  mov [rdi+0xDC],sil(=0)  ; resetea flag +0xDC a 0
0x5CD204  call [rax+0x1D8]       ; → 0x5CE020 = getCurrentState (think principal)
0x5CD210  cmp [rax+8],0xB        ; ¿state.type == 0xB (idle/none)?
0x5CD214  je  0x5CD229
0x5CD21C  call [rax+0x1E0]       ; → 0x5E1E60 = bool hasActiveTask()
0x5CD222  test al,al ; jne 0x5CD229
0x5CD226  mov sil,1
0x5CD229  call [rax+0x60]        ; → 0xD1F80 = int getField(+0xA4)  -> ebx
0x5CD23F  call [r8+0x40]         ; → 0x5CE680 = dispatcher/resolver  -> rax
0x5CD24D  lea rcx,[0x142134110]  ; GameWorld (instancia embebida)
0x5CD254  call 0xA0AF10          ; commit a GameWorld (recalcula desde reloj) -> xmm0
0x5CD259  movss [rdi+0xD8],xmm0   ; *** char+0xD8 = nuevo valor ***                ← write #2
0x5CD261  ...                    ; epílogo: tick de movimiento ([rdi+0x648], [rdi+0x448]) y RET
```

**Puntos clave:**
- **NO es "early-return si +0xD8 < 0.75".** Es lo contrario: el think corre cuando **+0xD8 < 0.75**.
- El gate horario tiene un **bypass**: si `[vtbl+0x58](char)→[+0x250] != 0`, se ignora +0xD8 y se
  piensa siempre. Es decir, +0xD8 es un **filtro de frecuencia horario opcional**, no un cerrojo.
- `char+0xD8` se **escribe dos veces** en la propia rama viva (entrada y, si piensa, tras el commit).
  No persiste como "acumulador" entre frames; se sobrescribe cada AI tick.

---

## 2. Qué ACUMULA (en realidad: qué CALCULA) char+0xD8

**No acumula nada.** Lo **calcula** `0x66CB50(reloj)`:

```asm
0x66CB50  movss xmm0,[rcx+0x84]   ; rcx = RELOJ → +0x84 = hora actual del día (float)
0x66CB58  movss xmm1,[rcx+0xB4]   ; +0xB4 = 23.0f  (límite superior de la ventana)
0x66CB60  comiss xmm0,xmm1 ; jbe  ; si hora > 23.0 → return 0.0
0x66CB69  movss xmm2,[rcx+0xB0]   ; +0xB0 = 5.0f   (límite inferior)
          ... addss/subss con 0.0833333 (=1/12) ; normalización lineal inversa ...
0x66CBB8  movss xmm0,[0x14167C308]= 1.0  ; fallback
          ret
```

- `char+0x84/+0xB0/+0xB4` **NO son campos de Character** — son del **objeto reloj** (`+0xB0=5.0`,
  `+0xB4=23.0`, fijados en el ctor del reloj `0x66E380`; `+0x84` = hora variable en runtime).
- Constantes confirmadas: `0.75` (`0x1696C88`), `1/12=0.0833333` (`0x16FD53C`), `1.0` (`0x167C308`),
  ventana `5.0`/`23.0`. Interpretación: **+0xD8 ≈ fracción de la ventana diurna [0,1]** (CONFIRMADO
  en cuanto a las constantes; etiqueta "luz diurna/fase del día" es interpretación razonable).
- **De dónde viene el "dt":** de NINGÚN sitio del personaje. Viene del **reloj global**. Por eso la
  premisa "el dt del host llega en 0" no aplica: +0xD8 no se alimenta de un delta por-char.

`0xA0AF10` (write #2) **también** arranca con `mov rcx,[0x1421303D0]; call 0x66CB50` y un early-out
`comiss xmm6,[0.5]; jbe → return 1.0`; si no, escala por 2.0 y refina iterando entidades de la zona
(`zoneMgr [0x21349C0]`). Devuelve un float que es **el mismo tipo de "peso horario/de zona"**, no un
cooldown que decrezca ni una orden de combate encolada. (Lo de "encolar tarea" era suposición previa;
los bytes muestran un re-cómputo de peso, no un push a la cola de Tasks.)

---

## 3. La contradicción `GW+0x768` vs el AI tick — RESUELTA

El dato de runtime de esta tanda — **`[DIAG-SIMLIST] hostInSimList=YES`** — es el que cierra el caso:

- `GW+0x768/+0x770/+0x788` **es la lista (unordered_set) que itera `updateCharacters 0x786E30`** y
  la que alimenta el AI tick `[vtbl+0xE8] = 0x5CCD90`. Es la lista correcta. (audit-07 §3 confirmó que
  `+0x750…+0x788` son campos internos del **mismo** `std::unordered_set<Character*>` de MSVC.)
- `hostInSimList=YES` ⇒ **el host SÍ está en esa lista** ⇒ **H1 (host fuera de simulación) REFUTADA.**
- El host **sí recibe** el AI tick (entra a `0x5CCD90`, pasa el gate `+0x5BC==0`, va a `0x5CD1C0`).

Por tanto: el host recibe el tick, entra a la rama viva, y el gate 0.75 **no** lo bloquea (es horario
y con bypass). Lo que falla está **dentro de `0x5CCD90` antes de la rama viva**, o en la capa de
órdenes. Ver §6.

---

## 4. ¿El `dt=0` viene del sistema de sync (Fase 4)?

**No, no por la vía de +0xD8.** `char+0xD8` no consume el `dt` de simulación del personaje; consume
la **hora global**. Dos consecuencias:

- Un hook de tiempo/sync del mod que dejara el `dt` por-char en 0 **no afecta a +0xD8** (que se deriva
  del reloj global, no del delta del char).
- El reloj global **sí avanza** (`[DIAG-CLOCK] simClock avanza`, confirmado en sesiones previas). Así
  que `0x66CB50` produce valores válidos y `+0xD8` se recalcula con normalidad.

Donde el sync **sí** puede morder es en `char+0xD0` (**lastProcessed**, horas de juego ya procesadas
por este char): si un char reclamado por el mod arranca con `+0xD0 == 0.0` y el reloj ya va por >12h,
el AI tick `0x5CCD90` cae **antes de la rama viva** en la rama "catch-up/cleanup" (`0x5CCE76:
comiss diff,12.0; ja cleanup`) que **no** escribe +0xD0 → se queda clavado → nunca progresa el char.
Eso **sí** es plausible-mente "culpa del flujo de reclamo del mod", y es lo que ataca el FIX-SIMSEED.

**Importante / matiz:** la rama "catch-up" (`0x5CCE31…`) está en el camino del char **MUERTO**
(`+0x5BC != 0`), porque el `je 0x5CD1C0` del char vivo salta **por encima** de ella. Si el host es
`+0x5BC==0` (vivo, confirmado), va directo a `0x5CD1C0` y **NO pasa** por el catch-up de +0xD0. Esto
genera una **tensión** con la narrativa del FIX-SIMSEED (que asume que el host cae en catch-up):
para un char vivo, sembrar +0xD0 puede ser irrelevante. **Hay que confirmarlo en runtime** con el
DIAG-AICHK (`+0x5BC` del primario) ANTES de dar el seed por bueno. Ver §6, verificación.

---

## 5. Cambio aplicado en esta auditoría (solo DIAG, read-only)

El DIAG-THINK de `core.cpp` tenía la **semántica del gate 0.75 invertida** (decía "timer>0.75 → SÍ
debería pensar"), dando a Zero un veredicto **engañoso**. Corregido (sin tocar lógica de lectura, ya
bajo SEH):

- `SEH_ReadSimDiag` (~línea 2730): comentario corregido — +0xD8 = caché horaria, gate inverso, bypass.
- Bloque `[DIAG-THINK]` (~línea 3046): veredicto corregido:
  - `+0xD8 < 0.75` → "el gate horario PERMITE pensar"
  - `+0xD8 >= 0.75` → "gate horario salta (salvo `[vtbl+0x58]+0x250 != 0`, que piensa igual)"
  - Se añade la coletilla `[NOTA: +0xD8 NO es la causa del combate congelado]` para no volver a
    perseguir esta pista muerta.

**Compilación:** `cmake --build . --target KenshiMP.Core --config Release` → OK, sin errores ni
warnings nuevos. `KenshiMP.Core.dll` generado.

---

## 6. Causa más probable (re-priorizada) y qué verificar

Con `hostInSimList=YES` y el gate 0.75 descartado, el orden de probabilidad queda:

| # | Hipótesis | Cómo confirmar en el log | Fix candidato |
|---|---|---|---|
| **A** | El primario que el mod muestrea **NO es el char real que controla el jugador** (verify `char+0x10 faction FAIL` es la pista): el AI tick corre para el char bueno, pero el DIAG/seed apunta a otro. | `[DIAG-AICHK] +0x5BC` del primario, y `[DIAG-SIMLIST] primaryChar=0x…` ¿coincide con el char seleccionado en pantalla? | Arreglar la resolución del `primaryChar`/host-char en el mod (no toca el motor). |
| **B** | `char+0xD0` atrasado manda el tick a catch-up — **PERO solo si el host es `+0x5BC!=0`** (muerto) o si el camino real difiere. Si `+0x5BC==0`, el seed de +0xD0 es inocuo pero **no** desbloquea. | `[DIAG-AICHK] +0x5BC==0?` + `[DIAG-CLOCK] char+0xD0 avanza tras el seed?` | FIX-SIMSEED ya presente; **validar que aplica al char correcto y que +0xD0 empieza a avanzar**. |
| **C** | El tick corre y `+0xD0` avanza, pero **no hay Task de ataque encolado** (capa de ORDEN: Tasker/GOAP, audit-03 §3). El combate no es cuestión de AI tick sino de que la orden del jugador no llega a la cola. | `+0xD0` avanza pero sigue sin atacar | Rastrear el handler de clic→Task (RVA de la orden real, aún sin localizar). |

**Verificación concreta para la próxima prueba de Zero (con el DIAG ya recompilado):**
1. `[DIAG-AICHK]` — anotar `+0x5BC` del primario. Si `==0` (vivo) y NO pelea → la pista es **A o C**,
   **no** el seed de +0xD0 (que actúa en la rama del muerto).
2. `[DIAG-SIMLIST]` — `hostInSimList` debe seguir `YES`; anotar `primaryChar=0x…` y comprobar que es
   el char que Zero tiene seleccionado/controlando (no un template fantasma).
3. `[DIAG-THINK]` (ya corregido) — ahora dirá la verdad sobre +0xD8; servirá para confirmar que el
   valor es una fracción horaria sensata (típicamente en [0,1]) y descartarlo formalmente.
4. `[DIAG-CLOCK]` — `char+0xD0 avanza` tras el seed. Si **no** avanza y `+0x5BC==0`, el seed no aplica
   (rama del vivo) y hay que ir a **A/C**.

**NO escribir nada en +0xD8** (es una caché horaria del motor; sobrescribirla solo se pisaría al
siguiente AI tick y podría alterar el filtro de frecuencia de "think" de todos los chars). El único
write seguro ya en el mod es `+0xD0` (campo de estado del propio char), y su utilidad depende de
confirmar el camino (A/B/C) en runtime.

---

## 7. RVAs confirmados en esta auditoría (Steam 1.0.68, ImageBase 0x140000000)

| RVA | Rol (CONFIRMADO por bytes salvo indicación) |
|---|---|
| `0x5CCD90` | AI tick pesado `[vtbl+0xE8]` `(Character* rcx)`. Carga reloj en rcx (`0x5CCE1D`), gate `+0x5BC` (`0x5CCE24/2B`). |
| `0x5CD1C0` | **rama viva** (char vivo). Calcula `+0xD8` desde el reloj, gate horario 0.75, think pesado. |
| `0x5CD1C5` | `movss [char+0xD8],xmm0` — write #1 de +0xD8 (= `0x66CB50(reloj)`). |
| `0x5CD1F4` | `comiss 0.75,[char+0xD8]; jbe salir` — **gate horario** (think si `+0xD8 < 0.75`). |
| `0x5CD1EA` | `jne 0x5CD1FD` — **bypass** del gate si `[vtbl+0x58]→[+0x250] != 0`. |
| `0x5CD259` | `movss [char+0xD8],xmm0` — write #2 de +0xD8 (= `0xA0AF10(...)`). |
| `0x66CB50` | `float calc(GameClock* rcx)` — fracción de la ventana diurna 5.0–23.0 (consts 1/12, 1.0). |
| `0xA0AF10` | commit/recompute a GameWorld: recalcula desde reloj (early-out 0.5→1.0), refina por zona. Devuelve float → +0xD8. |
| `0x9B0F60` | helper de `0x66CB50`: `(x-lo)/(hi-lo)` (interp. lineal inversa). |
| `0x66E380` | ctor del objeto reloj (fija `+0xB0=5.0`, `+0xB4=23.0`, `+0x88=60`). |
| `0x21303D0` (.data) | puntero al **objeto reloj** (`*(modBase+0x21303D0)`). +0x84=hora, +0xA0=reloj(double). |
| `0x141696C88` (.rdata) | constante `0.75f` (gate horario). |
| `0x1416FD53C` | constante `0.0833333f` (=1/12). |
| `0x14167C308` | constante `1.0f` (fallback). |
| `0x16F9EB8` (.rdata) | **vtable de Character** (RTTI `.?AVCharacter@@`; validada: `[vt+0xE8]=0x5CCD90`). |
| `0x594640` | `[vtbl+0x58]` — construye string faction/global; su `[ret+0x250]` decide el bypass del gate. |
| `0x5CE020` | `[vtbl+0x1D8]` — getCurrentState (think principal); `state.type==0xB` = idle/none. |
| `0x5E1E60` | `[vtbl+0x1E0]` — `bool hasActiveTask()` (`state.type!=0xB && [char+0xD5]!=0`). |
| `0xD1F80` | `[vtbl+0x60]` — `int getField(+0xA4)` (getter trivial: `mov eax,[rcx+0xA4]; ret`). |
| `0x5CE680` | `[vtbl+0x40]` — dispatcher/resolver (usa `[char+0x2F8]`/`[char+0x3D4]`, map `[0x142133F90]`). |

Campos del **objeto reloj** (NO de Character): `+0x84` hora actual, `+0xB0`=5.0, `+0xB4`=23.0,
`+0xA0` reloj(double). Campos de **Character** relevantes aquí: `+0xD8` (caché horaria — write only
dentro de 0x5CCD90), `+0xDC` (flag dirty/repensar — reseteado a 0 al pensar), `+0xD0` (lastProcessed),
`+0x5BC` (isDead), `+0x640` (CharMovement), `+0xA4` (campo leído por `[vtbl+0x60]`).

---

## 8. Veredicto honesto

- **+0xD8 NO es la causa del combate congelado.** Es una caché derivada del reloj global, con gate
  horario invertido y con bypass. No hay nada que "forzar a cruzar 0.75": forzarlo no desbloquearía
  el combate y se pisaría al siguiente tick.
- **`hostInSimList=YES` refuta H1**: el host recibe el AI tick. El problema vive **dentro de
  `0x5CCD90`** (rama por `+0x5BC`, o catch-up por `+0xD0`) o en la **capa de órdenes** (Tasker/GOAP).
- **Siguiente paso correcto:** con el DIAG-THINK ya corregido + DIAG-AICHK + DIAG-SIMLIST, confirmar
  en runtime (a) que `primaryChar` ES el char que controla el jugador, (b) el valor de `+0x5BC` del
  primario, y (c) si `+0xD0` avanza tras el seed. Eso discrimina A vs B vs C **sin tocar +0xD8** ni
  ninguna estructura del motor.

## 9. Archivos relevantes (rutas absolutas)

- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\core.cpp` — DIAG-THINK corregido (~2730 y ~3046);
  `SEH_ReadSimDiag` (~2703), `SEH_WalkSimList` (~2657), FIX-SIMSEED (~2407–2520).
- `E:\Aplicaciones\Kenshi-Online\docs\architecture\audit-06-aitick-invocacion.md` — invocación del tick.
- `E:\Aplicaciones\Kenshi-Online\docs\architecture\audit-07-fix-h1-insercion.md` — `+0x750` = unordered_set; H1.
- `E:\Aplicaciones\Kenshi-Online\docs\reverse-engineering\kenshi-re-memory.md` — notas RE base (la
  sección "char+0x5BC = FLAG MUERTO" describe +0xD8 con la semántica antigua; esta auditoría la corrige).
