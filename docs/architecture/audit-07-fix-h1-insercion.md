# Auditoría 07 — Diseño del FIX de H1 (combate congelado: char del host fuera de la simulación)

> Auditoría **READ-ONLY de DISEÑO** (NO se editó código del mod). Fecha: 2026-06-18.
> Binario: `kenshi_x64.exe` v1.0.68 Steam (App 233860), MSVC x64, ImageBase `0x140000000`.
> Método: desensamblado directo de bytes con `pefile` + `iced-x86` sobre el .exe (RVA→VA).
> Objetivo: diseñar (sin aplicar) el fix de **H1** = el `Character*` del host no está en la lista de
> simulación activa que recorre `updateCharacters 0x786E30`, por lo que no recibe el AI tick
> `[vtbl+0xE8] = 0x5CCD90` → combate/levantarse/recuperar KO/regen stun congelados.

---

## 0. TL;DR (lo esencial primero)

1. **CORRECCIÓN GRAVE del audit-06:** `0x5429A0` **NO es un insertador**. Es el **`erase`/`remove`**
   del `unordered_set`. Y la cola `+0x890` **NO es de altas diferidas — es de BAJAS diferidas**
   (su flush en `0x795520` y `0x7A8E60` llama a `erase` por cada elemento). **Llamar a `0x5429A0`
   o encolar en `+0x890` para "añadir" el char del host BORRARÍA chars, no los añadiría.** Ese era
   el plan candidato del audit-06 §5/H1 y es **directamente peligroso**. Descartado.
2. **`+0x750` y `+0x768/+0x770/+0x788` son la MISMA estructura**, no dos. Es **un único
   `std::unordered_set<Character*>` de MSVC** embebido en GameWorld a partir de `+0x750`. El audit-06
   los modeló como "hash set maestro" vs "snapshot linealizado derivado" — eso es incorrecto: son
   campos internos contiguos del mismo set. Confirmado por el ctor `0x8751EF`–`0x875227`.
3. **El insertador real del motor NO se identificó con firma verificada.** El hasher del set
   (`imul 0x109`) aparece en 344 sitios (es genérico de `unordered_set<ptr>`), así que no discrimina.
   Llamar a un "insert del motor" a ciegas es de alto riesgo.
4. **Recomendación honesta:** **NO insertar nada todavía.** Primero ejecutar el `[DIAG-SIMLIST]`
   (ya implementado, fiel a `updateCharacters`) y CONFIRMAR `hostInSimList=NO`. Solo entonces, y con
   el **insert real localizado y su firma verificada**, aplicar el fix. Si urge, la ruta de **menor
   riesgo** no es insertar a mano sino **arreglar el origen**: hacer que el char del host pase por el
   flujo nativo de spawn que ya lo registra en el set (ver §5, opción C).

---

## 1. Qué es realmente `0x5429A0` (el supuesto "insertador" del audit-06)

Desensamblado (`VA 0x1405429A0`):

```asm
0x5429A0  push rbp ; sub rsp,0x20
0x5429A6  cmp qword [rcx+0x20],0       ; if (set._Size == 0)
0x5429AE  jne 0x5429B8
0x5429B0  xor eax,eax ; ... ; ret      ;   return 0   (set vacío → nada que borrar)
0x5429B8  mov r9,[rdx]                 ; r9 = *pCharacter   (rdx = Character**)
          ... hasher (shr/shl/xor, imul 0x109, imul 0x15, ...) ...
0x542A1D  mov rdi,[rax+rsi*8]          ; rdi = bucketArray[hash & mask]
          ... recorre la cadena del bucket ...
0x542A45  cmp r9,[rcx+0x10]            ; ¿este nodo guarda nuestro Character*?
0x542A49  je 0x542A68                  ;   sí → rama ELIMINAR
0x542A56  xor eax,eax ; ... ; ret      ;   no encontrado → return 0
0x542A68:                              ; rama ELIMINAR (elemento hallado)
0x542A79  call 0x384700               ;   desenlaza nodo(s): [rdi]=next, dec [set+0x20], deleter
0x542A8A  call 0x540A60               ;   relink del bucket (first/last)
          ... ; ret
```

- `0x384700` (rama found, paso 1): bucle `mov [rdi],r9 ; call [0x142248828] ; dec [set+0x20]` →
  **desenlaza y destruye nodo(s) y decrementa el `_Size`**. `[0x142248828]` = deleter/free del nodo.
- `0x540A60` (rama found, paso 2): repara los punteros `first/last` del bucket (`[set+0x38]`).

**Conclusión: `0x5429A0(set, Character**)` es `unordered_set::erase(*pChar)`.** Devuelve 0 si no
existe, ≠0 si borró. **Inequívocamente una BAJA, no un alta.**

## 2. La cola `+0x890` es de BAJAS diferidas (no de altas)

`0x795520` (flush corto) y `0x7A8E60` (teardown) hacen ambos:

```asm
        ; for (i=0; i < [GW+0x890]; i++)
mov rax,[GW+0x898]                ; array de la cola
lea rdx,[rax + i*8]              ; &cola[i]   (Character**)
lea rcx,[GW+0x750]               ; el set
call 0x052838                    ; → 0x5429A0 = ERASE
...
mov dword [GW+0x890],0           ; vacía la cola
```

Cada elemento de la cola `+0x890`/`+0x898` se pasa a **`erase`**. Por tanto **`+0x890` acumula
Characters a ELIMINAR del set en el commit diferido**, gobernado por el guard `+0x749`. El audit-06
§3.1 ("cola de altas diferidas … se vuelca a `+0x750`") es **incorrecto**. **Encolar el host aquí lo
borraría de la simulación al siguiente commit** — exactamente lo contrario de lo que queremos.

## 3. `+0x750` … `+0x788` = UN solo `unordered_set<Character*>` (MSVC)

Init en el ctor de GameWorld (`0x8751EF`–`0x875227`, `rdi = this`):

```asm
0x8751EF  mov [rdi+0x750],r13b              ; +0x750  byte   (flag/padding del comparador EBO)
0x8751F6  mov [rdi+0x758],rsi               ; +0x758  ptr    (proxy del allocator)
0x8751FD  mov [rdi+0x760],rbx               ; +0x760  ptr    (_List: sentinel de la lista interna)
0x875204  mov qword [rdi+0x768],0x10        ; +0x768  = 0x10  (_Mask inicial; NO "head index")
0x87520F  mov [rdi+0x770],r13               ; +0x770  = 0      (_Size = nº de elementos)
0x875216  mov dword [rdi+0x778],0x3F800000  ; +0x778  = 1.0f   (max_load_factor)
0x875220  mov [rdi+0x780],r13               ; +0x780  = 0
0x875227  mov [rdi+0x788],r13               ; +0x788  = 0      (_Vec: bucket array, null hasta 1er insert)
```

Es la huella canónica de `std::unordered_set` de MSVC (`_Hash`): comparador EBO, allocator proxy,
lista doblemente enlazada de nodos (`_List`), `_Mask`, `_Size`, `_Maxidx`/load-factor, `_Vec`
(bucket array de pares first/last). **`+0x768`, `+0x770`, `+0x788` son campos internos del set que
empieza en `+0x750`, no estructuras separadas.**

### 3.1 Cómo `updateCharacters 0x786E30` itera ese set (confirmado por bytes)

```asm
0x786EB4  cmp [r13+0x770],rdi      ; rdi=0 → if (_Size==0) lista vacía → skip
0x786EBD  mov rcx,[r13+0x768]      ; _Mask (=0x10 inicial)
0x786EC4  mov rax,[r13+0x788]      ; _Vec (bucket array)
0x786ECB  mov rcx,[rax+rcx*8]      ; head = _Vec[_Mask]  (acceso al sentinel/primer nodo del _List)
...
0x786EDB  mov rbx,[rbx]            ; next = [nodo]      (recorre el _List interno)
0x786EF0  mov rdi,[rbx+0x10]       ; Character* = nodo+0x10
0x786EF3..0x786F3F  ... AI tick [vtbl+0xE8] ...
```

El `[DIAG-SIMLIST]` (`core.cpp:2346 SEH_WalkSimList`) replica EXACTAMENTE este recorrido
(`count=+0x770`, `array=+0x788`, `idx=+0x768`, `head=array[idx]`, `next=[nodo]`,
`char=[nodo+0x10]`). Es correcto. (Nota cosmética: lo nombra "head index" cuando es `_Mask`; el
resultado del walk es el mismo, no afecta a la corrección.)

## 4. Por qué el char del host puede quedar FUERA del set

- El char del host **no se crea por `CharacterCreate`** (ese hook sólo cubre spawns nativos y el
  NPC-hijack de **remotos**, `entity_hooks.cpp:325/585`). El char "Player N" ya existe al cargar el
  `.mod`; el mod lo **reclama por nombre** en `FindAndClaimModCharacters` (`core.cpp:2066`).
- Reclamar = `WritePosition` + registrar en el `EntityRegistry` del mod (`core.cpp:2121-2126`). **No
  toca `GW+0x750`.** El char entra en el squad del jugador (`GW+0x580 → PlayerInterface +0x2B0`),
  que es lo único que el `CharacterIterator` del mod recorre (`game_character.cpp:748-775`).
- **Estar en el squad ≠ estar en el `unordered_set` de simulación `+0x750`.** Si el char "Player N"
  del template del mod no fue insertado en `+0x750` por el flujo de carga (o fue removido), entonces
  `updateCharacters` no lo recorre → **nunca recibe `[vtbl+0xE8]` (AI tick)** aunque el reloj avance
  y los demás chars se simulen. Esto encaja con el síntoma exacto y con la pista `char+0x10 (faction)
  FAIL` del `/verify`.

**Importante:** esto sigue siendo una **hipótesis**. La confirma o la refuta el `[DIAG-SIMLIST]` en
juego (`hostInSimList`). NO tocar el set antes de ese resultado.

## 5. Diseño del fix — opciones, de menos a más riesgo

> Precondición para CUALQUIER opción: `[DIAG-SIMLIST]` debe reportar `hostInSimList=NO` con
> `gate(+0x8B9)==0` y `gameSpeed==1.0` (descartado H2). Si `hostInSimList=YES`, H1 queda refutada y
> NO se aplica nada de esta auditoría (ir a H3/H4 del audit-06).

### Opción C (RECOMENDADA — menor riesgo): arreglar el ORIGEN, no insertar a mano
No tocar el `unordered_set` directamente. Investigar por qué el template "Player N" no quedó en
`+0x750` tras la carga y forzar el flujo NATIVO que sí registra:
- Hipótesis: el char del host existe "dormido" (cargado del save del .mod) y el motor lo inserta en
  `+0x750` sólo cuando entra en una zona activa / se le asigna el control del jugador. Reclamarlo y
  `WritePosition` puede dejarlo en un estado intermedio.
- Acción de diseño: localizar la función nativa "activar Character en simulación / entrar en zona"
  (el insert real del set), y llamarla **una vez** sobre el char del host justo tras reclamarlo, en
  lugar de manipular el set. **Pendiente: localizar esa función con firma verificada** (ver §6).
- Ventaja: usa la ruta que el motor ya usa para todos los NPCs → respeta invariantes del set, el
  `_List`, el bucket array y el guard `+0x749`. Sin riesgo de corromper el `unordered_set`.

### Opción A (insert directo del motor): viable SOLO con el insert real localizado y verificado
- Llamar a `engineInsert(rcx = &GW+0x750, rdx = &hostChar)` con la función `insert` REAL del set.
- **Bloqueante:** esa función **no está identificada con certeza** (el audit-06 confundió `0x5429A0`
  = erase con el insert). Hasta tenerla, esta opción **no es ejecutable sin adivinar**.
- Si se localiza: el insert de MSVC es find-or-insert (idempotente), así que **no hay riesgo de doble
  inserción** (si ya está, no duplica). Aun así envolver en SEH + validar `hostChar` en rango de heap
  y `GW` válido. Ejecutar en el **hilo de lógica** (dentro de un hook de game-tick), nunca desde el
  hilo de red ni el de render → thread-safety con `updateCharacters`.

### Opción B (encolar en `+0x890`): DESCARTADA — la cola es de BAJAS
Encolar el host en `+0x890` lo programaría para **`erase`** en el commit diferido. Hace lo contrario
de lo deseado. **No usar.**

### Riesgos transversales (cualquier opción que toque el set)
- **Corromper el `unordered_set`:** escribir nodos/buckets a mano sin respetar `_List`, `_Mask`,
  `_Vec` y `_Size` rompe el set para TODO el motor (crash o iteración corrupta en `updateCharacters`,
  que corre cada frame). Por eso se prohíbe el insert "manual byte a byte".
- **Thread-safety:** `updateCharacters` corre en el hilo de lógica cada frame y pone los guards
  `+0x8B8`/`+0x749` mientras itera. Insertar mientras itera puede invalidar el walk. Insertar SOLO
  cuando `updateCharacters` no está en curso (en un hook de inicio/fin de tick), no en medio.
- **Reentrancia del guard `+0x749`:** gobierna el commit diferido (altas/bajas). Tocarlo a mano
  puede dejar la cola sin volcar. No tocarlo.

## 6. Lo que falta ANTES de poder aplicar el fix (trabajo de RE pendiente)

1. **Confirmar H1 en juego** con `[DIAG-SIMLIST]` (`hostInSimList=NO`, `gate=0`, `gameSpeed=1.0`).
   Sin esto, no se toca nada.
2. **Localizar el INSERT real del set `+0x750`** con firma verificada. Pistas:
   - El erase es `0x5429A0`; el insert MSVC es su gemelo (mismo prólogo + mismo hasher, pero rama
     "no encontrado → allocar nodo + enlazar" en vez de "encontrado → borrar").
   - Buscar callers que hagan `lea rcx,[reg+0x750] ; ... ; call <fn>` donde `<fn>` NO sea
     `0x023DEE` (clear), `0x052838` (erase) ni `0x052C3E` (begin/iterator). Candidatos vistos:
     `0x36BAB2` (es clear, descartar), sitios `0x79BA3D`/`0x85685B`/`0x88893C` (par
     `0x0110C2`+`0x008A3F`) — revisar si `rbp` ahí es GameWorld y si el segundo call inserta un
     `Character*`; **no confirmado en esta auditoría**.
   - Alternativa más fiable: localizar la función nativa de "spawn/activar Character" (la que el
     motor llama cuando un NPC entra en zona) y ver qué `insert` invoca sobre `+0x750`.
3. **Decidir punto de inyección:** tras `FindAndClaimModCharacters` reclamar el host (`core.cpp:2124`),
   en el hilo de lógica, una sola vez (one-shot re-armable en disconnect, como el host-faction-fix).

## 7. RVAs (corregidos/confirmados en esta auditoría, Steam 1.0.68, ImageBase 0x140000000)

| RVA | Rol REAL (corrige audit-06) |
|---|---|
| `0x5429A0` | **`unordered_set::erase(set, Character**)`** — NO insertador. Audit-06 lo etiquetó mal. |
| `0x052838` | thunk → `0x5429A0` (erase). |
| `0x384700` | erase paso 1: desenlaza/destruye nodo, `dec [set+0x20]`, deleter `[0x142248828]`. |
| `0x540A60` | erase paso 2: relink first/last del bucket. |
| `0x023DEE` / `0x385020` | thunk / **`unordered_set::clear`** del set (teardown). SITIO A `0x36CD0B`. |
| `0x052C3E` / `0x664B90` | thunk / **`begin()`/iterator-fetch** del set. SITIO B `0x66266B`. |
| `0x795520` | flush corto: vacía cola `+0x890` llamando **erase** por elemento. (NO insert.) |
| `0x7A8E60` | teardown: vacía set `+0x7E8/+0x808` y cola `+0x890` (erase). |
| `0x786E30` | `GameWorld::updateCharacters`; itera el set `+0x768(_Mask)/+0x770(_Size)/+0x788(_Vec)`. |
| `0x8751EF`–`0x875227` | ctor: init del `unordered_set` `+0x750`…`+0x788`. |
| `0x142248828` | deleter/free de nodo del set (puntero en .data). |
| `0x5CCD90` | `[vtbl+0xE8]` = AI tick pesado (combate/Jobs). El que el host no recibe si está fuera. |
| **INSERT real** | **NO LOCALIZADO** — pendiente (ver §6.2). Bloqueante para Opción A. |

## 8. Veredicto honesto

- El plan candidato del audit-06 (llamar `0x5429A0` o encolar en `+0x890` para "añadir" el host)
  es **peligroso y haría lo contrario** (borrar chars). **Descartado.**
- **No se debe insertar nada todavía:** (a) H1 no está confirmada en juego, y (b) el insert real del
  set no está identificado con firma verificada. Tocar el `unordered_set` a ciegas puede corromper la
  simulación entera (crash por frame).
- **Ruta correcta:** 1) confirmar `hostInSimList=NO` con el DIAG; 2) localizar el insert real (o la
  función nativa de "activar en simulación"); 3) preferir la **Opción C** (arreglar el origen del
  spawn/reclamo) sobre el insert manual. Solo si C no es viable, **Opción A** con el insert verificado,
  envuelto en SEH, en el hilo de lógica, fuera del recorrido de `updateCharacters`.

## 9. Archivos relevantes (rutas absolutas)

- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\core.cpp` — `FindAndClaimModCharacters` (2066),
  `SEH_WalkSimList`/`[DIAG-SIMLIST]` (2294–2391, 2672+), Step 0 unpause / host-faction-fix (2214+).
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\game\game_character.cpp` — `CharacterIterator::Reset`
  (662–810; itera el squad `+0x580→+0x2B0`, NO el set de simulación).
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\hooks\entity_hooks.cpp` — `Hook_CharacterCreate` (585),
  NPC-hijack (325) — cubre remotos/spawns nativos, NO el reclamo del host.
- `E:\Aplicaciones\Kenshi-Online\docs\architecture\audit-06-aitick-invocacion.md` — auditoría previa
  (corregida aquí en §3.1 "insertador"/"cola de altas").
- `E:\Aplicaciones\Kenshi-Online\docs\reverse-engineering\kenshi-re-memory.md` — notas RE base.
