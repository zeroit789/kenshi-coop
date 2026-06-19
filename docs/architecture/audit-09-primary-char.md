# audit-09 — ¿El mod resuelve el char CORRECTO del jugador? (DIAG-PRIMARY)

> **Fecha:** 2026-06-18 · **Binario:** `kenshi_x64.exe` Steam 1.0.68 · ImageBase 0x140000000
> **Ámbito:** Investigación RE (read-only) + DIAG read-only en `core.cpp`. NO se tocan fixes
> ni estructuras del motor. Verificado por desensamblado independiente (capstone + pefile).

---

## 0. Veredicto corto

**SOSPECHA FUNDADA de que el mod agarra el char EQUIVOCADO.** Por desensamblado de bytes está
**CONFIRMADO** que Kenshi guarda el personaje *controlado/activo* del jugador en un campo
SEPARADO del que lee el mod:

| Quién | Cómo resuelve el "char del jugador" | Offset |
|---|---|---|
| **El mod (hoy)** | `data[0]` del lektor `playerCharacters` | `PlayerInterface + 0x2B0` → `+0x2C0`[0] |
| **El motor (real)** | `controlledChar` (objeto de las acciones del jugador) | `PlayerInterface + 0x2A8` |

Si ambos punteros divergen en la partida real, los fixes del mod (faction `+0x10`, seed `+0xD0`)
se aplican a un char que el jugador NO controla, y el char real nunca se toca → causa raíz
candidata del combate congelado. **Falta confirmar la divergencia en runtime** (para eso es el
`[DIAG-PRIMARY]` añadido). No es 100 % seguro porque con 1 solo char vivo ambos coinciden;
divergen al haber escuadra o cuando un NPC contratado entra primero en el lektor del mod.

---

## 1. Evidencia de RE (CONFIRMADA por bytes, doble verificación)

### 1.1 `SetControlledChar` — escribe `PI+0x2A8`

- String ancla: `"Player now controlling: "` en **RVA `0x171AA88`** (`.rdata`), referenciado por
  la función en **RVA `0x802520`**.
- Núcleo desensamblado (Intel, verificado con capstone sobre el .exe):

```asm
0x80267A  mov ecx, [r8+0x210]      ; r8 = Faction (PI+0x2A0); ecx = memberCount
0x802681  dec ecx                  ; index = count-1   (toma el ÚLTIMO miembro)
0x802683  mov rax, [r8+0x218]      ; memberArray (Character**)
0x80268A  mov rcx, [rax+rcx*8]     ; rcx = memberArray[count-1] = Character*
0x80268E  mov [rbx+0x2A8], rcx     ; *** PlayerInterface+0x2A8 = char controlado ***
0x802695  add r8, 0x1A8            ; r8 = faction.name (+0x1A8) → imprime el nombre
```

### 1.2 Segunda ruta de escritura de `PI+0x2A8` (RVA `0x7F8C00`)

```asm
0x7F8C0C  mov eax, [rsi+0x218]     ; faction.memberArray
0x7F8C1D  cmp [rax+0x2A8], rcx     ; ¿ya es el controlado?
0x7F8C26  mov [rax+0x2A8], rcx     ; PI+0x2A8 = char controlado
0x7F8C45  cmp r13d, [rsi+0x210]    ; loop hasta faction.memberCount
```

### 1.3 Consumo de `PI+0x2A8` — el motor lo LEE para aplicar la acción del jugador

```asm
0x50E9C8  mov rcx, [rsi+0x1B8]     ; rsi+0x1B8 = PlayerInterface (ruta de acceso alterna)
0x50E9CF  mov rcx, [rcx+0x2A8]     ; rcx = char controlado → sujeto del comando/evento
```

### 1.4 Offsets confirmados de Faction (refuerzo)

| Offset | Campo | Confirmación |
|---|---|---|
| `Faction + 0x210` | `memberCount` (uint32) | `cmp dword [r8+0x210]`, `dec ecx` |
| `Faction + 0x218` | `memberArray` (`Character**`) | `mov rax,[r8+0x218]; mov rcx,[rax+rcx*8]` |

> Estos no estaban documentados en `06-game-offsets.md` §5 (Faction); ahí solo aparecían
> `+0x1A8 name`, `+0x250 isPlayer`. Añadirlos cuando se confirme el fix.

### 1.5 PlayerInterface — campo nuevo

```
PlayerInterface (apuntado por GameWorld+0x580):
   +0x2A0  participant (Faction*)            [ya documentado]
   +0x2A8  controlledChar (Character*)       *** NUEVO — char controlado activo ***
   +0x2B0  playerCharacters (lektor)         [lo que usa el mod]
             +0x2B8 size · +0x2BC cap(=10) · +0x2C0 data → data[0]
```

Dato de uso (608 sitios que cargan el PI global): `+0x2A0` ×155, `+0x298` ×76, `+0x2A8` ×4,
y el lektor `+0x2B0`/`+0x2C0` **≈ ×0** desde el flujo del jugador. El motor gobierna al jugador
por **facción + controlledChar**, casi nunca por `playerCharacters`. Esto explica que `data[0]`
no sea fiable como "el char que controlas".

---

## 2. Por qué el mod puede estar equivocado

- `GetPlayerPrimaryCharacterDirect()` (`game_character.cpp:913`) lee `PI+0x2B0` → `data[0]`.
- `PlayerController::GetPrimaryCharacter()` (`player_controller.cpp:46`) devuelve el primer
  objeto del **EntityRegistry del mod** (lo que el mod registró al crear personajes).
- Ninguno de los dos lee `PI+0x2A8`. Ambos asumen que el primer char de su lista = el que
  controla el jugador. El motor NO comparte esa premisa: usa el último miembro de la facción
  (`Faction+0x218[count-1]`) volcado a `PI+0x2A8`.
- `data[0]` es **orden de inserción** del lektor, sin concepto de "principal". No hay evidencia
  de que el motor trate `data[0]` como líder.

---

## 3. DIAG añadido — `[DIAG-PRIMARY]` (solo `core.cpp`, read-only, SEH)

Añadido dentro del bloque throttled `s_simDiagCount` de `OnGameTick` (junto a DIAG-SIM/CLOCK/
AICHK/THINK/SIMLIST), mismo throttle (primeros 20 frames + cada 120). Piezas nuevas, todas POD
y SEH-aisladas (restricción C2712), **sin tocar fixes ni estructuras del motor**:

- `struct PrimaryDiagSnapshot` — POD de salida.
- `SEH_ReadCharName()` — lee `char+0x18` (std::string MSVC SSO/heap) a buffer C, solo imprimibles.
- `SEH_ReadPrimaryDiag()` — resuelve `PI = *(GW+0x580)` y vuelca los 3 candidatos.
- Bloque `[DIAG-PRIMARY]` que loguea y emite veredicto.

### Qué volcará (2 líneas por muestra)

```
[DIAG-PRIMARY] PI=0x... | MOD-usa=0x... (vs data0=IGUAL/DISTINTO vs ctrl=IGUAL/DISTINTO)
               | data[0](PI+0x2B0)=0x... name='...' fac=0x... lektor[size=N cap=10]
               | ctrlChar(PI+0x2A8)=0x... name='...' fac=0x...
               | facMemberCnt(+0x210)=N facLast(+0x218[cnt-1])=0x...
[DIAG-PRIMARY] modEqCtrl=? modEqFacLast=? ctrlEqFacLast=? ==> <VEREDICTO>
```

- `MOD-usa` = `m_playerController.GetPrimaryCharacter()` (el char que el mod realmente parchea).
- `data[0]` = lo que devolvería `GetPlayerPrimaryCharacterDirect()`.
- `ctrlChar` = `PI+0x2A8` (el char que el motor considera controlado).
- `facLast` = `Faction+0x218[count-1]` (la fuente por defecto de `SetControlledChar`).

### Veredictos posibles

| Condición | Significado |
|---|---|
| `ctrlChar==0` | Aún sin char controlado (menú/carga) — reintentar. |
| `data[0] == ctrlChar` | **OK**: el mod agarra el char correcto → la causa del combate NO es la resolución de char. |
| `data[0] != ctrlChar` | **CHAR EQUIVOCADO**: el mod parchea un char que el jugador no controla. FIX = resolver `PI+0x2A8`. |

---

## 4. Fix propuesto (NO aplicado — pendiente de confirmar con el DIAG)

Si el DIAG muestra `data[0] != ctrlChar` (o `MOD-usa != ctrlChar`), migrar la resolución del
char primario a `PI+0x2A8`:

```
GameWorld(0x2134110) → +0x580 (PlayerInterface*) → +0x2A8 = controlledChar (Character*)
```

Cadena de fallback razonada:
1. `PI+0x2A8` (controlledChar) — char activo real.
2. Si `+0x2A8 == 0`: `Faction(PI+0x2A0)+0x218[Faction+0x210 - 1]` (lo que el motor elige por defecto).
3. `PI+0x2B0 data[0]` (lo de hoy) — último recurso.

Sitios a tocar cuando se confirme:
- `game_types.h` `PlayerInterfaceOffsets`: añadir `controlledChar = 0x02A8`.
- `game_character.cpp::GetPlayerPrimaryCharacterDirect`: anteponer `PI+0x2A8` a `data[0]`.
- `game_types.h` `FactionOffsets`: añadir `memberCount=0x210`, `memberArray=0x218`.
- `06-game-offsets.md` §3 y §5: documentar los offsets nuevos.

> ⚠️ Validación de versión: `+0x2A8`, `+0x210`, `+0x218` son offsets de CAMPO (portables
> Steam/GOG). Las RVA `0x802520`/`0x50E9CF` son de FUNCIÓN (no portables) — solo se usan como
> evidencia, no se hookean.

---

## 5. Estado de compilación

`cmake --build build --config Release --target KenshiMP.Core` → **OK**, sin errores ni warnings.
Genera `build/bin/Release/KenshiMP.Core.dll`. **No desplegado** (por instrucción).

---

*Cruzado con: `audit-08-rama-viva-think.md` (Hipótesis A), `06-game-offsets.md` §3/§5,
`game_character.cpp:913`, `player_controller.cpp:46`. Evidencia de bytes verificada de forma
independiente con capstone 5.0.7 + pefile sobre el binario Steam 1.0.68.*
