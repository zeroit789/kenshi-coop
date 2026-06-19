# Auditoría 06 — ¿Por qué el AI tick NO progresa para el char del host?

> Auditoría **READ-ONLY** (no se editó código). Fecha: 2026-06-18.
> Binario: `kenshi_x64.exe` v1.0.68 Steam (App 233860), MSVC x64, ImageBase `0x140000000`.
> Método: desensamblado directo de bytes con `iced-x86` + `pefile` sobre el .exe (RVA→file offset).
> Foco: invocación del AI tick pesado `[vtbl+0xE8] = 0x5CCD90` (combate / Jobs / levantarse /
> recuperar KO / regen stun) para el char del host en `GameWorld::updateCharacters` (`0x786E30`).

---

## 0. TL;DR (5 líneas)

El AI tick pesado `[vtbl+0xE8]` se llama en `0x786F3F` **antes** de leer el LOD del char (`+0xE4`)
y, con N≤8 chars activos, el round-robin lo concede a **TODOS cada frame** (tope duro `cmp edx,8`).
Por tanto, para el host (1 char / squad pequeño) **ni el round-robin ni `char+0xE4` son la causa**:
ambas hipótesis de la consigna quedan **refutadas por bytes**. La causa real, por orden de
probabilidad, es: (H1) **el char del host NO está en la lista de simulación `GW+0x768/+0x770/+0x788`**
(distinta del squad que itera el mod), o (H2) el gate `GW+0x8B9` re-pegado por `gameSpeed==0`.

---

## 1. Estructura confirmada de `updateCharacters` (`0x786E30`)

Prólogo `mov rax,rsp` (de ahí el MovRaxRsp fix). `r13 = this = GameWorld`. Recorre una
**lista enlazada** de nodos; cada nodo guarda el `Character*` en `nodo+0x10`; el "next" está en
`nodo+0x00` (`mov rbx,[rbx]` en `0x786EDB` y `0x78712E`). El primer nodo se obtiene de
`array[idx]` con `array = [GW+0x788]`, `idx = [GW+0x768]`, y `count = [GW+0x770]`.

### 1.1 Cuerpo por char (`0x786EF0`–`0x78712E`) — orden REAL de ejecución

```
0x786EF0  mov rdi,[rbx+10h]            ; rdi = Character* actual
0x786EF4  call 0x42136                 ; (prep; toca rdi)
0x786F00  movzx r12d, byte [rdi+0E4h]  ; r12b = char+0xE4  (LOD)  <-- se LEE aquí
;  ── BLOQUE AI TICK (clase A) ──
0x786F08  cmp edx,8                    ; edx = slotCount del frame (tope DURO = 8)
0x786F0B  jge 0x786F83                 ; si ya se gastaron 8 slots -> NO hace AI tick
0x786F0D  mov ecx,[2132ED0h]           ; cursor clase A
0x786F13  cmp ebp,ecx ; jg 0x786F27    ; ebp = índice del char en la pasada
0x786F17  ... (umbral cur - count + 9) ; reparte solo cuando count es grande
0x786F27  inc edx ; cmp edx,8 ; cmove ecx,ebp ; mov [2132ED0h],ecx
0x786F39  mov rax,[rdi]                ; vtable
0x786F3C  mov rcx,rdi
0x786F3F  call qword [rax+0E8h]        ; *** AI TICK PESADO = 0x5CCD90 ***  <-- AQUÍ
;  ── reparto en cola diferida [0x21348A0]+0x1D0 ──
0x786F83  test r12b,r12b               ; <-- char+0xE4 se USA RECIÉN AQUÍ
0x786F86  jne 0x787070                 ; rama LOD alto
;     LOD bajo (r12b==0): round-robin clase B (cmp r14d,6) -> [vtbl+0xE0]  o  char+0xC0 += dt
;     LOD alto (r12b!=0): round-robin clase C (budget esi) -> [vtbl+0xE0]
0x78712E  mov rbx,[rbx]               ; next; inc ebp; loop
0x787128  inc dword [2132ED4h]        ; frameCursor++ (por char)
```

### 1.2 Conclusión estructural (CONFIRMADA por bytes)

- **`[vtbl+0xE8]` (AI tick pesado) se llama en `0x786F3F`, ANTES del `test r12b,r12b` (`0x786F83`).**
  Su ejecución **NO depende** de `char+0xE4`. Solo la gobierna el round-robin de clase A (tope `8`).
- **`char+0xE4` solo decide qué rama de `[vtbl+0xE0]` (update "completo") se ejecuta** y su propio
  round-robin (clase B vs clase C). `[vtbl+0xE0]` es el update de presencia/animación/física, NO el
  AI tick de combate. Caminar/hablar/sigilo van por aquí + el bucle 2 (`[vtbl+0x268]`, incondicional).
- Por tanto, **un char en "LOD reducido" (`+0xE4==0`) SÍ recibe el AI tick de combate** mientras esté
  en la lista y haya presupuesto de slots. **`+0xE4` NO puede dejar al host sin AI.** → la **hipótesis
  #2 de la consigna queda REFUTADA**.

---

## 2. Round-robin del AI tick — modelado y refutado (hipótesis #1)

Lógica de entrada (`0x786E5B`–`0x786E9E`): `budget = max((gFrame + (gFrame&3))>>2, 4)` (`esi`), pero
el **techo real del AI tick clase A es la constante `cmp edx,8`** (8 invocaciones de `[vtbl+0xE8]`
por pasada). Simulación del reparto (`/tmp/rr.py`, fiel a los bytes):

| N (chars activos) | ¿todos reciben AI tick cada frame? |
|---|---|
| 1, 2, 3, 4, 8 | **SÍ** (todos, cada frame) |
| 9, 12, 30 | No — empieza a repartir (time-slicing) |

**Para el host la lista activa es pequeña (1 char, o el squad).** Mientras N≤8, el char del host
recibe `[vtbl+0xE8]` **cada frame**. Además el cursor `0x2132ED4` se **resetea a 0 cada pasada**
(`0x786EAA`) → el round-robin es **auto-recuperante**, no puede "congelarse" permanentemente.
→ la **hipótesis #1 de la consigna (round-robin se salta al host) queda REFUTADA** salvo que el
host tenga ≥9 chars en su lista de simulación, lo cual el DIAG ya puede medir (`activeChars(+0x770)`).

---

## 3. La LISTA de simulación: de dónde sale y por qué importa (hipótesis FUERTE)

### 3.1 Dos listas DISTINTAS (confirmado)

| Estructura | Qué es | Quién la usa |
|---|---|---|
| `GW+0x750` | **hash set** maestro de `Character*` "vivos en simulación" (insertador `0x5429A0`, función de hash con mul/shift = unordered_set). | El motor. |
| `GW+0x768`/`+0x770`/`+0x788` | **lista enlazada / snapshot linealizado** derivado de `+0x750`. Inicializado en el ctor de GameWorld (`0x875227`). | **`updateCharacters` itera ESTA** → es la que decide quién recibe AI tick. |
| `GW+0x890`/`+0x898` | **cola de altas diferidas**. Se vuelca a `+0x750` al final del tick (`0x795520`, llama insert `0x52838→0x5429A0` con `rcx=GW+0x750`). Gobernada por el guard `GW+0x749`. | El motor (commit diferido). |
| `GameWorld+0x580 → +0x2B0` | **`PlayerInterface.playerCharacters`** = lista del **SQUAD del jugador** (lektor de 24B: size@+0x08, cap@+0x0C, data@+0x10). | **El MOD itera ESTA** (`CharacterIterator::Reset`, `game_character.cpp:774`). |

**Punto crítico:** el mod (`CharacterIterator`, `FindAndClaimModCharacters`) trabaja sobre el
**squad** (`+0x580→+0x2B0`), NO sobre la lista de simulación (`+0x768`). **Estar en el squad ≠ estar
en la lista de simulación activa.** Si el char "Player N" del host está en el squad pero **no** entró
(o salió) del hash set `+0x750` → del snapshot `+0x768` → `updateCharacters` nunca lo recorre → **el
AI tick `[vtbl+0xE8]` jamás lo toca**, aunque el reloj avance y los demás chars se simulen. **Esto
encaja exactamente con "el mod hace algo raro con el char del host".**

### 3.2 Cómo entra un char en la lista de simulación

`array[count++]` con commit diferido vía cola `+0x890`. Un char spawneado por el flujo NATIVO del
mundo entra solo. Pero el char "Player N" es un **mod-template** spawneado/reposicionado por el mod
(`FindAndClaimModCharacters` le hace `WritePosition`, lo registra en el entity registry, pero **NO lo
inserta ni valida en `+0x750`/`+0x768`**). Si el template se materializó por una ruta que no encoló
su alta en `+0x890`, o si algún paso del mod lo sacó de la simulación, el AI tick nunca corre para él.

---

## 4. El gate `GW+0x8B9` (segunda causa más probable)

`mainLoop 0x788A00`, gate en `0x788FEE`: `cmp byte[rsi+8B9h],0 ; jne 0x789006`.
- `+0x8B9 == 0` → `call 0x4B664` (thunk → `updateCharacters 0x786E30`) → corre el AI tick.
- `+0x8B9 != 0` → rama "paused" `0x787230` → solo `[vtbl+0x270]` (tick reducido: pos/anim). **Reloj
  corre, chars animan, pero NO atacan/recuperan.** = síntoma exacto.

**Escritores de `+0x8B9` en TODO el binario (7 accesos totales):**
- `0x787D76`/`0x787D82` = setter `setPaused 0x787D40`: **`+0x8B9 = argBool OR (gameSpeed[+0x700]==0.0f exacto)`**
  (constante 0.0f en `0x1681B38`, comparación `==` estricta). **Único escritor gobernante en runtime.**
- `0x87534A` = `mov [rdi+8B9h], r13w` con `r13=0` → es el **ctor/init de GameWorld** (pone +0x8B0/+0x8B9/+0x8C0=0). Inocuo, no re-pausa por frame. (Verificado: descarta "segundo pausador oculto".)
- `0xDEE00`, `0x787CBF`, `0x788FEE` = lectores.

**Implicación:** si el host pausó con barra espaciadora (`gameSpeed=0.0`), `setPaused(false)` NO basta:
el `OR` re-pega `+0x8B9=1` mientras `gameSpeed` siga clavado en `0.0`. **Hay que subir `gameSpeed`
a `1.0` ANTES de llamar al setter.** (Ya documentado; el DIAG-SIM lee gate y gameSpeed.)

---

## 5. Hipótesis ordenadas por probabilidad

### H1 (MÁS PROBABLE) — El char del host NO está en la lista de simulación `+0x768`
El mod opera sobre el squad (`+0x580→+0x2B0`); la simulación itera `+0x768` (derivada de `+0x750`).
Si el char "Player N" no entró/salió del hash set `+0x750`, `updateCharacters` no lo recorre → sin AI tick.
- **DIAG discriminante:** recorrer la lista de simulación `+0x768/+0x770/+0x788` (la MISMA cadena que
  `updateCharacters`: head = `array[[GW+0x768]]` con `array=[GW+0x788]`, luego `next=[nodo]`,
  `char=[nodo+0x10]`) y comprobar si `primaryChar` aparece en ella. Loguear:
  `inSimList = (host ∈ +0x768)` y `simListCount(+0x770)`.
  - `inSimList == false` → **H1 CONFIRMADA.**
  - `inSimList == true` y combate sigue congelado → descartar H1, pasar a H2/H4.
- **Fix candidato:** insertar el char del host en la lista de simulación tras reclamarlo. Ruta segura:
  **no** escribir el hash set a mano (riesgo de corromper el unordered_set). En su lugar llamar al
  insertador del motor `0x5429A0` (vía `0x52838`) con `rcx = GW+0x750`, `rdx = &Character*`, **o**
  encolar el alta en `+0x890` para que el commit diferido (`0x795520`) lo vuelque. Validar con SEH +
  rango de heap. Alternativa de menor riesgo: averiguar por qué el spawn del template no lo registró y
  arreglar el spawn (que pase por el flujo nativo que encola en `+0x890`).

### H2 (PROBABLE) — Gate `+0x8B9` re-pegado por `gameSpeed==0`
Despausar sin subir antes `gameSpeed` a `1.0` deja el gate en 1 → rama paused → sin AI tick.
- **DIAG discriminante (ya presente):** `[DIAG-SIM]` vuelca `gate(+0x8B9)` y `gameSpeed(+0x700)`.
  - `gate==1` con `gameSpeed==0.0` → **H2 CONFIRMADA** (la simulación entra en rama paused).
  - `gate==0` y `gameSpeed==1.0` y combate congelado → descartar H2.
- **Fix candidato (ya diseñado):** en Step 0, `gameSpeed=1.0` ANTES de `setPaused(false)`. Confirmar
  que ningún otro punto reescribe gameSpeed a 0 cada frame (la UI de pausa). Si la UI lo reescribe,
  el fix debe correr cada tick o neutralizar el origen (input de pausa de UI).

### H3 (POSIBLE) — Lista de simulación con ≥9 chars (round-robin reparte)
Solo si el host tiene ≥9 chars activos en `+0x768`. Entonces el AI tick se reparte y un char puede
tardar varios frames en recibirlo (no congelado, sí ralentizado).
- **DIAG discriminante (ya presente):** `activeChars(+0x770)`. Si `< 9` → H3 DESCARTADA.
- **Fix candidato:** ninguno necesario salvo que `+0x770` sea anómalamente alto (entonces investigar
  por qué hay tantos chars en simulación local — posible fuga de remotos a la lista local).

### H4 (MENOS PROBABLE) — El char está en la lista pero el AI tick sale temprano internamente
`[vtbl+0xE8]=0x5CCD90` lee `char+0xCC` (timer), `char+0x640`, escribe estado de combate
`char+0x260/+0x268/+0x26C`. El AI tick global compara el reloj de sim (`SimClock+0xA0`, horas de
juego) con `char+0xD0` (lastProcessed). Si el char no progresa "trabajo" pese a recibir el tick, el
problema está DENTRO de `0x5CCD90` (p.ej. sin Task encolado, o `char+0xD0` ya == reloj → "nada que
hacer").
- **DIAG discriminante:** muestrear `char+0xD0` (lastProcessed) en dos instantes con combate
  ordenado. Si `+0xD0` **avanza** pero no hay daño → el tick corre, falta el Task de ataque (capa de
  orden, no de AI tick) → el problema es de órdenes (auditoría 03 §3), no de invocación. Si `+0xD0`
  **NO avanza** y `inSimList==true` y `gate==0` → el tick no se invoca pese a estar en lista →
  contradicción que apuntaría a un round-robin con N≥9 (volver a H3) o a reentrancia del guard `+0x749`.
- **Fix candidato:** depende del subcaso; mayoritariamente recae en la capa de ORDEN (Tasker/GOAP),
  no en updateCharacters.

---

## 6. DIAG nuevo a añadir (un solo bloque SEH read-only) para discriminar H1/H4

El DIAG-SIM actual lee `gate`, `gameSpeed`, contadores, `activeChars(+0x770)` y `primaryLOD(+0xE4)`.
**Falta lo único que discrimina la hipótesis fuerte H1:** ¿está el host EN la lista de simulación?

Añadir a `SimDiagSnapshot` / `SEH_ReadSimDiag` (en `core.cpp`, junto a `0x2300`–`0x2335`):

1. **`bool hostInSimList`** — recorrer la lista `+0x768/+0x770/+0x788` igual que `updateCharacters`:
   - `if ([GW+0x770]==0) -> lista vacía`.
   - `node = ([GW+0x788])[[GW+0x768]]`; luego `while(node){ if([node+0x10]==primaryChar) found; node=[node]; }`
     (cap de iteraciones ~10000 + validación de heap por salto, como hace `CharacterIterator`).
2. **`uint32_t simListCount = [GW+0x770]`** (ya se lee como `activeCnt`; renombrar en el log a
   "simListCount" para no confundir con el squad).
3. **`float hostLastProcessed = [primaryChar+0xD0]`** (para H4) — muestrear su avance entre frames.

Log sugerido (una línea): `[DIAG-AITICK] hostInSimList={} simListCount={} hostLastProcessed(+0xD0)={:.6f} adv={} gate={} gameSpeed={:.2f}`.

Tabla de decisión con el DIAG completo:

| `gate` | `gameSpeed` | `hostInSimList` | `+0xD0 adv` | Diagnóstico | Hipótesis |
|---|---|---|---|---|---|
| 1 | 0.0 | — | — | rama paused, sin AI tick | **H2** |
| 0 | 1.0 | **false** | — | host fuera de simulación | **H1** |
| 0 | 1.0 | true | FROZEN | en lista pero tick no avanza su +0xD0 | H3 (≥9 chars) o reentrancia +0x749 |
| 0 | 1.0 | true | YES | tick corre; falta el Task de ataque | capa de ORDEN (audit-03 §3), no AI tick |

---

## 7. RVAs confirmados en esta auditoría (Steam 1.0.68, ImageBase `0x140000000`)

| RVA | Símbolo / rol |
|---|---|
| `0x786E30` | `GameWorld::updateCharacters` (simulador; `mov rax,rsp`) |
| `0x786F3F` | `call [vtbl+0xE8]` = invocación del AI tick pesado |
| `0x786F00` | `movzx r12d, byte[char+0xE4]` (lee LOD) |
| `0x786F83` | `test r12b,r12b; jne` (USA el LOD — DESPUÉS del AI tick) |
| `0x786EAA` | reset frameCursor `0x2132ED4=0` (round-robin auto-recuperante) |
| `0x5CCD90` | AI tick pesado `[vtbl+0xE8]` `(Character* rcx)`; lee +0xCC/+0x640, escribe +0x260/+0x268/+0x26C |
| `0x788FEE` | gate `cmp byte[GW+0x8B9],0; jne` en `mainLoop` |
| `0x4B664` | thunk `jmp 0x786E30` (único call del gate) |
| `0x787230` | rama "paused" de `mainLoop` (`[vtbl+0x270]`) |
| `0x787D40` | `setPaused`; `+0x8B9 = arg OR (gameSpeed==0.0f)` en `0x787D74/D82` |
| `0x1681B38` | constante `0.0f` comparada con gameSpeed |
| `0x87534A` | `mov [GW+0x8B9],0` en el **ctor de GameWorld** (inocuo, no re-pausa) |
| `0x795520` | commit diferido: cola `[GW+0x890]` → insert en `GW+0x750` |
| `0x5429A0` | insertador real del hash set `+0x750` (`0x52838`→ILT) |
| `0x875227` | ctor de GameWorld: init de la lista de simulación `+0x768/+0x788` |

Lista de simulación que itera `updateCharacters`: **`GW+0x768`** (idx/head), **`GW+0x770`** (count),
**`GW+0x788`** (array de nodos); nodo: `+0x00`=next, `+0x10`=`Character*`.

---

## 8. Archivos relevantes (rutas absolutas)

- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\core.cpp` (DIAG-SIM/DIAG-CLOCK: 2513–2605; `SEH_ReadSimDiag`/`SimDiagSnapshot`: ~2300–2335; Step 0 unpause; `FindAndClaimModCharacters`: 2066)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\game\game_character.cpp` (`CharacterIterator::Reset`: 662–810 — itera el SQUAD, no la lista de simulación)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\game\game_types.h` (offsets: `world.player=0x580`, `playerInterface.playerCharacters=0x2B0`, `characterList=0x888` DEPRECADO)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\hooks\entity_hooks.cpp` (NPC hijack — solo REMOTOS, no el host)
- `E:\Aplicaciones\Kenshi-Online\docs\reverse-engineering\kenshi-re-memory.md` (notas RE base de esta auditoría)
- `E:\Aplicaciones\Kenshi-Online\docs\architecture\audit-03-combate-plan.md` (capa de orden/pausa — complementaria)
