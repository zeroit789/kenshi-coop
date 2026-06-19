# Audit 13 — ¿El base The404Studios es GOG o Steam? ¿Qué del "What Works" es real? · Verificación de offsets

> **Ámbito:** investigación READ-ONLY (sin tocar código). Cruza el repo base
> `The404Studios/Kenshi-Online` (README, código, comentarios, issues) con el binario
> **Steam 1.0.68 real** (`E:/SteamLibrary/.../kenshi_x64.exe`, ImageBase `0x140000000`),
> con **KenshiLib** (`tools/KenshiLib-reference/`) y con nuestros audits previos (01–12).
>
> **Fecha:** 2026-06-19. **Método:** verificación empírica AOB/string-xref/prólogo sobre el
> binario Steam con `pefile` (script `C:/Users/Zero/verify_base_rvas.py`).
>
> **Leyenda:** ✅ confirmado en bytes · ⛔ refutado en bytes · 🟡 inflado/aspiracional · ❓ sin concluir.

---

## TL;DR (las 3 preguntas del encargo)

1. **¿GOG o Steam el base?** → **Mayoritariamente STEAM, con contaminación GOG en 3 sitios.**
   El núcleo de RVAs/AOB del base (los 21 patrones de `patterns.h`) son de **Steam 1.0.68** —
   verificado: los 21 AOB casan EXACTO en el binario Steam, y los string-xref de las anclas caen
   en el RVA exacto que el base afirma. PERO 3 funciones (`SquadSpawnBypass 0x4FF47C`,
   `SquadSpawnCall 0x4FFA88`, `CharAnimUpdate 0x65F6C7`) están copiadas de un **"research mod" GOG**
   y el propio código las marca `(research mod: GOG ...)`. Esas 3 NO son Steam.

2. **¿El "Combat sync" del What Works es real?** → **NO, es inflado (🟡).** El mismo README, en
   Known Issues, admite `Combat damage bars don't sync (ApplyDamage hook crash)`. Y técnicamente:
   el base hookea `ApplyDamage = 0x7A33A0`, que **NO es daño** — es construcción de tooltip de
   cobertura de armadura (refutado en bytes, audit-12). El daño real es `MedicalSystem::applyDamage
   0x64F300`, que el base NO hookea. "Combat sync = Death/KO synchronized" es una afirmación de
   header sin implementación funcional detrás.

3. **¿Nuestros RVAs de combate son correctos para Steam?** → **SÍ los confirmados en audit-12.**
   Los 14 RVAs de combate/órdenes/facción de nuestro fork (`0x5CB0A0`, `0x64F300`, `0x5D20D0`,
   `0x594640`, `0x787D40`, `0x786E30`, `0x788A00`...) son **todos inicios de función válidos en
   Steam** (precedidos de padding CC/C3). Los offsets de CAMPO (cadena CE de salud, `char+0x10`
   faction, `char+0x658` ActivePlatoon, etc.) son estables entre Steam/GOG. **Ninguno de NUESTROS
   RVAs de combate es GOG.** El desfase GOG está confinado a 3 hooks heredados del base.

---

## 1. ¿GOG o Steam? — Veredicto con evidencia dura

### 1.1 El núcleo del base es STEAM 1.0.68 (verificado en bytes)

Ejecuté los 21 AOB de `patterns.h` (idénticos en base y en nuestro fork para estos) contra el
binario Steam real. **Los 21 casan EXACTAMENTE en el RVA que el base afirma:**

| Patrón | RVA base | AOB casa en RVA | prólogo | pad previo |
|---|---|---|---|---|
| CHARACTER_SPAWN | 0x581770 | ✅ OK | ✅ | ✅ |
| APPLY_DAMAGE | 0x7A33A0 | ✅ OK | ✅ | ✅ |
| CHARACTER_DEATH | 0x7A6200 | ✅ OK | ✅ | ✅ |
| AI_CREATE | 0x622110 | ✅ OK | ✅ | ✅ |
| FACTION_RELATION | 0x872E00 | ✅ OK | ✅ | ✅ |
| TIME_UPDATE | 0x214B50 | ✅ OK | ✅ | ✅ |
| GAME_FRAME_UPDATE | 0x123A10 | ✅ OK | ✅ | ✅ |
| HEALTH_UPDATE | 0x86B2B0 | ✅ OK | ✅ | ✅ |
| (otros 13) | — | ✅ OK (21/21) | ✅ | ✅ |

**string-xref independiente de versión** (la string ancla → función contenedora en Steam):

| Ancla | RVA que afirma el base | Función contenedora en Steam | Veredicto |
|---|---|---|---|
| `[RootObjectFactory::process] Character` | 0x581770 | **0x581770** | ✅ MATCH |
| `[AI::create] No faction for` | 0x622110 | **0x622110** | ✅ MATCH |
| `faction relation` | 0x872E00 | **0x872E00** | ✅ MATCH |
| `timeScale` | 0x214B50 | **0x214B50** | ✅ MATCH |

**Conclusión:** el grueso de las RVAs hardcodeadas del base (la lista de `0x00123A10` a `0x008BA700`
del README "20+ functions reversed") es **Steam 1.0.68**. La afirmación "Target: Kenshi v1.0.68
Steam/GOG x64" del README es correcta para Steam; lo que no funciona es la parte GOG colada.

> Nota de método: que un AOB con prólogo `48 8B C4` (mov rax,rsp) sea común no invalida el match —
> verifiqué unicidad. La mayoría son 1 match en .text; los pocos con 2 (CHARACTER_SPAWN,
> APPLY_DAMAGE, START_ATTACK, CHARACTER_KO) incluyen el RVA correcto y el base los resuelve por
> string-xref primario, no por AOB ciego.

### 1.2 La contaminación GOG: 3 funciones (origen del desfase)

El `GameFunctions` del base (heredado tal cual en nuestro `patterns.h`, líneas 450-452) declara
tres campos con comentario literal **`(research mod: GOG 0xXXXX)`**:

```cpp
void*  SquadSpawnBypass = nullptr;  // Squad spawn check bypass (research mod: GOG 0x4FF47C)
void*  SquadSpawnCall   = nullptr;  // Squad spawn function call site (research mod: GOG 0x4FFA88)
void*  CharAnimUpdate   = nullptr;  // Character animation update callback (research mod: GOG 0x65F6C7)
```

Y `orchestrator.cpp` los registra con AOB+RVA **de GOG**:
- `SquadSpawnBypass`: AOB `48 8D AC 24 30 FF FF FF FF 48 81 EC D0 01 00 00` + RVA `0x4FF47C`
- `CharAnimUpdate`: AOB `48 8B 8B 20 03 00 00 40 88 B3 7C 03 00 00` + **sin ancla de string** + RVA `0x65F6C7`

**Verificación en el binario Steam (lo que decide si rompen):**

| Función | RVA GOG | ¿Es prólogo en Steam? | ¿AOB GOG casa en Steam? | Dónde casa realmente |
|---|---|---|---|---|
| SquadSpawnBypass | 0x4FF47C | ⛔ NO (mid-función, sin pad previo) | ⛔ **0 matches** | en ningún sitio |
| SquadSpawnCall | 0x4FFA88 | ⛔ NO (mid-función) | (call site, sin AOB) | n/a |
| CharAnimUpdate | 0x65F6C7 | ⛔ NO (mid-función) | ✅ 1 match pero en **0x65FD27** (≠ 0x65F6C7) | `Character::resetAnimState+0xB7` |

Bytes en Steam @0x4FF47C: `00 00 00 48 89 5D D7 88 5D C7 45...` (basura, mitad de instrucción).
Bytes en Steam @0x65F6C7: `8D 4D BB FF 15 30 7B BE 01...` (mitad de instrucción).

### 1.3 ¿Esto rompe Steam? — Matizado por la salvaguarda del orchestrator

`orchestrator.cpp::TryHardcodedOffset` (líneas 669-693) tiene una **salvaguarda explícita anti-GOG**:

> *"SAFETY: Don't blindly trust GOG hardcoded RVAs on Steam … If we have a pattern for this
> function and the pattern scan already failed, the bytes don't match → wrong binary → skip."*

Consecuencia real en Steam, caso por caso:

- **SquadSpawnBypass**: AOB GOG da 0 matches → pattern scan falla → tiene `pattern` → la salvaguarda
  **rechaza** el RVA GOG `0x4FF47C` → `funcs.SquadSpawnBypass = nullptr` → `squad_spawn_hooks::Install()`
  loguea *"SquadSpawnBypass address not resolved — hook not installed"* y **NO instala** (no crashea,
  pero la función queda muerta). El doc `02-hooks.md` que lo marca **"ACTIVO"** describe el caso GOG
  o el aspiracional, **NO el comportamiento real en Steam**. 🟡 doc inflado.

- **CharAnimUpdate**: AOB GOG SÍ casa (1 match) pero en **`0x65FD27`**, no en `0x65F6C7`. El scan
  tiene éxito y resuelve a `0x65FD27`. PERO `0x65FD27` es `Character::resetAnimState` (un reset
  one-shot que casi nunca dispara), **NO el update por-frame**. El hook inline se instala en un
  sitio que prácticamente no se ejecuta → "tracked: 0" (síntoma ya documentado en
  kenshi-re-memory.md). No peligroso, pero **funcionalmente inútil** en Steam. El update real
  por-frame es `Character::updateAnim 0x65FFA0`. 🟡 doc inflado ("ACTIVO" pero no trackea nada).

- **SquadSpawnCall** `0x4FFA88`: call-site GOG, mid-función en Steam. No se resuelve.

**Implicación clave para la pregunta de Zero:** el desfase GOG-vs-Steam **existe pero está
CONTENIDO** a estos 3 hooks. Gracias a la salvaguarda, NO provocan crash sistémico en Steam
(SquadSpawnBypass simplemente no se instala). NO son la causa de "el host no puede atacar" ni de la
simulación congelada — esa causa es el gate `GW+0x8B9` (audit-12) y la ausencia de sync, no estos
offsets. Pero SÍ explican por qué el **spawn de squads** y el **tracking por animación** funcionan
peor de lo que la documentación promete.

---

## 2. ¿El "Combat sync" es real o inflado?

### 2.1 La contradicción del README es real y autoexplicativa

El README del base lista en **What Works**: `✅ Combat sync - Death/KO events synchronized`.
El MISMO README, en **Known Issues**: `❌ Combat damage bars don't sync (ApplyDamage hook crash)`
y `⚠️ AI not synchronized (local AI decisions)`.

No es ambigüedad: **es un README inflado** (generado con Claude AI, como admite su pie de página
*"Built with 🧠 by Claude AI"*). El patrón es declarar la *intención* como *logro*. El "Combat sync"
del What Works es el diseño deseado; el Known Issues es el estado real.

### 2.2 Por qué el "Combat sync" NO puede funcionar en el base (causa técnica)

Cruzando con audit-12 (verificado en bytes + KenshiLib):

- El base hookea **`ApplyDamage = 0x7A33A0`** (su `patterns.h`, ancla `"Attack damage effect"`).
  ⛔ **`0x7A33A0` NO es aplicación de daño** — es construcción del **tooltip de cobertura de armadura**
  (refs strings `"part coverage"`, `"No Armour Coverage"`; el deref `char+0x40` es un hash de string,
  no salud). El hook está sobre la función equivocada → "ApplyDamage hook crash" del Known Issues
  encaja: hookear una función enorme de UI con la firma equivocada y derefs sin sentido **crashea**.
- El daño REAL es **`MedicalSystem::applyDamage 0x64F300`** (firma `Damages*` by-ref), que el base
  **NO hookea**. Sin hookear el punto correcto, no hay forma de capturar daño autoritativo → no hay
  sync de daño/KO posible. Death (`0x7A6200`) sí lo tienen bien, pero un evento de muerte aislado sin
  el daño que lo precede no es "combat sync".

**Veredicto:** "Combat sync" en el base es 🟡 **inflado**. Lo que SÍ es real en todos los intentos
(base, KServerMod, etc.) es ver moverse al otro jugador (posiciones interpoladas). El combate
sincronizado real (daño/KO/muerte fiables entre máquinas) **no lo ha logrado nadie** — coincide con
el Estado del Arte (`Kenshi-Multiplayer-Estado-Arte.md`).

> Distinción importante que pedía Zero: una cosa es **"Combat sync"** (replicar muerte/KO entre
> jugadores) — eso NO funciona. Otra es **"el combate local del host funciona"** — eso también está
> roto en nuestro fork, pero por una causa DISTINTA: el gate de simulación `GW+0x8B9` y la falta de
> alimentación de un 2º cliente (Fase 4), no por el hook de daño. Son dos muros separados.

### 2.3 Qué del "What Works" es real vs inflado (tabla)

| Ítem del What Works del base | Veredicto | Razón |
|---|---|---|
| 2-16 player co-op (conectar a server) | ✅ real (con condiciones) | ENet funciona; pero 2º jugador en misma zona crashea (issues #82/#69/#81/#90) |
| Real-time position sync | ✅ real | interpolación es lo más sólido en todos los intentos |
| **Combat sync (Death/KO synchronized)** | 🟡 **INFLADO** | hook en `0x7A33A0` (tooltip, no daño); daño real `0x64F300` sin hookear; el propio README admite el crash |
| Building/Squad/Faction sync | 🟡 parcial/inflado | Faction sí; Squad depende de `SquadSpawnBypass` GOG que NO se instala en Steam; edificios sin spawn visual |
| Authority validation (Phases 1-6 complete) | 🟡 inflado | issue #87: símbolos `AuthorityValidator::*` declarados y nunca implementados → build roto |
| Late join fixed / Steam deadlock fixed | ✅ probablemente real | son fixes de timeout concretos (v0.3.0) |

---

## 3. Verificación de NUESTROS RVAs de combate/facción contra Steam

Los RVAs que pedía verificar el encargo. Cruzados con: binario Steam (prólogo/pad), KenshiLib
(delta +0x780 en ventana de combate, audit-12) y nuestros audits.

| RVA nuestro | Símbolo | ¿Steam válido? | Estado |
|---|---|---|---|
| **0x5CB0A0** | `Character::attackTarget(Character*)` | ✅ inicio de fn (pad previo; prólogo `mov rcx,[rcx+650h]`) | ✅ Steam (KenshiLib 0x5CA920 +0x780) |
| **0x64F300** | `MedicalSystem::applyDamage` (DAÑO REAL) | ✅ prólogo + pad | ✅ Steam (PUNTO DE HOOK correcto) |
| **0x5D20D0** | `Character::addOrder` | ✅ prólogo + pad | ✅ Steam |
| **0x594640** | `Character::getFaction` (vt+0x58) | ✅ prólogo + pad | ✅ Steam |
| **0x665580** | `CombatClass::setAttackTarget` | ✅ prólogo + pad | ✅ Steam |
| **0x791DF0** | encolador real de orden (AI+0x20) | ✅ prólogo + pad | ✅ Steam |
| **0x5CCD90** | `Character::periodicUpdate` ("AI tick") | ✅ prólogo + pad | ✅ Steam |
| **0x5CDA20** | `Character::postUpdate` ("move_tick") | ✅ prólogo + pad | ✅ Steam |
| **0x787D40** | `GameWorld::setPaused` | ✅ prólogo + pad | ✅ Steam (AOB único verificado) |
| **0x786E30** | `GameWorld::updateCharacters` | ✅ prólogo + pad | ✅ Steam |
| **0x788A00** | `GameWorld::mainLoop` (gate 0x8B9) | ✅ prólogo + pad | ✅ Steam |
| **0x7A6200** | `Character::death` | ✅ prólogo + pad | ✅ Steam |
| **0x6215B0** | `Character::isDead` getter | ✅ inicio (prólogo `movzx eax,[rcx+5BC]`) | ✅ Steam |
| **0x5C92B0** | `Character::getCombatClass` | ✅ inicio (prólogo `mov rax,[rcx+648h]`) | ✅ Steam |

**Los 14 son inicios de función reales en Steam** (todos con padding CC/C3 previo). Los 3 que no
encajan en el set mínimo de prólogos de mi script (`attackTarget`, `isDead`, `getCombatClass`) es
porque empiezan con `mov rax,[rcx+disp]` / `movzx`, prólogos no-frame ya confirmados byte a byte en
audit-12. **Ninguno de nuestros RVAs de combate es GOG.**

### 3.1 RVAs del proyecto que SÍ están mal por versión (heredados del base GOG)

| RVA | Etiqueta proyecto | Problema | Fix |
|---|---|---|---|
| **0x4FF47C** | SquadSpawnBypass | GOG. No casa en Steam → hook NO se instala (queda muerto, no crashea) | resolver el sitio Steam equivalente por RTTI/AOB nuevo, o aceptar que el spawn de squad va por NPC-hijack |
| **0x4FFA88** | SquadSpawnCall | GOG. call-site mid-función en Steam | idem |
| **0x65F6C7** | CharAnimUpdate | GOG. En Steam el AOB casa en **0x65FD27** (`resetAnimState`, casi no dispara) → "tracked:0" | hookear `Character::updateAnim 0x65FFA0` (update por-frame real) |

### 3.2 RVA del proyecto que está mal por OTRA razón (no versión, sino mala identificación)

| RVA | Etiqueta proyecto | Realidad (audit-12, bytes) |
|---|---|---|
| **0x7A33A0** | "ApplyDamage" (hook de daño del base) | ⛔ es tooltip de cobertura de armadura. Daño real = **0x64F300**. ESTE es el "ApplyDamage hook crash" del README base |
| **0x345C10** | "CharacterKO" | ⛔ es selección de objetivo de combate. No hay flag KO único (deriva del médico) |
| **0x722EF0** | "IssueOrder" (en patterns.h, ya marcado ⚠ NO CONFIRMADA) | ⛔ es UI/tooltip (MyGUI::UString). Orden real entra por addOrder 0x5D20D0 |

Estas no son por GOG-vs-Steam; son por mala identificación de función (el base las eligió por
string ancla equivocada). Ya están refutadas y corregidas en audit-12.

---

## 4. Offsets de CAMPO — estables entre Steam/GOG (no afectados por versión)

Como indica nuestra metodología, los offsets de campo de struct son **iguales** Steam/GOG (lo que
cambia son las RVAs de función). Confirmados en audit-12 + KenshiLib:

- Cadena CE salud `PlayerBase+0x2B8→+0x5F8→+0x40` (vida) / `+0x44` (stun): ✅ válida como ruta de
  LECTURA en ambas (pasa por `CharacterMemory`). Para ESCRITURA autoritativa usar ruta canónica
  `char+0x458 (MedicalSystem) → +0x1A0 → part+0x40`.
- `char+0x10` = faction · `char+0x650` = AI · `char+0x648` = body · `char+0x640` = movement ·
  `char+0x658` = ActivePlatoon · `char+0x448` = AnimationClass · `char+0x5BC` = isDead. ✅ ambas.

El `PlayerBase = 0x01AC8A90` del base es un **offset de .rdata** y se resuelve en runtime por
base+offset (es un valor de versión, pero el base lo trata como hardcoded global con validación de
doble-deref en `TryHardcodedOffset`, lo cual es robusto). No es la cadena GameWorld (esa la
resolvimos nosotros: `0x2134110`, instancia embebida — RE que el base NO tiene).

---

## 5. Conclusiones para Zero (caveman)

1. **El base es STEAM** en su núcleo (21/21 AOB casan en Steam real). NO partimos de un binario GOG
   desfasado. La hipótesis "todo falla porque el base es GOG" es **FALSA en lo general**.

2. **Pero el base se coló 3 trozos de un research mod GOG** (`0x4FF47C`, `0x4FFA88`, `0x65F6C7`).
   En Steam: SquadSpawnBypass no se instala (salvaguarda lo rechaza, no crashea), CharAnimUpdate
   trackea en el sitio equivocado (0x65FD27, casi no dispara). Son fallos REALES de funcionalidad
   (spawn de squad y tracking), pero **contenidos** — no el muro del combate.

3. **"Combat sync" del What Works = INFLADO.** El base hookea la función de daño equivocada
   (`0x7A33A0` = tooltip). El daño real `0x64F300` nunca lo tocó. El propio README lo confiesa
   en Known Issues. Nadie en el ecosistema tiene combat sync real.

4. **Nuestros RVAs de combate (audit-12) son CORRECTOS para Steam.** Los 14 verificados son inicios
   de función reales en el binario Steam. Vamos por delante del base en RE: tenemos el daño real
   (0x64F300), el gate de pausa (0x8B9), la orden real (addOrder 0x5D20D0), GameWorld 1.0.68
   (0x2134110) — cosas que el base NO tiene documentadas.

5. **Acciones concretas sugeridas** (fuera del ámbito de este audit, solo recomendación):
   - Sustituir el hook de daño del base (`0x7A33A0`) por `0x64F300` (ya planeado en audit-12 §6).
   - Re-resolver `CharAnimUpdate` a `0x65FFA0` (updateAnim real) en vez de `0x65F6C7` GOG.
   - Re-resolver o jubilar `SquadSpawnBypass`/`SquadSpawnCall` (GOG) — el spawn ya va por NPC-hijack.
   - Borrar del What Works del README las afirmaciones infladas (Combat sync, Authority 1-6) o
     marcarlas WIP, para no auto-engañarnos en futuras sesiones.

---

## 6. Fuentes y verificación

- Binario: `E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe` (Steam 1.0.68, 36,718,592 bytes,
  ImageBase 0x140000000). Verificación: `C:/Users/Zero/verify_base_rvas.py` (pefile, READ-ONLY).
- Repo base: `The404Studios/Kenshi-Online` (sha db743a1) — README.md, `KenshiMP.Scanner/include/kmp/patterns.h`
  (idéntico al nuestro en los 21 RVAs base + los 3 campos GOG), issues #82/#69/#81/#87/#90 vía MCP github.
- Nuestro fork: `KenshiMP.Scanner/include/kmp/patterns.h`, `KenshiMP.Scanner/src/orchestrator.cpp`
  (líneas 217-232 registro GOG, 638-695 salvaguarda anti-GOG), `KenshiMP.Core/hooks/char_tracker_hooks.cpp`,
  `squad_spawn_hooks.cpp`, `docs/architecture/02-hooks.md`, `audit-12-combate-ordenes-offsets.md`.
- KenshiLib: `tools/KenshiLib-reference/` (GPL-3.0, offsets/firmas como hechos).
- Estado del Arte: `Claude-Memory-Wiki/wiki/referencias/Kenshi-Multiplayer-Estado-Arte.md`.

*Generado por game-reverse-engineer (sesión 2026-06-19). Verificación empírica en bytes sobre el
binario Steam de los 21 AOB del base (21/21 casan = Steam), los 3 RVAs GOG (0/3 válidos en Steam,
SquadSpawnBypass 0 matches / CharAnimUpdate casa en 0x65FD27 ≠ 0x65F6C7), y los 14 RVAs de combate
de nuestro fork (14/14 inicios de función Steam válidos).*
