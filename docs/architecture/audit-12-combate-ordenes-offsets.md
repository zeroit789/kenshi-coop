# Audit 12 — Capa de Combate, Órdenes (Tasker/GOAP) y Offsets · Kenshi Steam 1.0.68

> **Ámbito:** RE READ-ONLY del binario `kenshi_x64.exe` (Steam 1.0.68 "Newland",
> ImageBase `0x140000000`). Consolida la capa de órdenes (Tasker/GOAP/AI), el sistema
> de daño/KO/muerte y verifica offsets pendientes cruzando con **KenshiLib** (GPL-3.0,
> usado como segunda fuente de offsets/firmas — hechos, no código).
>
> **Fecha:** 2026-06-19. **Herramienta:** iced-x86 + pefile (capstone bloqueado por WDAC).
> Helper de RE: `C:\Users\Zero\ke_re.py`.
>
> **Leyenda:** ✅ confirmado por bytes/RTTI/string · ⛔ refutado por bytes ·
> ❓ no concluyente / sin segunda fuente · 🟡 derivado (delta de versión).

---

## 0. Metodología y hallazgo metodológico transversal

### 0.1 La vtable de Character usa THUNKS JMP (ILT / incremental linking)

La vtable de `Character` (RVA `0x16F9EB8`, confirmada por RTTI `.?AVCharacter@@`) **no
contiene punteros directos a las funciones**: cada slot apunta a un trampolín
`jmp <func_real>` en la zona baja de `.text` (Import/Incremental Linking Table). Para
resolver un slot hay que leer el qword y desensamblar 1 instrucción JMP:

```
vt+0xE8 -> thunk @0xB91F  ->  jmp 0x5CCD90   (función real)
vt+0x268 -> thunk @0x37D94 ->  jmp 0x5CDA20
```

Esto explica por qué leer el qword del slot crudo daba RVAs basura (`0xB91F`) en intentos
previos. **Todo el código de resolución de vtables del mod debe seguir el JMP**, no tomar
el qword del slot como destino final.

### 0.2 Delta de versión KenshiLib → Steam 1.0.68 = **+0x780** (en la región de combate)

KenshiLib resuelve por símbolo sobre una **versión base distinta** de 1.0.68. Verificando
15 slots de la vtable de Character (todos con offset numérico conocido en `Character.h`),
el delta RVA es **constante +0x780** en la región `.text` ≈ `0x5C0000–0x630000`
(Character / CombatClass / AI / órdenes):

| vtable off | nombre (KenshiLib) | RVA KenshiLib | RVA Steam | delta |
|---|---|---|---|---|
| +0x228 | init | 0x620A10 | 0x621190 | +0x780 |
| +0x268 | postUpdate | 0x5CD2A0 | 0x5CDA20 | +0x780 |
| +0x270 | pausedUpdate | 0x5C73A0 | 0x5C7B20 | +0x780 |
| +0x2C8 | getMovementSpeedOrders | 0x5C7C90 | 0x5C8410 | +0x780 |
| … (15/15) | … | … | … | **+0x780** |

> ⚠️ El delta **NO es global**. Fuera de esa ventana (p.ej. zona de Tasks `0x33xxxx`/`0x34xxxx`,
> o funciones médicas `0x64xxxx`) cae a mitad de instrucción. Para esas regiones se reverificó
> por **prólogo + string + RTTI**, nunca por delta a ciegas. Las vtables de Task se resolvieron
> por RTTI (método fiable e independiente de versión).

Implicación práctica: **se puede mapear cualquier RVA de KenshiLib a Steam 1.0.68 sumando
0x780 dentro de la ventana de combate** — pero validando el prólogo resultante.

---

## 1. CAPA DE ÓRDENES / Tasker / GOAP / AI

### 1.1 La orden del jugador entra por `Character::addOrder` ✅

**`Character::addOrder` Steam = `0x5D20D0`** (KenshiLib `0x5D1950` + `0x780`). Firma
(`__fastcall`):

```cpp
void Character::addOrder(Building* dest, TaskType t, RootObject* subject,
                         bool shift, bool clear, const Ogre::Vector3& location);
//   rcx=this(Character*)  rdx=dest  r8d=t  r9=subject  [rsp+90]=shift  ...
```

Desensamblado clave (prólogo + gate de permiso de orden):

```asm
0x5D20D0: mov [rsp+10h],rbx ; mov [rsp+18h],rsi ; push rdi/r12/r14
0x5D20F8: test r12b,r12b               ; r12 = shift
0x5D20FB: je 0x5D2187                  ; si !shift -> rama replace
0x5D2101: mov rax,[rcx]                ; vtable del Character
0x5D2104: call [rax+58h]               ; Character::getFaction()  (= 0x594640, resuelve char+0x10)
0x5D2107: cmp qword [rax+250h],0       ; Faction+0x250 = isPlayer (PlayerInterface*)
0x5D210F: je 0x5D2187                  ; si la faction NO es del jugador -> no acepta orden
0x5D2111: mov rax,[rbx+650h]           ; AI*  = char+0x650
0x5D2121: mov rcx,[rax+2A8h]           ; obj  = AI+0x2A8  (controlador de órdenes, tiene vtable)
0x5D2128: mov rax,[rcx] ; call [rax+8] ; obj->vmethod1() -> bool "puede recibir orden ahora"
```

**Conclusión:** `addOrder` valida (a) que el char pertenezca a la facción del jugador
(`Character::getFaction()` vt+0x58 → `Faction+0x250 isPlayer != null`) y (b) un gate de
permiso por `AI+0x2A8->vtable[+8]`. Esto cierra la duda histórica del proyecto sobre por
qué el motor rechaza órdenes si `char+0x10` (faction) es incorrecta: **`addOrder` lo
comprueba explícitamente**.

### 1.2 `Character::getFaction()` = vt+0x58 = `0x594640` ✅ (desambigua sección 4)

```asm
0x594640: ... mov rax,[rcx+10h]   ; char+0x10 = faction
          test rax,rax
          jne <devuelve faction>
          mov rax,[14212FEF0h]    ; fallback: faction global por defecto
```

Es un método de `Character` (no de otra clase) que **devuelve `Faction*`** resolviendo
`char+0x10` con fallback al global `0x212FEF0`. El proyecto lo etiquetaba "construye string
faction/global"; en realidad **resuelve/devuelve la facción** (el string es secundario).

### 1.3 `AI+0x20` = cola de tareas/órdenes (AITaskSystem) ✅ por inferencia de bytes

`addOrder`, `attackTarget` y los backends dereferencian sistemáticamente
`*(char+0x650 [AI*] + 0x20)` como el `this` de TODAS las operaciones de cola:

```asm
mov rax,[char+650h]   ; AI*
mov rcx,[rax+20h]     ; *(AI+0x20) = OrderList / AITaskSystem  <-- this de la cola
```

Estructura observada en `*(AI+0x20)`:

| Offset | Campo (inferido) |
|---|---|
| `+0x78` | count (u32) de órdenes/tasks en cola |
| `+0x80` | puntero al array de `Task*` (cada elemento con vtable; slot[0]=dtor) |
| `+0x2D8..+0x2E8` | parámetros de la orden en curso (target id + Vec3 location) |
| `+0x428..+0x440` | registro de la orden construida por el encolador `0x791DF0` |

> ❓ **`AI+0x20` se confirma por USO, no por RTTI.** KenshiLib no documenta la struct interna
> de `AI`, así que el nombre "AITaskSystem" sigue siendo nomenclatura del proyecto. Lo que SÍ
> es hecho-en-bytes: `AI+0x20` es el objeto sobre el que opera la cola de órdenes. El dato
> previo del proyecto (`AI+0x20 = AITaskSystem`) queda **corroborado funcionalmente**.
>
> **Ojo:** `AI+0x2A8` es un objeto DISTINTO (gate de permisos de orden, §1.1), no la cola.

### 1.4 Funciones de la cola de órdenes ✅ / candidatos del proyecto ⛔

| Símbolo | RVA Steam | Estado | Evidencia |
|---|---|---|---|
| **encolador/constructor de orden** | **`0x791DF0`** | ✅ | llamado desde addOrder@`0x5D2204` con `rcx=*(AI+0x20)`; construye registro en +0x428..+0x440 |
| set-params de orden (target/Vec3) | `0x599970` / `0x5966A0` | ✅ | escriben +0x2D8..+0x2E8 |
| `clearOrders` (vacía la cola) | `0x507530` / `0x5074A0` | ✅ | recorren `[*(AI+0x20)+0x80][i]`, count `[+0x78]`, dtor por slot, `[+0x78]=0` |
| backend "shift/append" | `0x5C8DA0` | ✅ | rama `shift==true` (addOrder@`0x5D217D`) |
| backend "normal/replace" | `0x5D1940` | ✅ | rama `shift==false`; valida `TaskType ∈ {4,5,0x10,0x15}` |
| ~~addTask~~ `0x510D60` | — | ⛔ **REFUTADO** | opera sobre `this+0x1B8`, no sobre AI; no se llama desde addOrder |
| ~~encolador~~ `0x269240` | — | ⛔ **REFUTADO** | opera `rcx+0x170/+0x178/+0x190`; `this`≠AI |
| ~~factory Task~~ `0x507070` | — | ⛔ **REFUTADO** | getter trivial `return [[this+0x10]+0xF8]` (bytes: `mov rax,[rcx+10h]; mov rax,[rax+0F8h]; ret`) |

### 1.5 `Character::attackTarget(Character* who)` = `0x5CB0A0` ✅

(KenshiLib `0x5CA920` + `0x780`.) Orden de ataque directa:

```asm
0x5CB0A0: test rdx,rdx ; je <salir>          ; who != null
0x5CB0BD: mov rcx,[rcx+650h]                 ; AI* = char+0x650
0x5CB0C9: mov eax,[rdx+60h] / [rdx+64h] / [rdx+68h]  ; lee who+0x60..0x6C (pos/handle del target)
0x5CB0D6: add rcx,78h                        ; opera sobre AI+0x78
```

### 1.6 Vtables de Tasker y Task (resueltas por RTTI) ✅

Los RVAs previos del proyecto (`Tasker 0x1CF6F50`, `Task_MeleeAttack 0x1CF7350`, …) son los
**strings `.?AV...@@` dentro del TypeDescriptor**; el TypeDescriptor real empieza **0x10
antes** y el `CompleteObjectLocator` (sig=1, x64) referencia el TD por **RVA**.

| Clase | TypeDescriptor (RVA) | COL (RVA) | **vtable (RVA)** |
|---|---|---|---|
| Tasker | `0x1CF6F40` | `0x18203A0` | **`0x16BDC68`** |
| Task_MeleeAttack | `0x1CF7340` | `0x18212E0` | **`0x16BE448`** |
| GOAPTaskMgr | (RTTI) | `0x181FAEC` | `0x16BC1D8` (solo 2 slots; no es un Tasker) |

**Vtable Tasker `0x16BDC68`** (slots → RVA real tras seguir thunk JMP):

| Slot | Método (Tasker.h) | RVA Steam | Nota |
|---|---|---|---|
| +0x00 | `~Tasker` | `0x32DEF0` | stub base |
| +0x08 | `startAction(CharBody*)` | `0x32DEC0` | stub base (override en derivadas) |
| +0x10 | **`runAction(CharBody*)` = 0 (PURO)** | `0x7448F0` | `call _purecall` ✅ |
| +0x18 | `endAction(CharBody*)` | `0x32DED0` | stub base |
| +0x20 | `taskSaysItsFinished(Character*)` | `0x32DEE0` | stub base |

**Vtable Task_MeleeAttack `0x16BE448`** (overrides reales):

| Slot | Método | RVA Steam |
|---|---|---|
| +0x00 | `~Task_MeleeAttack` | `0x334310` |
| +0x08 | `startAction` | `0x3341F0` |
| +0x10 | `runAction` | `0x34CA40` |
| +0x18 | `endAction` | `0x3342D0` |
| +0x20 | `taskSaysItsFinished` | `0x334230` |

Campos del `Tasker` (KenshiLib `Tasker.h`, offsets portables): `priority@+0x8`,
`subject(hand)@+0x10`, `weight@+0x30`, `currentSubTarget(hand)@+0x38`, `location(Vec3)@+0x58`,
`startTime@+0x64`, `endTime@+0x68`, `taskData(TaskData*)@+0x70`. ✅ por KenshiLib.

### 1.7 El "AI tick" = `Character::periodicUpdate` `0x5CCD90` ✅ (corrige etiqueta)

Lo que el proyecto llamaba "AI tick pesado [vt+0xE8]" es **`Character::periodicUpdate`**
(KenshiLib `0x5CC610` + `0x780` = `0x5CCD90`; prólogo `mov rax,rsp`). El `update()` principal
es `0x5CF130` (vt+0xE0). Flujo verificado:

```asm
0x5CCE24: cmp [rdi+5BCh],sil   ; sil=0 ; gate isDead (char+0x5BC)
0x5CCE2B: je 0x5CD1C0          ; VIVO -> rama char vivo
... rama 0x5CD1C0:
0x5CD1C5: movss [rdi+0D8h],xmm0       ; escribe char+0xD8 (_lightLevel/caché)
0x5CD1CD: cmp [rdi+0DCh],sil          ; char+0xDC (needsLightLevel)
0x5CD254: call 0xA0AF10               ; commit a GameWorld (rcx=GameWorld 0x2134110, r8=estado)
0x5CD270: mov r11,[rdi+448h]          ; char+0x448 = AnimationClass*  (KenshiLib confirma)
0x5CD277: mov rcx,[r11+0E8h]          ; +0xE8 = objeto Task/estado activo
0x5CD27E: mov rax,[rcx] ; call [rax+10h] ; vt+0x10 = runAction(...)  <-- EJECUTA el Task activo
```

**El think/ejecución del Task activo entra por `*(char+0x448 [AnimationClass*] + 0xE8)->vt[+0x10]`**
(que es justamente el slot `runAction` del Tasker). Para forzar/sincronizar una acción de
combate, la vía limpia es **preparar el target** (`attackTarget 0x5CB0A0` o
`CombatClass::setAttackTarget 0x665580`) y dejar que el tick la ejecute, NO fabricar el Task
a mano (requeriría un `this` de Task ya construido por `0x791DF0`).

### 1.8 CombatClass ✅

| Símbolo | RVA Steam | Estado | Nota |
|---|---|---|---|
| `CombatClass` se obtiene de | `*(char+0x648 [body] + 8)` | ✅ | `getCombatClass 0x5C92B0`: `mov rax,[rcx+648h]; mov rax,[rax+8]; ret` |
| `CombatClass::setAttackTarget` | `0x665580` | ✅ | opera `[this+0x290]` (currentTarget); AddRef/Release vía `0x1D0BB` |
| `CombatClass::go` | `0x60CC50` | ✅ | toca +0x168/+0x210/+0x214/+0x218 |
| `currentTarget` | `+0x290` | ✅ | confirmado en bytes (setAttackTarget) |
| `combatModeActive @+0x130`, `_isAttacking @+0x140` | — | ❓ | KenshiLib los lista pero NO se observan en setAttackTarget/go; sin 2ª fuente en bytes |

---

## 2. SISTEMA DE DAÑO / KO / MUERTE

### 2.1 ⛔ `0x7A33A0` NO es ApplyDamage — es construcción de tooltip de cobertura

**Corrección crítica.** `0x7A33A0` (que el proyecto hookeaba como "ApplyDamage") es una
función enorme (~6.9 KB, un solo RET) que **construye los strings de tooltip de cobertura de
armadura por parte del cuerpo**. Referencia (verificado, `lea rip`): `"part coverage"`
(`0x1714E60`, ×4), `"Coverage"` (`0x1715080`), `"No Armour Coverage"` (`0x1715068`),
corchetes `[`/`]`, `"Attack damage effect"` (`0x1714FB0`). El `mov r11,[rdi+40h]; movzx
eax,[r11+2B8h]` del inicio es un **hash de string** (constante `0x9E3779B9`) para indexar un
mapa de coberturas, **no** una aplicación de daño.

> El deref `char+0x40` sin null-check que reportaba el usuario **existe y es real**
> (`0x7A3415: mov r11,[rdi+40h]` sin test previo), pero pertenece a esta rutina de tooltip,
> no a la aplicación de daño. El `char+0x40` aquí es `GameData*` (backpointer), confirmado.

### 2.2 ApplyDamage REAL = `MedicalSystem::applyDamage` `0x64F300` ✅ — PUNTO DE HOOK

| RVA | Función | Firma | Estado |
|---|---|---|---|
| **`0x64F300`** | `MedicalSystem::applyDamage` | `rcx=MedicalSystem*, rdx=HealthPartStatus* part, r8=const Damages* (BY-REF), r9b=canSever, [rsp+0x60]=loadingSavestate` | ✅ |
| `0x644A70` | `HealthPartStatus::applyDamage` (hoja, inlineada) | `rcx=HealthPartStatus*, rdx=Damages*` | ✅ |
| `0x645EF0` | `MedicalSystem::_setHealth` (set absoluto por nombre) | `rcx=MedicalSystem*, rdx=std::string* bodyPart, xmm2=float valor` | ✅ |

Desensamblado clave (lectura de Damages by-ref + escritura de salud):

```asm
0x64F314: movss xmm1,[r8]       ; Damages.cut   (r8+0x0)
0x64F33E: addss xmm1,[r8+8]     ; Damages.pierce(r8+0x8)
0x64F357: mulss xmm1,[r8+10h]   ; Damages.bleedMult (r8+0x10)
...
0x64F385: movss xmm3,[rdx+40h]  ; flesh actual (part+0x40)
0x64F398: subss xmm3,xmm1       ; flesh -= daño
0x64F3A4: movss [rdx+40h],xmm3  ; <<< ESCRIBE la salud
```

> ⛔ **La firma asumida `void(target, attacker, bodyPart, cut, blunt, pierce)` con 3 floats
> sueltos está REFUTADA.** El daño entra como **`Damages*` by-ref** (struct de 20 bytes:
> `cut@0x0, blunt@0x4, pierce@0x8, extraStun@0xC, bleedMult@0x10, armourPenetration@0x14`,
> confirmado por `Damages.h` de KenshiLib).

### 2.3 Cadena de salud — ruta canónica vs ruta CE ✅

- **`char+0x2B8` = `CharacterMemory* _myMemory`** (KenshiLib `Character.h:571`), **NO un
  "healthChainPtr"**. ⛔ corrige la etiqueta del proyecto.
- **`char+0x450` = `CharStats* stats`** (PUNTERO) · **`char+0x458` = `MedicalSystem medical`**
  (INLINE, by-value) · `char+0x448` = `AnimationClass* animation`.
- **Ruta canónica del motor (salud):** `char+0x458 (MedicalSystem) → +0x1A0 (array de
  HealthPartStatus*, count en +0x198) → part+0x40 (flesh) / part+0x44 (fleshStun)`.
- **Ruta CE del proyecto** `char+0x2B8 → +0x5F8 → +0x40+part*8`: es una **ruta de LECTURA
  alternativa** que pasa por `CharacterMemory` y casualmente desemboca en el mismo float
  `flesh`. Funciona para leer, pero el motor **nunca escribe** la salud por ahí. Para sync
  autoritativa hay que leer/escribir por la ruta canónica (MedicalSystem) o hookear `0x64F300`.

**Layout `HealthPartStatus` (✅ binario + KenshiLib `MedicalSystem.h`):**
`data@0x0, whatAmI@0x8, medical@0x10, me(Character*)@0x18, side@0x20, robotLimb@0x28,
collapses@0x31, fatal@0x32, KOMult@0x34, hitChance@0x38, flesh@0x40, fleshStun@0x44,
bandaging@0x48, _maxHealth@0x54, HPMult@0x5C`.

**Layout `MedicalSystem` (✅):** `statusMap@0x8, armourList@0x48, hunger@0x60, fed@0x64,
blood@0x70, currentBleedRate@0x78, partCount@(char+)0x198, partArray@(char+)0x1A0,
character@0x3B8`.

### 2.4 KO vs muerte — desambiguado ✅ / ⛔

| Concepto | RVA / offset | Estado |
|---|---|---|
| `char+0x5BC` = **isDead** (byte: 0=VIVO, 1=MUERTO) | setter `mov byte[rcx+5BCh],1` @ `0x7A6242` | ✅ |
| `bool Character::isDead()` getter | `0x6215B0` (`movzx eax,byte[rcx+5BCh]; ret`) | ✅ |
| `Character::death` | `0x7A6200` — firma `rcx=Character* this` (**el `rdx` NO es killer aquí**) | ✅ |
| `char+0x5BD` / `char+0x5BE` = flags distintos | leídos en `0x345C10` | ✅ existen, ❓ semántica |
| ~~`CharacterKO 0x345C10`~~ | — | ⛔ **REFUTADO**: `0x345C10` es **selección de objetivo de combate** (vtables +0x160/+0x2D0/+0x3C0, compara líderes de escuadra `[+0x378]`), NO un KO |

**Estado KO (inconsciencia) real:** en Kenshi el KO NO es un byte único como `+0x5BC`. Se
**deriva del sistema médico**: `HealthPartStatus` tiene `collapses@0x31`, `fatal@0x32`,
`KOMult@0x34`; el char cae inconsciente cuando el daño acumulado por parte cruza umbrales.
`Character.h` expone `bool isDown()` (recalcula desde el médico) y
`playerWantsMeToGetUp@0xEC`. Para sync de KO autoritativo → replicar el estado médico
(flesh/stun por parte), no un flag.

---

## 3. OFFSETS VERIFICADOS (sección 3 del encargo, cruce con KenshiLib)

| Campo | Offset | Estado | Fuente |
|---|---|---|---|
| `Inventory.owner` | `+0x88` (`RootObject*`) | ✅ | KenshiLib `Inventory.h` (refuta el `+0x28` del mod) |
| `Inventory._allItems` | `+0x10` (`lektor<Item*>`) | ✅ | KenshiLib |
| `Item.quality` | `+0x11C` (float) | ✅ | KenshiLib `Item.h` |
| `Item.weight` | `+0x120` (float) | ✅ | KenshiLib |
| `Item.quantity` | `+0x12C` (int) | ✅ | KenshiLib |
| `Building.designation` | `+0xC4` (`BuildingDesignation`) | ✅ | KenshiLib `Building/Building.h` |
| `Building.residentSquad` | `+0xD0` (`hand`) | ✅ | KenshiLib |
| `Character.platoon` (squad del char) | `char+0x658` = `ActivePlatoon*` | ✅ | KenshiLib `Character.h:749` |
| `AI` (campo) | `char+0x650` | ✅ | KenshiLib + bytes (addOrder/attackTarget) |
| `body` (CharBody*) | `char+0x648` | ✅ | bytes (getCombatClass) |
| `movement` (CharMovement*) | `char+0x640` | ✅ | KenshiLib |
| `AI+0x20` = OrderList/AITaskSystem | `AI+0x20` | ❓ (uso, sin RTTI) | bytes (§1.3) |

### 3.1 ActivePlatoon vs Platoon vs UnloadedPlatoon — discrepancia RESUELTA

Son **tres clases distintas** (`Platoon.h`):

- **`Platoon`** (persistente/serializable): `me@0x78, characterCountCurrent@0xA0,
  squadType@0xA8, squadleader(hand)@0x128, ownerships@0x148, activePlatoon@0x1D8, isDead@0x1F0`.
- **`ActivePlatoon`** (cargado/activo en mundo, **es lo que apunta `char+0x658`**):
  `me(Platoon*)@0x78, characterHandles@0x80, squadleader(Character*)@0xA0,
  isPlayer(PlayerInterface*)@0xE8, isPhysical@0xF0`.
- **`UnloadedPlatoon`**: variante descargada.

> ✅ **Los offsets del proyecto para el squad del char SON correctos** (`me@0x78`,
> `squadleader@0xA0`, `isPlayer@0xE8`) — porque `char+0x658` es un **`ActivePlatoon*`**.
> El `squadleader@0x128` y `activePlatoon@0x1D8` pertenecen a `Platoon` (la entidad
> persistente), no a `ActivePlatoon`. Cadena: `char+0x658 (ActivePlatoon) → +0x78 (Platoon) →
> +0x128 (squadleader de Platoon)` si se necesita la entidad persistente.

---

## 4. RVAs NUEVAS / REFINADAS (sección 4 del encargo)

| Símbolo | RVA Steam | Estado | Evidencia |
|---|---|---|---|
| `Character::postUpdate` (proyecto: "move_tick") | `0x5CDA20` | ✅ | vt+0x268, prólogo `mov rax,rsp`; KenshiLib `postUpdate` |
| `Character::periodicUpdate` (proyecto: "think"/"AI tick") | `0x5CCD90` | ✅ | vt+0xE8 (thunk), prólogo `mov rax,rsp`; KenshiLib |
| `getCurrentState` (think principal) | `0x5CE020` | ✅ | vt+0x1D8 |
| `hasActiveTask` | `0x5E1E60` | ✅ | vt+0x1E0 |
| `Character::update` (principal) | `0x5CF130` | ✅ | vt+0xE0 |
| `Character::pausedUpdate` (rama paused) | `0x5C7B20` | ✅ | vt+0x270 |
| rama char VIVO | `0x5CD1C0` | ✅ | destino del `je` del gate +0x5BC en `0x5CCE2B` |
| `SetControlledChar` (escribe PI+0x2A8) | `0x802520` / `0x80267A` | ✅ | string `"Player now controlling: "` @`0x171AA88` |
| `Character::getFaction` (vt+0x58) | `0x594640` | ✅ | resuelve `char+0x10` con fallback global `0x212FEF0` — **es de Character, devuelve Faction*** |

---

## 5. Patrones AOB nuevos (validados, 1 match en `.text`)

```
# MedicalSystem::applyDamage  -> 0x64F300  (PUNTO DE HOOK de daño)
48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 F3 41 0F 10 08

# HealthPartStatus::applyDamage (hoja) -> 0x644A70
F3 0F 10 4A 08 F3 0F 10 05 ?? ?? ?? ?? F3 0F 10 59 40 F3 0F 59 42 04 F3 0F 58 0A
```

> ⚠️ `MedicalSystem::_setHealth` (`0x645EF0`) tiene prólogo `mov rax,rsp` (`48 8B C4 ...`)
> que NO es único (3 matches) — no anclar solo por prólogo. Usar xref de string o la cadena
> de llamada desde `0x64F300`.

`Character::addOrder` (`0x5D20D0`) y `attackTarget` (`0x5CB0A0`): de momento sin AOB propio
generado; se resuelven por delta+prólogo y por la cadena `char+0x650→+0x2A8`. Recomendable
generar AOB sobre el prólogo `48 89 5C 24 10 48 89 74 24 18 57 41 54 41 56 48 83 EC 50`
(addOrder) y validar unicidad antes de usar en producción.

---

## 6. Plan de hooks para SYNC de combate (host autoritativo)

Objetivo: el host calcula daño/KO/muerte/órdenes y los clientes los aplican.

1. **Daño (autoritativo):** hookear **`MedicalSystem::applyDamage 0x64F300`**. Da víctima
   (`rdx=HealthPartStatus* → part+0x18 = Character*`), magnitud (`r8=Damages*`) y contexto.
   Post-call, leer `part+0x40` (flesh) y `part+0x44` (stun) y replicarlos a los peers. Esto
   sustituye el hook erróneo en `0x7A33A0` (que era tooltip).
2. **Órdenes de combate:** para inyectar una orden de ataque en un peer, llamar
   **`Character::attackTarget 0x5CB0A0`** o **`CombatClass::setAttackTarget 0x665580`** (preparan
   el target; el tick lo ejecuta). NO manipular la cola `AI+0x20` a mano salvo necesidad.
3. **Muerte:** hookear **`Character::death 0x7A6200`** (firma `rcx=Character*`). Confirma
   `+0x5BC=1`. Replicar como evento autoritativo. **No escribir `+0x5BC` directamente** en los
   clientes (manda al char a rama cadáver + ~50 sitios lo tratan como fiambre).
4. **KO:** no hay flag único; replicar el estado médico (flesh/stun por parte) vía el hook de
   daño (1). El KO se deriva en cada cliente al aplicar el mismo daño.
5. **Causa raíz "simulación congelada" (Fase 4):** el siguiente probe recomendado es leer en
   runtime, sobre los chars congelados, **`char+0x448` (AnimationClass*)** y **`char+0x5BC`**.
   Si `char+0x448 == 0`, la llamada final del tick `*(char+0x448+0xE8)->vt[+0x10]` (ejecución
   del Task) no corre → personaje "vivo pero inerte". Encaja con el síntoma (reloj avanza,
   char no piensa). Ya sabemos que el gate global es `GW+0x8B9` (paused, audit previo); este
   probe discrimina el caso por-char.

---

## 7. Resumen de correcciones a la documentación del proyecto

| # | Antiguo | Corregido (este audit) | Veredicto |
|---|---|---|---|
| 1 | `ApplyDamage = 0x7A33A0` | `0x7A33A0` = tooltip de cobertura; **daño real = `MedicalSystem::applyDamage 0x64F300`** | ⛔→✅ |
| 2 | firma daño `(target,attacker,bodyPart,cut,blunt,pierce)` 3 floats | `Damages*` **by-ref** (struct 20B) | ⛔ |
| 3 | `char+0x2B8 = healthChainPtr1` | `char+0x2B8 = CharacterMemory* _myMemory`; salud canónica = `char+0x458 (MedicalSystem)` | ⛔→✅ |
| 4 | `CharacterKO = 0x345C10` | `0x345C10` = selección de objetivo de combate; **no hay flag KO único** (deriva del médico) | ⛔ |
| 5 | "AI tick [vt+0xE8]" | = **`Character::periodicUpdate 0x5CCD90`** | aclarado |
| 6 | "move_tick" / vt+0x268 | = **`Character::postUpdate 0x5CDA20`** | aclarado |
| 7 | addTask `0x510D60` / encolador `0x269240` / factory `0x507070` | **REFUTADOS**; encolador real = `0x791DF0`, gate = `AI+0x2A8`, cola = `AI+0x20` | ⛔ |
| 8 | vtable Tasker/Task = `0x1CF6F50`/`0x1CF7350` | esos son los **TypeDescriptor strings**; vtables reales: Tasker `0x16BDC68`, Task_MeleeAttack `0x16BE448` | aclarado |
| 9 | char+0x450 stats inline | `CharStats*` **puntero** (ya estaba en audit-02, reconfirmado) | ✅ |

---

*Generado por game-reverse-engineer (sesión 2026-06-19). Verificación independiente en bytes
de los 6 hallazgos load-bearing (0x7A33A0 tooltip, 0x64F300 daño real, char+0x2B8 memory,
char+0x458 medical, vt+0x58 getFaction, candidatos refutados). Cruce con KenshiLib commit
clonado en `C:/Users/Zero/KenshiLib_tmp` (GPL-3.0, offsets/firmas como hechos). Helper RE:
`C:/Users/Zero/ke_re.py`.*
