# Game Reverse Engineer - Agent Memory

## Pattern Verification (v1.0.68)
See [pattern-verification.md](pattern-verification.md) for detailed results.

### Critical Issues Found
- **AICreate** pattern is NOT unique (2 matches). Must extend to 41 bytes. See fix below.
- **GameFrameUpdate** pattern is NOT unique (2 matches). Must extend to 45 bytes. See fix below.
- **CharacterDestroy** and **TimeUpdate** patterns are confirmed UNIQUE and correct.
- **PlayerBase** (0x01AC8A90) is in .rdata section, resolved at runtime via base+offset. No code xrefs in static file.

### Required Pattern Fixes
AICreate (current 33-token pattern has false positive at 0x000AF870):
```
OLD: "40 57 48 81 EC 90 00 00 00 48 C7 44 24 28 FE FF FF FF 48 89 9C 24 B0 00 00 00 48 8B 05 ? ? ? ?"
NEW: "40 57 48 81 EC 90 00 00 00 48 C7 44 24 28 FE FF FF FF 48 89 9C 24 B0 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 80"
```

GameFrameUpdate (current 32-byte pattern has false positive at 0x00788100):
```
OLD: "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 68 88 48 81 EC 50 01 00 00 48 C7 44 24 38 FE FF FF FF"
NEW: "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 68 88 48 81 EC 50 01 00 00 48 C7 44 24 38 FE FF FF FF 48 89 58 10 48 89 70 18 48 89 78 20 48"
```

## PE Layout (kenshi_x64.exe v1.0.68)
- .text:  VA 0x00001000, Size 0x01671412 (22.4 MB)
- .rdata: VA 0x01673000, Size 0x0054A4CB (5.3 MB)
- .data:  VA 0x01BBE000, Size 0x0058E000
- ImageBase: 0x0000000140000000

## Verified Debug Strings in .rdata
- "NodeList::destroyNodesByBuilding" at RVA 0x016C6620
- "[AI::create] No faction for " at RVA 0x016F9B30
- "Kenshi 1.0.68 - x64 (Newland)" at RVA 0x01692288
- "timeScale" at RVA 0x016A6F48
- "[RootObjectFactory::process] Character '" at RVA 0x016EEB10

## GameWorld Singleton 'ou' — RESUELTO (v1.0.68 Steam) [2026-06-17]
- **GameWorld NO es un puntero global; es una INSTANCIA ESTATICA embebida en .data.**
- **RVA instancia GameWorld (== valor de 'ou' como GameWorld*) = 0x2134110**  (modBase + 0x2134110)
- Valor antiguo 0x2131020 (de 1.0.65) era INCORRECTO en 1.0.68. delta = +0x30F0.
- Metodo (analisis estatico con iced-x86, capstone bloqueado por WDAC):
  1. Vtable de GameWorld confirmada por RTTI: TypeDescriptor ".?AVGameWorld@@" en vtable RVA 0x1722608 (COL en vt-8 = 0x185A0F0, TD = 0x2007FD0).
  2. Constructor GameWorld::ctor = 0x874E70 (instala vtable, init campos +0x8/+0xC/+0x4A0/...). Destructor ~GameWorld = 0x86D650 (nulifica +0x4A0/+0x4A8/+0x4B0/+0x4B8/+0x580/+0x790/+0x8B0/+0x8C0 -> coinciden con GameWorld.h).
  3. Game loop: en 0x82B8C8 hace `lea rcx,[0x2134110]; call 0x26724` (= GameWorld::mainLoop_GPUSensitiveStuff, unico virtual, slot0). El `lea` (no `mov`) prueba que es objeto embebido, no puntero.
  4. 513 sitios en .text hacen `lea reg,[0x2134110]` para pasar la instancia como `this`.
  5. Campos verificados por conteo de xref absoluto (instancia + offset): player @0x2134690 (609 refs), zoneMgr @0x21349C0 (284), theFactory @0x21345B0 (98), factionMgr @0x21345B8 (67), navmesh @0x21345C0 (130), nodeList @0x21345C8 (62).
  6. NO existe ningun puntero en .data/.rdata que contenga 0x2134110 -> confirmado: no hay variable `GameWorld* ou` separada.
- **IMPLICACION PARA EL MOD**: validateGameWorld()/consumidores hacen `gwObj = *candidateAddr` (tratan GameWorldSingleton como GameWorld**). Con instancia directa eso es INCORRECTO. Dos arreglos posibles:
  - (A) GameWorldSingleton = base + 0x2134110 y tratarlo como GameWorld* DIRECTO (quitar el primer deref: gwObj = candidateAddr, NO *candidateAddr).
  - (B) Mantener el deref del mod solo si se apunta a un GameWorld** real (no existe aqui) -> no aplicable.
- Strings ancla 1.0.68: 'dayTime' y '[GameWorld::' NO existen (por eso StringXref del mod falla). Si existen: 'timeScale' 0x16A6F48, 'zone.%d.%d.zone' 0x16C4CC0, 'Kenshi 1.0.68' 0x1692288.
- __security_cookie = RVA 0x21173C8 (patron `48 8B 05 dd dd dd dd 48 33 C4`). NO confundir con singletons.

## MovRaxRsp Fix (SOLVED)
See [mov-rax-rsp-fix.md](mov-rax-rsp-fix.md) for design details.
- **Problem**: `mov rax, rsp` prologue functions crash when hooked via MinHook trampoline
- **Solution**: Two-part ASM stub approach in `mov_rax_rsp_fix.cpp`:
  1. **Naked detour**: Captures RSP at hook entry (before C++ prologue), JMPs to C++ hook
  2. **Trampoline wrapper**: Swaps RSP to captured value, pushes return addr, JMPs to tramp+3
- HookManager auto-applies fix for any function starting with `48 8B C4`
- Patches MinHook's relay to route through naked detour
- **All previously disabled hooks re-enabled**: GameFrameUpdate, Combat, Inventory, Building, Faction

## Building/Inventory/Faction/Squad Class Analysis
See [class-analysis-building-inventory-faction-squad.md](class-analysis-building-inventory-faction-squad.md).
Key findings:
- **CRITICAL BUG**: inventory_hooks passes inventory ptr (not owner char ptr) to GetNetId()
- Faction +0x08 = factionId, hardcoded in hook but missing from FactionOffsets
- SquadAddMember at 0x00928423 is mid-function (not aligned), hooking may be fragile
- ALL offset tables for these classes are UNVERIFIED guesses
- VTableScanner never applied to Building/Inventory/Faction/Squad classes

## Simulación de personajes / AI tick — CAUSA RAÍZ Fase 4 (combate/recuperación) [2026-06-18]
RE del binario Steam 1.0.68 (desensamblado de bytes verificado). Resuelve por qué atacar,
levantarse de cama, recuperarse de KO y regen de stun NO progresan en el host aunque
caminar/hablar/sigilo SÍ y gameSpeed=1.0, paused=0.

**setPaused 0x787D40 está COMPLETO y correcto** — no hay 4º subsistema ni flag "AI paused".
Pausa Physics/Audio/Birds (cada uno sub+0xB8 bool / sub+0xC0 double) vía listener 0x78AC50.
La sim de personajes NO la gobierna el bool +0x8B9 sino el dt derivado de gameSpeed GW+0x700.

**Cadena del game loop (todo CONFIRMADO en bytes):**
- `GameWorld::mainLoop = 0x788A00` — computa dt maestro: `mulss xmm5,[rsi+0x700]` (×gameSpeed).
  Escribe gScaledDt en .data: gRealDt @0x2133790, gScaledDt @0x2133794, gScaledDt_A @0x2133798.
  Check freeze en +0x4B (RVA 0x788AAB): `cmp byte[rsi+0x8B9],0; je` → si !=0 anula gScaledDt_A.
- `GameWorld::updateCharacters = 0x786E30` — simulador de tareas. DOS bucles sobre la lista de
  chars activos (GW+0x768 data / +0x770 count / +0x788 array de Character*):
  - **Bucle 1** (AI tick): por char incrementa timers char+0xC8/+0xCC con gScaledDt SIEMPRE.
    Luego, SOLO si hay presupuesto de slots (round-robin), llama `[vtbl+0xE8]` = **AI tick**
    (combate, Jobs, levantarse, recuperar KO, regen stun). El presupuesto lo gobiernan 4
    contadores globales .data: 0x2132EC8 / 0x2132ECC / 0x2132ED0 / 0x2132ED4 (frame cursor,
    se incrementa al final de updateCharacters). Flag por-char char+0xE4 (LOD) decide si además
    se hace `[vtbl+0xE0]` (update completo) o solo regen barato (char+0xC0).
  - **Bucle 2**: por char `[vtbl+0x268]` = update de movimiento/física/render — INCONDICIONAL.
- Guards de reentrancia: **GW+0x8B8** (=1 al entrar a updateCharacters, =0 al salir), GW+0x749.

**POR QUÉ el síntoma:** caminar/hablar/sigilo van por vtbl+0xE0 y el bucle 2 (vtbl+0x268) que
corren cada frame. Atacar/levantarse/recuperar van por vtbl+0xE8 (AI tick), time-sliced por los
contadores .data. Si esos contadores se congelan (frame counter 0x2132ED4 sin avanzar) o el guard
GW+0x8B8 queda pegado en 1 (salida SEH temprana de un hook a mitad de updateCharacters → 2ª pasada
hace early-skip), NINGÚN char recibe AI tick → tareas progresivas congeladas, movimiento no.

**Verificación en runtime (DIAG-SIM en core.cpp Step 0):** loguea ctrA/B/C, frameCtr+advancing,
busyGuard(+0x8B8), reentry(+0x749), activeChars(+0x770), char+0xE4 del primario. Discrimina:
frameCtr FROZEN = contadores congelados; busyGuard pegado en 1 = guard atascado (FIX-SIM lo limpia
si lleva ≥120 ticks en 1). char+0xE4==0 = char en LOD barato (no recibe update completo).

**RVAs nuevos confirmados (Steam 1.0.68, ImageBase 0x140000000):**
| RVA | Función |
|---|---|
| 0x788A00 | GameWorld::mainLoop (dt maestro + check freeze 0x8B9) |
| 0x788AAB | check freeze `cmp byte[GW+0x8B9],0` |
| 0x786E30 | GameWorld::updateCharacters (simulador AI/tareas, `mov rax,rsp`) |
| 0x722EF0 | issueOrder/setJobTarget — orden REAL del jugador (clic→Job). `(char,target,bool now)` |
| 0x787D40 | GameWorld::setPaused (setter oficial, 3 subsistemas) |
| 0x78AC50 | setter de pausa de UN subsistema (sub+0xB8 bool, sub+0xC0 double) |
| 0x724880 | broadcaster observer de pausa (lista [this+0x150], count +0x158) |
| 0x65FD27 | sitio del AOB CharAnimUpdate GOG (Character::resetAnimState +0xB7) — one-shot, NO por-frame |
| 0x65FFA0 | Character::updateAnim (update de animación por-frame real) |

**OJO CharAnimUpdate:** el AOB GOG `48 8B 8B 20 03 00 00 40 88 B3 7C 03 00 00` SÍ casa en Steam
pero en RVA 0x65FD27 (Character::resetAnimState, un reset one-shot que casi no dispara), NO el
update por-frame. El fallback RVA GOG 0x65F6C7 cae MID-FUNCIÓN en Steam (roto). Por eso "tracked:0":
el hook se instala en un sitio que casi nunca se ejecuta. Para tracking real habría que hookear
0x65FFA0 (updateAnim por-frame). Es síntoma secundario, NO la causa del combate.

**StartAttack:** el string "StartAttack"/"beginAttack"/etc NO existe en el binario. La función
0x7B2A20 que el mod hookeaba como "StartAttack" es "Cutting damage calc" (NO la orden). La orden
real del jugador es 0x722EF0 (IssueOrder). El DIAG ahora hookea 0x722EF0 (slot "StartAttack" en
HookManager por compat). Si dispara al clicar enemigo → la orden llega (problema en AI tick abajo).

## Combate congelado — CAUSA RAÍZ REAL (doble verificación de bytes) [2026-06-18]
Refuta las hipótesis previas (+0x8B8 y round-robin). RE de bytes con capstone, Steam 1.0.68.

**GATE de simulación = GW+0x8B9** (NO +0x8B8). En `mainLoop 0x788A00`, justo antes de
llamar a updateCharacters, en **0x788FF5**: `cmp byte[rsi+0x8B9],0 ; jne 0x789006`.
- +0x8B9 == 0 → `call 0x4B664` (thunk→updateCharacters 0x786E30) → bucle 1 ejecuta el AI
  tick `[vtbl+0xE8]` (combate/Jobs/levantarse/recuperar KO/regen stun).
- +0x8B9 != 0 → SALTA updateCharacters, llama rama "paused" 0x787230 → solo `[vtbl+0x270]`
  (tick reducido: posición/animación). **Reloj corre, chars caminan/animan pero NO atacan
  ni se recuperan.** = síntoma exacto.

**Por qué el gate se queda pegado:** setter oficial `GameWorld::setPaused 0x787D40` escribe
**`GW+0x8B9 = argBool OR (gameSpeed[GW+0x700] == 0.0f exacto)`** (en 0x787D74 `or bl,al`;
constante 0.0f en RVA 0x1681B38; comparación `==` estricta, no `<=` ni epsilon). Si el host
pausó con barra espaciadora (gameSpeed=0.0), llamar `setPaused(false)` NO basta: el OR re-pega
el flag a 1 mientras gameSpeed siga clavado en 0.0. **FIX (aplicado en core.cpp Step 0): subir
gameSpeed a 1.0 ANTES de llamar al setter cuando se va a despausar.** Solo un escritor de
+0x8B9 en TODO el binario (0x787D82, dentro del setter) — nada lo re-pausa por frame.

**Round-robin del AI tick NO es la causa (auto-recuperante):** contadores .data
0x2132EC8(claseC)/0x2132ECC(claseB)/**0x2132ED0(claseA=el de `[vtbl+0xE8]` combate)**/
0x2132ED4(frame cursor). El cursor se resetea a 0 cada frame (0x786EAA) y con N chars bajo
(host) TODOS reciben tick. Comparaciones signed pero `esi`(techo) nunca <4 (`cmovle` a
fallback 4) → imposible quedar en 0. Solo time-reparte con decenas de chars.

**+0x8B8 = PISTA MUERTA:** se ESCRIBE (=1 entra updateCharacters 0x786E6F, =0 sale 0x7871FA)
pero NO se LEE en ningún punto → inocuo, no gobierna nada. Guard real de reentrancia = GW+0x749
(leído 0x7874EA) pero gobierna altas/bajas DIFERIDAS de la lista (GW+0x750), no el AI tick.

**0x722EF0 NO es IssueOrder — REFUTADO:** construye un MyGUI::UString + aloca Ogre::Allocated
Object → es UI/GUI (tooltip/feedback de orden), no encola Jobs. Callers 0x6714A5/0x6719A8
pasan un UI manager global [0x21337D0], no un Character. La orden real del jugador (clic→Task)
entra por vtable de **Tasker/GOAPTaskMgr** que empuja un Task_* a la cola del char. RTTI
localizados: Task_MeleeAttack @0x1CF7350, Task_GetUp @0x1CF7538, Task_GetOutOfBed @0x1CF76D8,
Task_Runaway @0x1CF79E0, CombatState @0x1C93880, AttackState @0x1C93908, GOAPTaskMgr @0x1CDE7C0,
Tasker @0x1CF6F50. **RVA exacta de la orden SIN resolver** (pendiente: rastrear handler de clic
UI → método de Tasker). El hook DIAG "StartAttack/IssueOrder" quedó DESHABILITADO (no hookear UI).

**RVAs nuevos confirmados (Steam 1.0.68):**
| RVA | Función |
|---|---|
| 0x788FF5 | gate `cmp byte[GW+0x8B9],0; jne` en mainLoop (decide AI tick vs paused) |
| 0x4B664 | thunk `jmp 0x786E30` (updateCharacters) — único call desde 0x788FF7 |
| 0x787230 | rama "paused" de mainLoop (tick reducido `[vtbl+0x270]`) |
| 0x787D74 | `or bl,al` del setter (paused = arg OR gameSpeed==0) |
| 0x1681B38 | constante 0.0f comparada con gameSpeed en el setter |
| 0x786EAA | reset frame cursor 0x2132ED4=0 (round-robin auto-recuperante) |
| 0x5CCD90 | `[vtbl+0xE8]` clase A = AI tick pesado (combate/Jobs) — firma `(Character* rcx)` |

## char+0x5BC = FLAG MUERTO (NO KO/ragdoll) — CORRECCIÓN CRÍTICA [2026-06-18]
Triple verificación RE independiente (iced-x86, Steam 1.0.68). **REFUTA** la interpretación
previa del DIAG-AICHK (que decía `+0x5BC==0 → KO/ragdoll`). La realidad es la INVERSA:

- **`char+0x5BC` (byte) = flag isDead. `0 = VIVO`, `1 = MUERTO`.** CONFIRMADO.
  - Único setter en TODO .text: **0x7A6242** `mov byte[rcx+0x5BC],1`, dentro de la rutina de
    muerte **0x7A6200** (strings `"has died from blood loss/starvation"`, `"is dead"`, `"Expired"`).
  - Getter dedicado `bool Character::isDead()` en **0x6215B0** (`movzx eax,[rcx+0x5BC]; ret`).
  - ~50 lecturas `cmp byte[reg+0x5BC],0` por todo el binario (UI/selección/combate/pathfinding).
- **Gate 0x5CCE24/2B** (`cmp byte[rdi+0x5BC],sil(=0); je 0x5CD1C0`):
  - `+0x5BC==0` (VIVO) → **SALTA a 0x5CD1C0 = RAMA DEL CHAR VIVO** (reintegra a GameWorld
    0x2134110, llama vtable de IA). Aquí vive el umbral 0.75 (char+0xD8).
  - `+0x5BC!=0` (MUERTO) → cae a 0x5CCE31 = **rama CADÁVER** (catch-up de horas, +0xD0/+0x3D4/
    +0x2F8, umbrales 6.0/12.0 = decay/limpieza del cuerpo).
- **CONSECUENCIA:** el seed de char+0xD0 SOLO afecta a CADÁVERES (la rama viva salta antes de
  tocar +0xD0). NO desbloquea el combate de un char vivo. Y **escribir +0x5BC=1 MATARÍA al char**
  (lo manda a rama cadáver + ~50 sitios lo tratan como fiambre). NO TOCAR +0x5BC.
- **Rama viva 0x5CD1C0 — gate interno del "think" pesado:** corre think+commit (vtable 0x1D8/
  0x1E0, commit a GameWorld 0xA0AF10) SOLO si `char+0xD8 > 0.75` (0x5CD1EC) Y `char+0xDC != 0`
  (0x5CD1CD). Si +0xD8 no acumula >0.75 (dt de sim =0) → entra pero solo mueve, no piensa.
- **char+0x640 = `CharMovement*`** (vtable RVA 0x16FCC88, RTTI `.?AVCharMovement@@`). El gate
  del prólogo `cmp [char+0x640 → +0x24],0` NO descarta el char (no hay early-return en 0x5CCD90).

**CAUSA RAÍZ del combate congelado (re-confirmada):** el char del host está VIVO y bien; no pelea
porque NO recibe el AI tick (gate GW+0x8B9 en mainLoop / no entra en updateCharacters), NO por
+0x5BC ni por +0xD0. Discriminar en runtime con [DIAG-SIMLIST] (¿host en lista de sim?) y
[DIAG-THINK] (¿+0xD8 cruza 0.75?). El `/verify char+0x10 FAIL` es síntoma lateral (verify sobre
char no simulado), no offset mal.

**RVAs nuevos confirmados (Steam 1.0.68):**
| RVA | Función |
|---|---|
| 0x7A6200 | Character::death (rutina de muerte; pone +0x5BC=1 en 0x7A6242) |
| 0x6215B0 | `bool Character::isDead()` (getter de +0x5BC) |
| 0x5CD1C0 | rama del char VIVO (IA/combate; gate interno +0xD8 vs 0.75 en 0x5CD1EC) |
| 0x5CD1CD | gate flag char+0xDC (!=0 para pensar) |
| 0x797EC0 | sistema de sentidos/percepción (incondicional, strings "----SENSES----") |
| 0xA0AF10 | commit de acción de combate a GameWorld (encola tarea) |
| 0x16FCC88 | vtable de CharMovement (char+0x640) |

## Capa de combate/órdenes + corrección daño/KO — SESIÓN 2026-06-19 [audit-12]
RE de bytes (iced-x86, Steam 1.0.68) + cruce con **KenshiLib** (GPL-3.0, offsets/firmas como
hechos). Detalle completo en [../architecture/audit-12-combate-ordenes-offsets.md].

**HALLAZGO METODOLÓGICO 1 — vtable de Character usa THUNKS JMP (ILT):** cada slot de la
vtable (RVA 0x16F9EB8) apunta a un trampolín `jmp <real>` en .text baja, NO a la función
directa. Resolver: leer qword del slot → desensamblar 1 instr JMP → tomar destino. (Por eso
leer el qword crudo daba RVAs basura como 0xB91F en intentos previos.)

**HALLAZGO METODOLÓGICO 2 — delta KenshiLib→Steam 1.0.68 = +0x780** CONSTANTE en la región
.text ≈ 0x5C0000–0x630000 (Character/CombatClass/AI/órdenes). Verificado con 15 slots de
vtable. FUERA de esa ventana NO es fiable (reverificar por prólogo/string/RTTI). Permite
mapear RVAs de KenshiLib a Steam sumando 0x780 dentro de la ventana de combate.

**CORRECCIONES CRÍTICAS (refutaciones verificadas en bytes):**
- ⛔ **0x7A33A0 NO es ApplyDamage** — es construcción de tooltip de cobertura de armadura
  (refs strings "part coverage" 0x1714E60, "Coverage", "No Armour Coverage"). El deref
  `char+0x40` sin null-check existe pero es un hash de string para indexar coberturas.
- ✅ **Daño REAL = `MedicalSystem::applyDamage` 0x64F300** (PUNTO DE HOOK). Firma
  `(rcx=MedicalSystem*, rdx=HealthPartStatus* part, r8=const Damages* BY-REF, r9b=canSever)`.
  Escribe salud en `part+0x40` (flesh): `movss xmm3,[rdx+40h]; subss xmm3,xmm1; movss [rdx+40h],xmm3`.
  ⛔ La firma asumida con 3 floats sueltos está REFUTADA: `Damages` entra by-ref (struct 20B:
  cut@0x0/blunt@0x4/pierce@0x8/extraStun@0xC/bleedMult@0x10/armourPen@0x14, conf. KenshiLib).
- ⛔ **char+0x2B8 = `CharacterMemory* _myMemory`** (KenshiLib Character.h:571), NO "healthChainPtr".
  Salud canónica del motor = `char+0x458 (MedicalSystem inline) → +0x1A0 (array HealthPartStatus*,
  count +0x198) → part+0x40 (flesh) / +0x44 (stun)`. La cadena CE `char+0x2B8→+0x5F8→+0x40` es
  ruta de LECTURA alternativa (pasa por CharacterMemory, coincide en el float; el motor no
  escribe por ahí).
- ✅ char+0x450 = `CharStats*` (puntero, multiplicadores) · char+0x458 = MedicalSystem (inline) ·
  char+0x448 = `AnimationClass*`.
- ⛔ **CharacterKO 0x345C10 REFUTADO** — es selección de objetivo de combate (vtables +0x160/
  +0x2D0/+0x3C0, líderes de escuadra +0x378). NO hay flag KO único; el KO se deriva del médico
  (HealthPartStatus: collapses@0x31, fatal@0x32, KOMult@0x34).
- ✅ Character::death 0x7A6200: firma `rcx=Character*` (el rdx NO es killer). Pone +0x5BC=1 en
  0x7A6242. isDead getter 0x6215B0.

**CAPA DE ÓRDENES (Tasker/GOAP/AI) — confirmada en bytes:**
- ✅ **`Character::addOrder` 0x5D20D0** `(char, Building* dest, TaskType, RootObject* subject,
  bool shift, bool clear, Vec3 loc)`. Valida facción del jugador: `call [charVt+0x58]`
  (=getFaction 0x594640, resuelve char+0x10) → `cmp [Faction+0x250],0` (isPlayer). Luego gate
  `AI+0x2A8->vt[+8]`. **Esto explica por qué el motor rechaza órdenes si char+0x10 es incorrecta.**
- ✅ **`Character::attackTarget(Character* who)` 0x5CB0A0** — orden de ataque directa (deref
  char+0x650 AI* +0x78).
- ✅ **`AI+0x20` = cola de órdenes/AITaskSystem** (por uso, no RTTI): count@+0x78, array Task*@+0x80,
  params orden@+0x2D8..+0x2E8, registro@+0x428. Corrobora funcionalmente el dato del proyecto.
  OJO: `AI+0x2A8` es DISTINTO (gate de permisos).
- ✅ Encolador real = **0x791DF0** (+ setters 0x599970/0x5966A0, clear 0x507530/0x5074A0).
  ⛔ Candidatos del proyecto REFUTADOS: addTask 0x510D60 (opera this+0x1B8), encolador 0x269240
  (this≠AI), factory 0x507070 (getter trivial `[[this+0x10]+0xF8]`).
- ✅ **Vtables por RTTI:** Tasker 0x16BDC68 (runAction puro vt+0x10=0x7448F0), Task_MeleeAttack
  0x16BE448 (startAction 0x3341F0, runAction 0x34CA40). Los RVAs previos 0x1CF6F50/0x1CF7350 son
  los TypeDescriptor STRINGS, no las vtables.
- ✅ **"AI tick" = `Character::periodicUpdate` 0x5CCD90** (vt+0xE8). El think ejecuta el Task
  activo vía `*(char+0x448 [AnimationClass*] +0xE8)->vt[+0x10]` (=runAction del Tasker). Gate
  +0x5BC en 0x5CCE24 → rama viva 0x5CD1C0. Commit a GameWorld 0xA0AF10.
- ✅ "move_tick" del proyecto = `Character::postUpdate` 0x5CDA20 (vt+0x268).

**OFFSETS verificados/aclarados:** Inventory.owner+0x88, _allItems+0x10; Item quality+0x11C/
weight+0x120/quantity+0x12C; Building designation+0xC4/residentSquad+0xD0. **char+0x658 =
ActivePlatoon*** (me+0x78, squadleader+0xA0, isPlayer+0xE8 — son de ActivePlatoon, NO de
Platoon; en Platoon squadleader@0x128/activePlatoon@0x1D8). CombatClass = *(char+0x648 body +8);
setAttackTarget 0x665580 (currentTarget@+0x290).

**Plan sync combate:** hook 0x64F300 (daño autoritativo, replicar part+0x40/+0x44); órdenes vía
attackTarget 0x5CB0A0 / setAttackTarget 0x665580; muerte vía hook 0x7A6200 (NO escribir +0x5BC
en clientes); KO derivado del médico replicado. Probe Fase 4: leer char+0x448 y char+0x5BC en
chars congelados (si +0x448==0, el tick no ejecuta Task → vivo pero inerte).

**RVAs nuevos confirmados (Steam 1.0.68):**
| RVA | Función |
|---|---|
| 0x64F300 | MedicalSystem::applyDamage (DAÑO REAL — punto de hook; Damages* by-ref) |
| 0x644A70 | HealthPartStatus::applyDamage (hoja) |
| 0x645EF0 | MedicalSystem::_setHealth (set por nombre; prólogo mov rax,rsp NO único) |
| 0x5D20D0 | Character::addOrder (orden del jugador; valida faction+0x250 isPlayer) |
| 0x5CB0A0 | Character::attackTarget(Character* who) |
| 0x594640 | Character::getFaction (vt+0x58; resuelve char+0x10, fallback global 0x212FEF0) |
| 0x791DF0 | encolador real de orden (sobre AI+0x20) |
| 0x5CCD90 | Character::periodicUpdate (="AI tick"; ejecuta Task vía char+0x448→+0xE8 vt+0x10) |
| 0x5CDA20 | Character::postUpdate (="move_tick"; vt+0x268) |
| 0x5CF130 | Character::update (principal; vt+0xE0) |
| 0x5C7B20 | Character::pausedUpdate (vt+0x270) |
| 0x665580 | CombatClass::setAttackTarget (currentTarget@+0x290) |
| 0x60CC50 | CombatClass::go |
| 0x5C92B0 | Character::getCombatClass (CombatClass = *(char+0x648 +8)) |
| 0x16BDC68 | vtable Tasker (RTTI) |
| 0x16BE448 | vtable Task_MeleeAttack (RTTI) |
| ⛔ 0x7A33A0 | NO es ApplyDamage (tooltip cobertura armadura) |
| ⛔ 0x345C10 | NO es CharacterKO (selección de objetivo de combate) |

**AOB nuevos (1 match en .text):**
- MedicalSystem::applyDamage 0x64F300: `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 F3 41 0F 10 08`
- HealthPartStatus::applyDamage 0x644A70: `F3 0F 10 4A 08 F3 0F 10 05 ?? ?? ?? ?? F3 0F 10 59 40 F3 0F 59 42 04 F3 0F 58 0A`

## Hostilidad / gate de combate — herencia de relaciones del host-clon [audit-15, 2026-06-19]
RE de bytes (iced-x86, Steam 1.0.68) + cruce con KenshiLib (FactionRelations.h/.inc, Faction.h,
Enums.h, GameData.h). Resuelve por qué el FIX-HOSTILITY del defaultRelation NO bastaba.

**CAUSA RAÍZ (confirmada runtime log 11:01):** el host es un CLON 'Player 1' (10-kenshi-online.mod)
cuya FactionRelations tiene **relCount=105 entries EXPLÍCITAS, todas relation=0.00** (neutral). El
gate de combate hace `getRelationData(other)` (vtbl+0x50, alocador 0x6B4C60) que DEVUELVE NODO
no-null para esas 105 facciones → lee relation=0 → 0 > -30 → NO enemigo. El default `FR+0x60` SOLO
se consulta cuando NO hay nodo → las entries a 0 GANAN al default -100. El clon copió la ESTRUCTURA
pero NO las relaciones reales del jugador vanilla 'Nameless' (204-gamedata.base).

**⚠ CORRECCIÓN audit-16 (2026-06-19, RE confirmado byte a byte 2 veces): 0x6B2630 ES
`FactionRelations::isAlly` (TRUE = ALIADO), NO "isEnemy enriquecida".** audit-15 (abajo) la etiquetó
AL REVÉS; esa polaridad invertida hizo que el FIX-HOSTILITY-HOOK SABOTEARA el ataque (devolvía TRUE
"para marcar enemigo", pero el encolador lo lee como "es aliado → NO ataques" → host amIdle=1 siempre).
- **0x6B2630 = isAlly**: compara `relation >= +50.0f` (.rdata 0x1683170 = `00 00 48 42`); lee un FLAG
  alliance en `RelationData+0x0` (`cmp byte ptr [rax],0; jne →return TRUE`); devuelve TRUE también si
  `other == this->ownerFaction` (FR+0x8). TRUE en los tres caminos = ALIADO. Firma
  `bool __fastcall(FactionRelations* this, Faction* other)`.
- **Encolador de ataque 0x6744A0** (modo 4) tiene DOS gates, ambos isAlly, ambos con
  `test al,al; jne 0x67451E` (salida SIN encolar). Para que el ataque SE ENCOLE, AMBOS deben dar FALSE:
  1. `isAlly(attackerFaction->relations /*Faction+0x78*/, targetFaction)` → 0x6B2630.
  2. `attacker->isAlly(target, true)` → `Character::isAlly` 0x7923D0 (char vt+0x3F0).
  `Character::isAlly` NO se hookea: tras sus checks propios (misma facción @0x79241A / squad / alianza
  dinámica vía helper 0x678960, ninguno aplica host-vs-bandido) DELEGA en el mismo 0x6B2630 (vía thunk
  0x6B27B0). Para host-vs-bandido devuelve FALSE por sí solo → hookear 0x6B2630 cubre AMBOS gates.
- Las **dos** isEnemy simples 0x6B26D0 / 0x6B25D0 son BYTE-IDÉNTICAS (copias por COMDAT) y las usan
  UI/sentidos (~24 callers), NO el gate de ataque. `relation <= -30.0` (const .rdata 0x16CCD2C). NO
  leen el flag de RelationData+0x0. Son funciones DISTINTAS de 0x6B2630 (constante y signo opuestos).
- **getRelationData/getOrCreate = 0x6B4C60** (aloca el nodo). Layout FactionRelations boost:
  bucket_count@FR+0x38, element_count@FR+0x40, buckets(node**)@FR+0x58, defaultRelation@FR+0x60.
  Nodo: next@+0x0, hash@+0x8, key(Faction*)@+0x10, **VALOR RelationData empieza en nodo+0x8**
  (getRelationData devuelve nodo+8), relation@valor+0x4 → **relation@nodo+0xC** (⚠ la nota vieja
  decía nodo+0x1C asumiendo valor@nodo+0x18; reverificar si se itera el nodo a mano — el FIX-HOSTILITY
  del defaultRel NO lo usa, así que no afecta).

**SOLUCIÓN IMPLEMENTADA (FIX-HOSTILITY-HOOK en core.cpp, NO desplegada) — POLARIDAD isAlly
CORREGIDA (audit-16):** hook de 0x6B2630 = `FactionRelations::isAlly` (prólogo estándar
`48 89 5C 24 08 57`, NO mov rax,rsp). ⚠ El retorno del hook ES el de isAlly: **TRUE=ALIADO bloquea
el ataque, FALSE=no-aliado deja atacar.** Detour HÍBRIDO A→C con whitelist propia:
1. fast-path: si `this != hostFR` → original sin tocar (NPC-vs-NPC, 99% de llamadas).
2. whitelist propia: si `other == hostFaction` → **TRUE (ALIADO)** (anti auto-ataque del squad).
3A. si Nameless existe con relaciones → `original(namelessFR, other)` (hereda el isAlly real:
    no-aliado de bandidos → ataca; neutral/aliado de comerciantes → no). Reentrancia segura:
    namelessFR != hostFR → cae en fast-path.
3C. fallback si Nameless no existe → clasificar por fundamentalType (Faction+0x34): OT_BANDIT(8)/
    OT_SLAVER(7) → **FALSE (no-aliado → atacar)**; resto → original (neutral).
Cache host (hostFaction/hostFR) refrescado por el FIX-HOSTILITY; Nameless resuelto 1 vez.
Verificación en log: para host->bandido el hook debe loguear `isAlly=NO-ALIADO` (=> el host ATACA);
tras attackTarget, `amIdle` pasa a 0 y `currentTarget(CombatClass+0x290) != 0`.

**CORRECCIÓN OFFSET GameData:** **GameData.stringID @ +0x58** (Kenshi std::string, p.ej.
"204-gamedata.base"). ⛔ NO +0x18 ni +0x28 (+0x28 = name humano). Para localizar Nameless: iterar
FactionManager (modBase+0x21345B8: count@+0x08, array Faction**@+0x10) → Faction+0x240 (GameData*)
→ GameData+0x58 (stringID) == "204-gamedata.base".

**fundamentalType (Faction+0x34, CharacterTypeEnum):** OT_NONE=0, OT_LAW_ENFORCEMENT=1, OT_MILITARY=2,
OT_TRADER=3, OT_CIVILIAN=4, OT_DIPLOMAT=5, OT_SLAVE=6, OT_SLAVER=7, OT_BANDIT=8, OT_ADVENTURER=9.

**AOBs (1 match en .text salvo donde se indica):**
- isAlly 0x6B2630 (HOOKEAR): `48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 48 85 D2 75 0D 32 C0 48 8B 5C 24 30`
- Character::isAlly 0x7923D0 (vt+0x3F0, NO se hookea): `48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 41 0F B6 E8 48 8B FA 48 8B F1 48 85 D2 75 12`
- encolador 0x6744A0: `48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 41 8B E8 48 8B F2 48 8B F9 41 83 F8 04`
- isEnemy 0x6B26D0 (2 matches, COMDAT): `48 85 C0 75 07 F3 0F 10 4B 60 EB 05 F3 0F 10 48 04 F3 0F 10 05 21 A6 01 01 0F 2F C1 72 0B`

| RVA Steam 1.0.68 | Función / dato |
|---|---|
| 0x6B2630 | **`FactionRelations::isAlly` (hookear esta)** — TRUE=aliado, `rel>=+50` o flag alliance@nodo+0x0 |
| 0x7923D0 | `Character::isAlly` (char vt+0x3F0) — 2º gate; NO se hookea (delega en 0x6B2630) |
| 0x6B26D0 / 0x6B25D0 | isEnemy simples (UI/sentidos) — `rel<=-30` |
| 0x6B4C60 | getRelationData/getOrCreate (aloca nodo) |
| 0x6744A0 | encolador de ataque (modo 4) — 2 gates isAlly (0x6B2630 + 0x7923D0), ambos FALSE para encolar |
| 0x6BD4E0 | lookup GameData por stringID (manager .data 0x2134130) |
| .rdata 0x16CCD2C / 0x1683170 | const -30.0f / +50.0f |
| .rdata 0x16C4248 / 0x16C4258 | "Nameless" / "204-gamedata.base" |

## Character Class Analysis
See [character-class-analysis.md](character-class-analysis.md) for full struct layout, known offsets, and missing fields.

## CharacterSpawn / RootObjectFactory::process Verification
- RVA: 0x00581770, confirmed via pattern + .pdata + string xref
- Pattern: exact match, unique (1 hit in .text)
- .pdata: function 0x00581770-0x0058307A, size 0x190A (6410 bytes)
- String xref (LEA rdx) at RVA 0x00582324, +0xBB4 into function
- **RuntimeStringScanner::FindFunctionStart backward walk FAILS** for this function
  (2996 bytes from xref to prologue, exceeds 2048-byte limit + internal CC bytes confuse it)
- **Orchestrator TryStringXref SUCCEEDS** via .pdata-based resolution (StringAnalyzer uses
  PDataEnumerator::FindContaining, not the backward walk)
- IsPrologue() does NOT recognize "48 8B C4" (mov rax, rsp) as a prologue pattern
- Previously documented RVA 0x0089A560 in MEMORY.md was WRONG (old/stale), corrected to 0x00581770
