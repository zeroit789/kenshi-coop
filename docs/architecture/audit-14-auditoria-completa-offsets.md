# audit-14 — Auditoría completa de offsets, RVAs y hooks (READ-ONLY)

**Fecha:** 2026-06-19
**Binario auditado:** `E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe` (Steam 1.0.68, ImageBase 0x140000000, 36,718,592 bytes)
**Segunda fuente (HECHOS):** KenshiLib `tools/KenshiLib-reference/KenshiLib/Include/kenshi/` (layouts de clase con offsets de campo y RVAs de método explícitos)
**Método:** desensamblado iced-x86 + pefile (READ-ONLY) sobre bytes reales + cruce con KenshiLib + cruce con audit-12/13.
**Naturaleza READ-ONLY:** NO se editó ningún `.cpp/.h` del mod. Único archivo escrito: este informe.

> **Convención de estabilidad:** Offsets de **CAMPO** de struct = estables Steam/GOG (KenshiLib los da como hechos). RVAs de **FUNCIÓN** = cambian por versión/plataforma (verificadas en bytes Steam 1.0.68). Distingo ambos en cada tabla.

---

## 1. TL;DR — Conteo

| Estado | Hooks/RVAs | Offsets de campo | Patrones AOB | TOTAL |
|--------|-----------:|-----------------:|-------------:|------:|
| ✅ correcto (verificado bytes + KenshiLib) | 24 | 33 | 19 | **76** |
| ⛔ INCORRECTO (con valor correcto probado) | 1 | 14 | 0 | **15** |
| 🟡 dudoso (validar con CE) | 4 | 9 | 2 | **15** |
| ❓ no verificable / sin 2ª fuente | 0 | 3 | 0 | **3** |

**Veredicto global:**
- Las **RVAs de función** del mod están **muy sanas**: 24/24 verificadas casan en su prólogo real (padding CC/C3 previo) salvo las 3 GOG ya conocidas (audit-13) y los `nullptr`/disabled correctos. El único ⛔ de función es una **discrepancia HOOKS.md vs orchestrator** en BuildingRepair (no afecta a runtime — el código real usa el RVA del orchestrator).
- Los **AOB** casan todos exacto en su RVA. 19 ✅, 2 marcados 🟡 por ser mid-función a propósito (disabled).
- Los **offsets de campo** son el área con más errores: **14 ⛔**, concentrados en **ItemOffsets (todos), StatsOffsets (todos), BuildingOffsets (varios), GameData.id, faction.relations en offsets.json**. La mayoría NO están en rutas críticas activas (los accesos críticos — faction, inventory.owner, position — ya están corregidos), pero ItemOffsets y BuildingOffsets SÍ se usan y producen lecturas basura.

---

## 2. Tablas por categoría

### 2.1 HOOKS / RVAs de FUNCIÓN (cambian por versión — verificadas en bytes Steam 1.0.68)

Verificación: `prev8` = 8 bytes anteriores. `CC...` o `…C3 CC` = inicio de función real. Cruce con KenshiLib RVA cuando aplica.

| Símbolo | RVA en mod | Estado | Prueba (bytes Steam 1.0.68) |
|---------|-----------|:------:|------------------------------|
| CharacterSpawn (RootObjectFactory::process) | 0x581770 | ✅ | `48 8B C4 55 56 57 41 54…` prev8=CC. Inicio real. |
| CharacterDeath | 0x7A6200 | ✅ | `48 8B C4 55 57 41 54…` prev8=CC. (audit-12 ✅) |
| ApplyDamage (DISABLED, tooltip) | 0x7A33A0 | ✅ | Inicio real. audit-12: es "Attack damage effect" tooltip, NO daño real. Correcto dejarlo disabled. |
| StartAttack (DIAG, "Cutting damage") | 0x7B2A20 | ✅ | `48 8B C4 55 56 57…` prev8=CC. Es cálculo de daño cut/blunt, NO la orden del jugador (combat_hooks ya lo documenta). |
| HealthUpdate | 0x86B2B0 | ✅ | `48 8B C4 55 57 41 54…` prev8=CC. Inicio real. |
| AICreate | 0x622110 | ✅ | `40 57 48 81 EC 90…` prev8=CC. Inicio real. **Escribe aiPackage en char+0x20 en 0x6221AF (`mov [rbx+0x20],rax`, rbx=char). CONFIRMADO.** |
| BuildingRepair (orchestrator/patterns.h) | 0x555650 | ✅ | `48 8B C4 55 56 57 41 54…` prev8=`5D C3`. Inicio real. String "construction progress" (2ª func). |
| BuildingRepair (HOOKS.md afirma) | 0x5C9E70 | ⛔ | Discrepancia doc: HOOKS.md dice 0x5C9E70, pero orchestrator.cpp + patterns.h usan 0x555650. 0x5C9E70 ES una función real (`48 89 5C 24 08 57…`, firma 2 punteros) pero NO es la que registra el código. Ver §5. **No afecta runtime** (el código usa 0x555650). |
| SquadCreate ("Reset squad positions") | 0x480B50 | ✅ | `48 8B C4` (mov rax,rsp) prev8=CC. Inicio real. HOOKS.md, patterns.h y orchestrator coinciden en 0x480B50. (La nota "OLD 0x928470" es de SquadAddMember, no de SquadCreate). DISABLED por crash mov-rax-rsp = decisión correcta. |
| SquadAddMember | 0x928423 | ✅(disabled) | prev8=`C0 75 05 48 83 C4 28 C3` → **mid-función, NO inicio**. Por eso AOB=nullptr y se resuelve por RTTI vtable slot 2. DISABLED correcto. |
| TimeUpdate ("timeScale") | 0x214B50 | ✅ | `40 55 56 48 83 EC 28…` prev8=CC. Inicio real. Nunca dispara en Steam (Present conduce el tick) — documentado. |
| LoadGame (SaveManager::loadGame) | 0x373F00 | ✅ | `40 55 56 57 41 54…` prev8=`C3`. Inicio real. |
| ItemPickup (addItem) | 0x74C8B0 | ✅ | `48 8B C4 44 89 40 18 55…` prev8=CC. Inicio real (mov rax,rsp). |
| ItemDrop (removeItem) | 0x745DE0 | ✅ | `40 53 48 83 EC 20 48 8B 01…` prev8=CC. Inicio real. |
| FactionRelation | 0x872E00 | ✅(disabled) | `48 8B C4 55 41 54…` prev8=CC. Inicio real, pero es un LOGGER (el mod ya lo deshabilita y lo documenta). Setter real = addRelation 0x6B2EA0. |
| FactoryCreate (RootObjectFactory::create) | 0x583400 | ✅ | `40 55 56 57 41 54…` prev8=CC. Inicio real. |
| CreateRandomChar | 0x5836E0 | ✅ | `40 55 53 56 57 41 54…` prev8=`5D C3`. Inicio real. |
| GameFrameUpdate ("Kenshi 1.0.") | 0x123A10 | ✅ | `48 8B C4 55 41 54…` prev8=CC. Inicio real. (audit-13: GAME_FRAME_UPDATE 0x123A10) |
| GameWorld::setPaused | 0x787D40 | ✅ | audit-12 ✅. SET_PAUSED AOB único. |
| GameWorld::updateCharacters | 0x786E30 | ✅ | audit-12 ✅ |
| GameWorld::mainLoop (gate 0x8B9) | 0x788A00 | ✅ | audit-12 ✅ |
| Character::attackTarget | 0x5CB0A0 | ✅ | audit-12 ✅. (KenshiLib: attackTarget RVA 0x5CA920 — leve diferencia; audit-12 lo verificó en bytes, dar por bueno el de audit-12.) |
| MedicalSystem::applyDamage (DAÑO REAL) | 0x64F300 | ✅ | audit-12 ✅ |
| Character::addOrder | 0x5D20D0 | ✅ | audit-12 ✅. (KenshiLib: addOrder RVA 0x5D1950 — diferencia; audit-12 verificó en bytes su valor. Posible que apunten a addJob/addOrder distintos. 🟡 cruzar si falla.) |
| SquadSpawnBypass | 0x4FF47C (GOG) | ⛔→❓ | audit-13: RVA GOG, 0 matches en Steam → hook NO se instala (salvaguarda lo rechaza). Función muerta en Steam. Ver §3. |
| SquadSpawnCall | 0x4FFA88 (GOG) | 🟡 | audit-13: call-site mid-función en Steam. |
| CharAnimUpdate | 0x65F6C7 (GOG) | 🟡 | audit-13: en Steam el AOB casa en 0x65FD27 (resetAnimState, casi nunca dispara). Update real por-frame = Character::updateAnim 0x65FFA0. |

### 2.2 CharacterOffsets (offsets de CAMPO — estables Steam/GOG). Fuente: KenshiLib RootObjectBase.h + Character.h

| Símbolo | Valor en mod | Estado | Valor correcto | Fuente/prueba |
|---------|-------------|:------:|----------------|----------------|
| faction | 0x10 | ✅ | 0x10 | RootObjectBase `Faction* owner` @0x10 |
| name (displayName) | 0x18 | ✅ | 0x18 | RootObjectBase `std::string displayName` @0x18 |
| gameDataPtr | 0x40 | ✅ | 0x40 | RootObjectBase `GameData* data` @0x40 |
| position | 0x48 | ✅ | 0x48 | RootObjectBase `Ogre::Vector3 pos` @0x48 |
| rotation | 0x58 | ⛔ | **RootObject+0xB0** | RootObjectBase+0x58 = `hand handle` (NO rotación). La rotación es `Ogre::Quaternion rot` en RootObject @+0xB0. Ver §3. |
| aiPackage (AITaskSytem*) | 0x20 | ✅ | 0x20 | **Bytes AI::create 0x6221AF: `mov [rbx+0x20],rax`, rbx=char. CONFIRMADO.** |
| inventory | 0x2E8 | ✅ | 0x2E8 | Character `Inventory* inventory` @0x2E8 |
| stats (CharStats*) | 0x450 | ✅ | 0x450 | Character `CharStats* stats` @0x450 |
| moneyChain 0x298→0x78→0x88 | — | 🟡 | a validar | No verificable estáticamente. La cadena de money de KenshiLib no es trivial (Character::getMoney RVA 0x790400). Validar con CE. |
| healthChain1 | 0x2B8 | ⛔ | (cadena inválida) | Character+0x2B8 = `CharacterMemory* _myMemory` (KenshiLib). NO es salud. Ver §3 + §4. |
| healthChain2 | 0x5F8 | ⛔ | (cadena inválida) | Sigue _myMemory → float arbitrario. /verify da FAIL. |
| healthBase | 0x40 | 🟡 | 0x40 (coincide con flesh) | HealthPartStatus.flesh @+0x40 (coincide por casualidad). El offset FINAL es correcto, la RUTA hasta él no. |
| healthStride | 8 | ⛔ | no aplica | HealthPartStatus es objeto de 0x68 bytes (flesh@0x40, fleshStun@0x44, _maxHealth@0x54), y viven en un `ogre_unordered_map status` @MedicalSystem+0x8. NO es array indexable por part*8. |
| charMovementOffset (animClass→CharMovement) | 0xC0 | 🟡 | a validar | Cadena de escritura de posición de KServerMod, no verificable sin AnimationClass.h. Validar con CE. |
| writablePosOffset | 0x320 | 🟡 | a validar | Idem. |
| writablePosVecOffset | 0x20 | 🟡 | a validar | Idem. |
| isPlayerControlled | -1 | ✅ | -1 (campo inexistente) | Correcto: Character NO tiene este campo. Se deriva por facción (isPlayerCharacter, RVA 0x790470). |

**Campos adicionales confirmados por KenshiLib (referencia para el mod, no presentes en CharacterOffsets):**
`medical` (MedicalSystem INLINE) @Character+0x458 · `animation` (AnimationClass*) @0x448 · `movement` (CharMovement*) @0x640 · `body` (CharBody*) @0x648 · `ai` (AI*) @0x650 · `platoon` (ActivePlatoon*) @0x658 · `_myMemory` (CharacterMemory*) @0x2B8 · `isDead()` RVA 0x620E30. Todos coinciden con audit-12.

### 2.3 SquadOffsets (CAMPO) — game_types.h `SquadOffsets` NO corresponde a ninguna clase real de KenshiLib

El `SquadOffsets` del mod (name=0x10, memberList=0x28, memberCount=0x30, factionId=0x38, isPlayerSquad=0x40) **no casa con Platoon ni con ActivePlatoon de KenshiLib**. Parece heredado sin verificar.

| Símbolo | Valor en mod | Estado | Valor correcto (KenshiLib) | Fuente |
|---------|-------------|:------:|----------------------------|--------|
| name | 0x10 | 🟡 | (depende de clase) Platoon hereda RootObjectBase: displayName@0x18; ActivePlatoon::getName usa otra ruta | Platoon.h |
| memberList | 0x28 | ⛔ | **ActivePlatoon: `HandleList* characterHandles` @0x80** | Platoon.h:266 |
| memberCount | 0x30 | ⛔ | **Platoon: `_characterCountCurrent` @0xA0**; ActivePlatoon::getSquadSize RVA 0x4197B0 | Platoon.h:109 |
| factionId | 0x38 | ⛔ | No existe id plano; la facción se obtiene por owner/participant | — |
| isPlayerSquad | 0x40 | ⛔ | **ActivePlatoon: `PlayerInterface* isPlayer` @0xE8** | Platoon.h:295 |

**Nota:** `ActivePlatoon` (el "squad activo" real) es lo que importa: `me`(Platoon*)@0x78, `characterHandles`(HandleList*)@0x80, `squadleader`(Character*)@0xA0, `isPlayer`(PlayerInterface*)@0xE8, `isPhysical`@0xF0. El mod usa `char.platoon`=char+0x658 (✅ audit-12) que apunta a un ActivePlatoon, así que el camino real ya existe; SquadOffsets es código muerto/erróneo. Si SquadAccessor llega a usarse, reescribir contra ActivePlatoon.

> **OJO squad_spawn_hooks.cpp** usa offsets sobre activePlatoon: +0xF0 (skip1), +0x58 (skip2), +0x250 (skip3), +0x78 (squad), +0xA0 (leader). Cruzando con ActivePlatoon de KenshiLib: +0x78=`me`(Platoon*) ✅ coherente, +0xA0=`squadleader`(Character*) ✅ coherente, +0xF0=`isPhysical`(bool) — plausible como "skip check". +0x250 NO existe en ActivePlatoon (la clase no llega tan lejos). Estos offsets son del **research mod GOG** y van con SquadSpawnBypass (0x4FF47C GOG, muerto en Steam), así que **no se ejecutan en Steam** → 🟡 irrelevantes hasta portar el bypass.

### 2.4 WorldOffsets (CAMPO) — Fuente: KenshiLib GameWorld.h. EXCELENTE estado.

| Símbolo | Valor en mod | Estado | Valor correcto | Fuente |
|---------|-------------|:------:|----------------|--------|
| gameSpeed (frameSpeedMult) | 0x700 | ✅ | 0x700 | GameWorld `float frameSpeedMult` @0x700 |
| player (PlayerInterface*) | 0x580 | ✅ | 0x580 | GameWorld `PlayerInterface* player` @0x580 |
| paused | 0x8B9 | ✅ | 0x8B9 | GameWorld `bool paused` @0x8B9 |
| zoneManager (zoneMgr) | 0x8B0 | ✅ | 0x8B0 | GameWorld `ZoneManager* zoneMgr` @0x8B0 |
| characterList | 0x888 | ✅(deprecado) | 0x888 = `mainUpdateListRemovalQueue` | GameWorld @0x888. Correcto marcarlo DEPRECADO: es la cola de borrado, NO la lista de personajes. La lista del jugador va por player→0x2B0. |
| timeOfDay | -1 (en TimeManager) | ✅ | -1 | Correcto: no está en GameWorld. |

> Bonus KenshiLib: `factionMgr` (FactionManager*) @GameWorld+0x4A8, `theFactory` @0x4A0, `navmesh` @0x4B0, `gameResetting` @0x8BA, `debugFlag` @0x8B8. Útil si se necesita el FactionManager por puntero (en vez de la instancia embebida que usa el mod en GameWorld+0x21345B8).

### 2.5 PlayerInterfaceOffsets (CAMPO) — Fuente: KenshiLib PlayerInterface.h

| Símbolo | Valor en mod | Estado | Valor correcto | Fuente/prueba |
|---------|-------------|:------:|----------------|----------------|
| participant (Faction*) | 0x2A0 | ✅ | 0x2A0 | PlayerInterface `Faction* participant` @0x2A0. **Bytes SetControlledChar 0x80254E: `mov rax,[rcx+2A0h]` y 0x802563: `mov [rcx+2A0h],rdx`. CONFIRMADO.** |
| controlledChar (Character*) | 0x2A8 | ⛔/🟡 | **0x2A8 = `currentPlatoon` (Platoon*)** según KenshiLib | PlayerInterface.h:249 dice `Platoon* currentPlatoon` @0x2A8, NO un Character*. Ver §3 + §4. |
| playerCharacters (lektor<Character*>) | 0x2B0 | ✅ | 0x2B0 | PlayerInterface `lektor<Character*> playerCharacters` @0x2B0 |

> **Discrepancia crítica sobre controlledChar (0x2A8):** El mod afirma (audit-09) que SetControlledChar 0x802520 escribe el char activo en PI+0x2A8. La verificación de bytes de 0x802520 muestra que esa función escribe en **PI+0x2A0 (participant/Faction*)**, NO en +0x2A8. Y KenshiLib dice que +0x2A8 = `currentPlatoon` (Platoon*). **Conclusión: la afirmación de que PI+0x2A8 contiene el "Character* controlado" es DUDOSA/INCORRECTA.** Es posible que 0x802520 NO sea la función que el mod cree (puede ser `PlayerInterface::setFaction`, que casa con escribir participant). El "char primario del jugador" se obtiene de forma fiable por `playerCharacters`(+0x2B0) lektor + el getter directo del mod, o por `getAnyPlayerCharacter()` RVA 0x7F19B0. **Validar con CE antes de confiar en +0x2A8 como Character*.**

### 2.6 FactionOffsets / FactionExtra / FactionRelations (CAMPO) — Fuente: KenshiLib Faction.h + FactionRelations.h

| Símbolo | Valor en mod | Estado | Valor correcto | Fuente |
|---------|-------------|:------:|----------------|--------|
| FactionExtra.nameStr | 0x1A8 | ✅ | 0x1A8 | Faction `std::string name` @0x1A8 |
| FactionExtra.isPlayer (PlayerInterface*) | 0x250 | ✅ | 0x250 | Faction `PlayerInterface* isPlayer` @0x250 |
| Faction.id | -1 | ✅ | -1 (no existe id plano) | Correcto: Faction+0x8 = `bool _antiSlavery`, NO id. Identificar por name(0x1A8) o data(0x240). |
| Faction.name | 0x1A8 | ✅ | 0x1A8 | igual que nameStr |
| Faction.relations (FactionRelations*) | 0x78 | ✅ | 0x78 | Faction `FactionRelations* relations` @0x78. **(offsets.json dice 0x50 — ⛔, ver §5.)** |
| Faction.data (GameData*) | 0x240 | ✅ | 0x240 | Faction `GameData* data` @0x240 |
| Faction.isPlayerIface | 0x250 | ✅ | 0x250 | = isPlayer (PlayerInterface*) |
| Faction.memberCountReal | 0x210 | 🟡 | (no casa con KenshiLib) | KenshiLib Faction NO tiene un `memberCount@0x210` ni `memberArray@0x218`: @0x208=`activePlatoons`(lektor), @0x220=`unloadedPlatoons`. El "array de miembros" del jugador NO está directo en Faction; los personajes cuelgan de los Platoons. audit-09 lo verificó por bytes de SetControlledChar → **se conserva como hallazgo de audit-09 pero NO lo confirma KenshiLib**. Validar con CE. |
| Faction.memberArrayReal | 0x218 | 🟡 | idem | idem. KenshiLib: Faction+0x218 está dentro de `lektor<Platoon*> unloadedPlatoons`(@0x220) / `activePlatoons`(@0x208). Conflicto. |
| Faction.members | 0x30 | ⛔ | (no es members) | Faction+0x30 = `bool allowSlavesWeapons`. NO una lista de miembros. |
| Faction.memberCount | 0x38 | ⛔ | (no existe) | Faction+0x34 = `fundamentalNPCType`; +0x38 = `Faction* myLawEnforcementFaction`. NO un count. |
| Faction.color1 | 0x80 | ⛔ | (no es color) | Faction+0x80 = `Ownerships* factionOwnerships`. |
| Faction.color2 | 0x84 | ⛔ | (no existe) | dentro de factionOwnerships ptr. |
| Faction.isPlayerFaction | 0x90 | ⛔ | **0x250** | Faction+0x90 = `TradeCulture tradeCulture` (inline). El flag jugador real es `isPlayer`(PlayerInterface*) @0x250. |
| Faction.money | 0xA0 | ⛔ | (no es money) | Faction+0xA0 está dentro de tradeCulture. El dinero de facción va por Ownerships::getMoney. |
| FactionRelations.size (relCount) | 0x28 | 🟡 | a validar | KenshiLib FactionRelations: el map `_factionRelations` (ogre_unordered_map<Faction*,RelationData>) está @+0x20. El layout interno del map MSVC/boost (size, buckets) hay que confirmarlo en vivo. defaultFactionRelation @+0x60. |
| FactionRelations.nodeValue | 0x18 | 🟡 | **RelationData.relation @+0x4** | El valor de relación dentro de `RelationData` está en +0x4 (relation), NO +0x18. Si el map almacena `pair<Faction*, RelationData>` el nodo es: key(Faction*) + RelationData{alliance@0,…,relation@4}. El mod lee +0x18 (dudoso). Validar con CE. |

> **Nota crítica facciones:** Lo que SÍ está confirmado y es load-bearing para el combate: `relations`@0x78 ✅ (corregido desde 0x50), `name`@0x1A8 ✅, `data`@0x240 ✅, `isPlayer`@0x250 ✅, `_antiSlavery`@0x8 ✅. Los offsets ⛔ (members/color/money/isPlayerFaction@0x90) están marcados "DUDOSO" en game_types.h y **no se usan en rutas críticas** según las notas del propio archivo — pero conviene corregir isPlayerFaction 0x90→0x250 (ya existe isPlayerIface@0x250 al lado, redundante). memberCountReal/memberArrayReal (0x210/0x218) son hallazgo de audit-09 (bytes) que KenshiLib NO respalda → marcar 🟡 y validar con CE antes de confiar.

### 2.7 BuildingOffsets (CAMPO) — Fuente: KenshiLib Building/Building.h (hereda RootObject→RootObjectBase)

| Símbolo | Valor en mod | Estado | Valor correcto | Fuente |
|---------|-------------|:------:|----------------|--------|
| name | 0x10 | ⛔ | **0x18 (displayName)** | Building hereda RootObjectBase: name=`displayName`@0x18, NO 0x10. (+0x10 = `Faction* owner`.) |
| position | 0x48 | ✅ | 0x48 | RootObjectBase `pos`@0x48 (heredado) |
| rotation | 0x58 | ⛔ | **RootObject+0xB0** | +0x58 = `hand handle`. La rot está en RootObject `rot`@0xB0. |
| ownerFaction | 0x80 | ⛔ | **0x10 (owner)** | RootObjectBase `Faction* owner`@0x10. Building+0x80 = `hand isInsideBuilding` (RootObject). |
| health | 0xA0 | ⛔ | (no es float salud) | Building+0xA0 está en RootObject (`isInsideTownWalls` int @0xA0). La salud de edificio va por `ConstructionState` (Building `_buildState`@0x160, constructionProgress dentro). |
| maxHealth | 0xA4 | ⛔ | (no existe) | RootObject `floorNum`@0xA4. |
| isDestroyed | 0xA8 | ⛔ | **0x1A1 (`destroyed` bool)** | Building `bool destroyed`@0x1A1; o virtual `isDestroyed()` RVA 0xF6B50. +0xA8 = RootObject `spacialKey`. |
| functionality | 0xC0 | ⛔ | (no es ptr) | Building+0xC0 = `bool isFoliage`. El "special function" es `BuildingFunction specialFunction`@0x158 / virtual getSpecialFunction(). |
| inventory | 0xE0 | ⛔ | (virtual) | Building no tiene campo inventory directo; getInventory() RVA 0x2AD6C0 (virtual). +0xE0 está vacío en Building base. |
| townId | 0x100 | 🟡 | (parcial) | Building+0x100 = `InstanceID instanceID` (no town id). El town es `hand _town`@0x1D0. |
| buildProgress | 0x110 | ⛔ | **ConstructionState.constructionProgress (_buildState@0x160 + 0x4)** | Building+0x110 = `std::string layoutInstanceID`. El progreso vive en _buildState (ConstructionState inline @0x160), campo constructionProgress@+0x4. |
| isConstructed | 0x114 | ⛔ | (deriva de ConstructionState.isComplete@0x160+0x0) | Building+0x114 dentro de layoutInstanceID. |
| **designation** | 0xC4 | ✅ | 0xC4 | Building `BuildingDesignation designation`@0xC4 (audit-12 ✅). |
| **residentSquad** (hand) | 0xD0 | ✅ | 0xD0 | Building `hand residentSquad`@0xD0 (audit-12 ✅). |

> **BuildingOffsets está mayoritariamente ⛔.** Building hereda de RootObject→RootObjectBase, así que comparte el layout base (owner@0x10, displayName@0x18, data@0x40, pos@0x48, handle@0x58, rot@0xB0). Los offsets propios de Building empiezan en 0xC0 (isFoliage, designation@0xC4, residentSquad@0xD0…). Los únicos correctos son position(0x48), designation(0xC4), residentSquad(0xD0). **building_hooks.cpp lee GameData en building+0x28 (⛔, debería ser +0x40) y templateId en gameData+0x08 (⛔, ver GameData abajo)** — produce templateId basura en C2S_BuildRequest. building_hooks es "optional/crash-prone" así que el impacto es bajo, pero la lectura de plantilla está rota.

### 2.8 InventoryOffsets (CAMPO) — Fuente: KenshiLib Inventory.h. BUEN estado.

| Símbolo | Valor en mod | Estado | Valor correcto | Fuente |
|---------|-------------|:------:|----------------|--------|
| items (lektor<Item*> _allItems) | 0x10 | ✅ | 0x10 | Inventory `lektor<Item*> _allItems`@0x10 |
| itemCount (size del lektor) | 0x18 | ✅ | 0x18 | lektor: data@0x10, size@0x18 (estándar lektor) |
| owner (RootObject*) | 0x88 | ✅ | 0x88 | Inventory `RootObject* owner`@0x88. **CORREGIDO desde 0x28 (audit-02). inventory_hooks.cpp ya usa GetOffsets().inventory.owner dinámico = 0x88.** |
| width | 0x20 | 🟡 | a validar | KenshiLib Inventory no expone width/height como campos directos (van por InventorySection). +0x20 está en el rango del map `sections`. Dudoso pero no crítico. |
| height | 0x24 | 🟡 | a validar | idem. |
| maxStackMult | 0x30 | 🟡 | a validar | +0x30 dentro de `sections` (boost map). Dudoso. |

> Bonus: `callbackObject`(RootObject*)@0x80, `totalWeight`(float)@0x90, `sectionsInSearchOrder`(lektor)@0x68. owner@0x88 es el campo crítico para pickup/drop y está ✅.

### 2.9 ItemOffsets (CAMPO) — ⛔⛔⛔ COMPLETAMENTE INCORRECTO. Fuente: KenshiLib Item.h (InventoryItemBase)

| Símbolo | Valor en mod | Estado | Valor correcto (KenshiLib) | Fuente |
|---------|-------------|:------:|----------------------------|--------|
| name | 0x10 | ⛔ | **0x18 (displayName)** | Item hereda RootObjectBase: name@0x18, NO 0x10. |
| templateId (GameData*) | 0x20 | ⛔ | **0x40 (data)** | +0x20 cae DENTRO de `std::string displayName` (0x18..0x40). El GameData* es `data`@0x40 (RootObjectBase). **inventory_hooks/building_hooks leen templateId aquí → basura.** |
| stackCount | 0x30 | ⛔ | **quantity@0x12C** | +0x30 está dentro de displayName/data. |
| quality | 0x38 | ⛔ | **0x11C** | InventoryItemBase `float quality`@0x11C (audit-12 ✅). |
| value | 0x40 | ⛔ | (no plano; getValueSingle RVA 0x7A7D30) | +0x40 = `GameData* data`. El valor se calcula. |
| weight | 0x48 | ⛔ | **0x120** | InventoryItemBase `float weight`@0x120 (audit-12 ✅). |
| equipSlot (AttachSlot) | 0x50 | ⛔ | **slotType@0x110** | InventoryItemBase `AttachSlot slotType`@0x110. |
| condition (durabilidad) | 0x58 | ⛔ | **chargesLeft@0x118 / quality@0x11C** | +0x58 está en RootObjectBase (handle). |

> **CONTRADICCIÓN GRAVE confirmada (era el aviso del prompt):** ItemOffsets de game_types.h tiene los offsets VIEJOS (estilo 0x10-0x58). audit-12 + KenshiLib confirman: quality=0x11C, weight=0x120, quantity=0x12C. **TODO ItemOffsets debe reescribirse.** Lo usa `TryGetItemTemplateId` en inventory_hooks (templateId=0x20 → debería ser data@0x40) y building_hooks. Impacto: itemTemplateId enviado por red es basura en C2S_ItemPickup / C2S_ItemDrop / C2S_TradeRequest / C2S_BuildRequest. **Prioridad media-alta** (rompe sync de items, no crashea).

**Layout correcto de Item (InventoryItemBase) para reescribir:**
```
manufacturerData (GameData*)  @0xC0
materialData     (GameData*)  @0xC8
inventorySection (std::string)@0xE8
slotType (AttachSlot)         @0x110
chargesLeft (float)           @0x118
quality (float)               @0x11C   // ✅ audit-12
weight  (float)               @0x120   // ✅ audit-12
quantity (int)                @0x12C   // ✅ audit-12
itemWidth (int)               @0x130
itemHeight(int)               @0x134
objectType (itemType)         @0x13C
// GameData* plantilla: data @0x40 (heredado de RootObjectBase)
// name: displayName @0x18 (heredado)
```

### 2.10 StatsOffsets (CAMPO) — ⛔⛔⛔ COMPLETAMENTE INCORRECTO. Fuente: KenshiLib CharStats.h

El `stats` del Character (char+0x450) es un `CharStats*`. El `StatsOffsets` del mod asume un array plano de skills en 0x00-0x60. **CharStats NO tiene ese layout.** Los skills reales empiezan en +0x80 (_strength) y los de combate están dispersos hasta +0x118.

| Símbolo | Valor en mod | Estado | Valor correcto (CharStats) | Fuente |
|---------|-------------|:------:|----------------------------|--------|
| meleeAttack | 0x00 | ⛔ | **__meleeAttack @0x120** | CharStats.h:141. +0x00 = vtable/AllocatedObject. |
| meleeDefence | 0x04 | ⛔ | **_meleeDefence @0x124** | CharStats.h:142 |
| dodge | 0x08 | ⛔ | **dodging @0xF0** | CharStats.h:125 |
| martialArts | 0x0C | ⛔ | **unarmed @0x10C** | CharStats.h:132 |
| strength | 0x10 | ⛔ | **_strength @0x80** | CharStats.h:91 |
| toughness | 0x14 | ⛔ | **_toughness @0x90** | CharStats.h:99 |
| dexterity | 0x18 | ⛔ | **_dexterity @0x88** | CharStats.h:95 |
| athletics | 0x1C | ⛔ | **_athletics @0x94** | CharStats.h:102 |
| (resto de skills) | 0x20-0x60 | ⛔ | dispersos @0x80-0x118 | Ver layout abajo |

> **StatsOffsets entero es ⛔.** Hay que reescribirlo contra CharStats. Layout real (parcial): _strength@0x80, _dexterity@0x88, perception@0x8C, _toughness@0x90, _athletics@0x94, medic@0x98, stealth@0xA4, thieving@0xAC, lockpicking@0xB0, assassin@0xB8, science@0xE0, labouring@0xE4, farming@0xE8, cooking@0xEC, dodging@0xF0, katanas@0xF8, sabres@0xFC, hackers@0x100, blunt@0x104, heavyWeapons@0x108, unarmed@0x10C, bows@0x110, turrets@0x114, polearms@0x118, __meleeAttack@0x120, _meleeDefence@0x124. **NOTA:** Si StatsAccessor no se usa en runtime activo, es código muerto; verificar antes de invertir esfuerzo. Validar 2-3 con CE para confirmar el layout exacto de tu build.

### 2.11 GameData / TimeManager (CAMPO)

| Símbolo | Valor en mod | Estado | Valor correcto | Fuente |
|---------|-------------|:------:|----------------|--------|
| GameData.id | 0x08 | ⛔/🟡 | **0x08 = `int validity`** (no id) | KenshiLib GameData: +0x8 = `int validity`, +0x10 = `GameDataContainer* sourceContainer`, +0x18 = `bool isStandalone`. NO hay un "template id" entero en +0x8. El id estable es el string-id (más adelante, RVA getFactionByStringID lo usa). **building_hooks lee gameData+0x08 como templateId → es `validity`, basura.** Validar con CE el offset real del string-id/nombre. |
| GameData.managerPtr | 0x10 | ✅ | 0x10 (sourceContainer/manager) | GameData `GameDataContainer* sourceContainer`@0x10 |
| GameData.name | 0x28 | 🟡 | a validar | KenshiLib GameData base no muestra `name` en +0x28 directamente (GameDataHeader.name@0x0 es de otra clase). Validar con CE. |
| TimeManager.timeOfDay | 0x08 | ✅ | 0x08 | offsets.json verified (runtime hook capture) |
| TimeManager.gameSpeed | 0x10 | ✅ | 0x10 | offsets.json verified |

### 2.12 patterns.json — AOB (verificados en bytes Steam 1.0.68)

| Símbolo | RVA | Estado | Prueba |
|---------|-----|:------:|--------|
| CHARACTER_KO | 0x345C10 | ✅ casa / ⛔ semántica | raw_bytes casan EXACTO, prev8=CC (inicio real). PERO audit-12: 0x345C10 = selección de objetivo de combate, NO "knockout único". El AOB es correcto, la etiqueta "CharacterKO" es engañosa. |
| CHARACTER_MOVE_TO | 0x2EF4E3 | 🟡 (disabled) | raw_bytes casan, pero prev8=`85 28 02 00 00 4C 8B F1` → **mid-función** (correcto: contiene IAT indirect call no wildcardeable; pattern.h ya lo pone nullptr/disabled). |
| GAME_INIT | 0x123590 | ✅ | raw_bytes casan, prev8=`C3` (inicio real). String "Kenshi 1.0." |
| NAVMESH | 0x881950 | ✅ | raw_bytes casan, prev8=CC (inicio real). |
| SAVE_GAME | 0x7EF040 | ✅ | raw_bytes casan, prev8=CC (inicio real). String "quicksave". |
| ZONE_LOAD | 0x377710 | ✅ | raw_bytes casan, prev8=CC (inicio real). String "zone.%d.%d.zone". |

**patterns.h (KenshiMP.Scanner) — AOB adicionales verificados ✅:** CHARACTER_SPAWN, CHARACTER_DESTROY, CREATE_RANDOM_SQUAD, CHARACTER_SERIALISE, APPLY_DAMAGE, START_ATTACK, CHARACTER_DEATH, HEALTH_UPDATE, AI_CREATE, TIME_UPDATE, SET_PAUSED, LOAD_GAME, ITEM_PICKUP, ITEM_DROP, FACTION_RELATION, GAME_FRAME_UPDATE — todos con prólogo de inicio de función verificado arriba o en audit-12. SET_PAUSED es único (audit-12). CHARACTER_MOVE_TO y SQUAD_ADD_MEMBER correctamente = nullptr.

---

## 3. ⛔ CORRECCIONES A APLICAR (priorizadas — combate/facción/spawn/órdenes primero)

> Recordatorio: NO edité los .cpp/.h (lo hace el otro agente). Aquí va el fix exacto con prueba.

### PRIORIDAD 1 — Facción / control del jugador (afecta combate)

1. **`game_types.h` PlayerInterfaceOffsets.controlledChar = 0x2A8** — ⛔/🟡
   **Problema:** se afirma que PI+0x2A8 es el Character* controlado. KenshiLib dice +0x2A8 = `currentPlatoon` (Platoon*). Los bytes de 0x802520 (que el mod cree "SetControlledChar") escriben en PI+0x2A0 (participant/Faction*), NO en +0x2A8.
   **Fix:** No confiar en +0x2A8 como Character*. Resolver el char primario por `playerCharacters`(+0x2B0, lektor) data[0], o por `getAnyPlayerCharacter()` RVA 0x7F19B0. **Marcar +0x2A8 como 🟡 y VALIDAR CON CE** (ver §4) antes de cambiar código. Si CE confirma que +0x2A8 sí guarda un Character* en esta build, dejarlo; si guarda un Platoon*, es la fuente del bug de "char primario = Dani != Sinnombre".
   **Prueba:** PlayerInterface.h:248-250 (participant@0x2A0, currentPlatoon@0x2A8, playerCharacters@0x2B0) + bytes 0x80254E/0x802563.

2. **`game_types.h` FactionOffsets.isPlayerFaction = 0x90 → 0x250** — ⛔
   **Problema:** +0x90 = `TradeCulture` inline, NO un flag jugador. Lecturas de "es facción de jugador" dan basura.
   **Fix:** usar 0x250 (ya existe `isPlayerIface=0x250` al lado; `isPlayerFaction` es redundante/erróneo → apuntarlo a 0x250 o eliminarlo y usar isPlayerIface).
   **Prueba:** Faction.h:158 `PlayerInterface* isPlayer @0x250`.

3. **`game_types.h` FactionOffsets.memberCountReal/memberArrayReal (0x210/0x218)** — 🟡 (hallazgo audit-09 NO respaldado por KenshiLib)
   **Problema:** KenshiLib Faction NO tiene array de miembros en 0x210/0x218 (ahí hay activePlatoons@0x208 / unloadedPlatoons@0x220). Los personajes cuelgan de Platoons, no directos en Faction.
   **Fix:** NO eliminar (audit-09 lo verificó por bytes), pero **VALIDAR CON CE** que +0x210/+0x218 contienen realmente count+array de Character* en 1.0.68. Documentar la contradicción con KenshiLib.

### PRIORIDAD 2 — Items (rompe sync de inventario/comercio)

4. **`game_types.h` ItemOffsets — REESCRIBIR ENTERO** — ⛔ (todos)
   **Problema:** offsets viejos (0x10-0x58). templateId=0x20 cae dentro de displayName → `TryGetItemTemplateId` envía basura por red.
   **Fix:** templateId/plantilla → `data @0x40` (heredado RootObjectBase). name → 0x18. quality → 0x11C. weight → 0x120. quantity (stackCount) → 0x12C. slotType → 0x110. (Ver layout completo §2.9.)
   **Prueba:** KenshiLib Item.h (InventoryItemBase) + audit-12 (quality 0x11C, weight 0x120, quantity 0x12C verificados en bytes).

5. **`building_hooks.cpp` lectura de GameData** — ⛔
   **Problema:** lee `bldPtr+0x28` para GameData* y `gameData+0x08` para templateId. +0x28 NO es el GameData (es displayName/sections), y gameData+0x08 = `validity` int.
   **Fix:** GameData* del building → `bldPtr+0x40` (data, RootObjectBase). El "templateId" entero NO existe en GameData+0x8; usar el string-id (resolver su offset con CE) o el puntero GameData mismo como clave.

### PRIORIDAD 3 — Building / Stats (código opcional/muerto, bajo impacto)

6. **`game_types.h` BuildingOffsets** — ⛔ (name, rotation, ownerFaction, health, maxHealth, isDestroyed, functionality, inventory, buildProgress, isConstructed)
   **Fix mínimo (los que importan):** name 0x10→0x18; ownerFaction 0x80→0x10 (owner); isDestroyed 0xA8→0x1A1; position(0x48) designation(0xC4) residentSquad(0xD0) ya ✅. El resto (health/maxHealth/functionality/inventory) NO existen como campos planos → marcar -1 y usar virtuales (getInventory 0x2AD6C0, getSpecialFunction, ConstructionState para progreso).
   **Prueba:** KenshiLib Building/Building.h (hereda RootObject→RootObjectBase).

7. **`game_types.h` StatsOffsets** — ⛔ (todos)
   **Fix:** reescribir contra CharStats (strength→0x80, toughness→0x90, dexterity→0x88, athletics→0x94, dodging→0xF0, unarmed→0x10C, __meleeAttack→0x120, _meleeDefence→0x124…). Ver §2.10. **Antes verificar si StatsAccessor se usa en runtime** — si es código muerto, baja prioridad.

8. **`game_types.h` CharacterOffsets.rotation = 0x58 → 0xB0** — ⛔
   **Problema:** char+0x58 = `hand handle`, NO rotación. La rot real es `Ogre::Quaternion rot` en RootObject @+0xB0.
   **Fix:** rotation 0x58→0xB0. **NOTA:** verificar que nada dependa de leer rotación en 0x58 (si se sincroniza rotación de personajes, está leyendo un handle). HOOKS.md "Struct Offsets" también dice 0x58=rotation (⛔, doc obsoleta).

### PRIORIDAD 4 — Documentación (no afecta runtime)

9. **HOOKS.md** — ⛔ varias inconsistencias doc vs código real:
   - BuildingRepair: HOOKS.md 0x5C9E70 vs orchestrator/patterns.h 0x555650 (el código usa 0x555650).
   - "Struct Offsets" sección (líneas 717-723): inventory+0x28 (⛔ es +0x88), character+0x58=rotation (⛔ es 0xB0), building GameData+0x28 (⛔ es +0x40). Doc desactualizada respecto a audit-02.
   - ItemPickup signature dice "owner at inventory+0x28" (⛔ +0x88) — solo doc, el código ya usa el dinámico +0x88.
   - inventory_hooks BuyItem signature en HOOKS.md (4 params buyer/seller/item/quantity) NO coincide con el código real (3 params buyer/item/seller) — el .cpp ya está corregido, la doc no.

### healthChain (cadena de salud) — ⛔ inválida pero ya marcada PENDIENTE

10. **`game_types.h` healthChain1=0x2B8, healthChain2=0x5F8, healthStride=8** — ⛔ (ya marcado PENDIENTE en el archivo, lo CONFIRMO inválido)
    **Problema:** char+0x2B8 = `CharacterMemory* _myMemory` (KenshiLib Character.h:571). NO es salud. La cadena CE +0x2B8→+0x5F8→+0x40 sigue _myMemory y lee un float arbitrario → /verify FAIL.
    **Ruta canónica (a validar con CE):** char+0x458 (medical, MedicalSystem INLINE) → +0x8 (`status` ogre_unordered_map<GameData*, HealthPartStatus>) → iterar → HealthPartStatus.flesh@+0x40, fleshStun@+0x44, _maxHealth@+0x54. HealthPartStatus es objeto de 0x68 bytes (NO array stride 8). Alternativa: MedicalSystem tiene getPart(index) RVA 0x2AA150 y getPart(Limb) RVA 0x656E10 — llamarlos es más robusto que recorrer el map a mano. `blood` (sangre global) @ medical+0x70.
    **Prueba:** KenshiLib MedicalSystem.h (HealthPartStatus layout) + Character.h:571 (_myMemory@0x2B8) + Character.h:739 (medical@0x458).

---

## 4. 🟡 A VALIDAR CON CHEAT ENGINE (mañana)

Cadenas/offsets concretos a probar en vivo sobre 1.0.68 Steam (base CE conocida: base+0x01AC8A90; instancia GameWorld embebida en base+0x2134110):

1. **PI+0x2A8 = ¿Character* o Platoon*?** — Resolver GameWorld(base+0x2134110) → +0x580 (player/PlayerInterface) → leer +0x2A8. Si apunta a un objeto cuyo +0x18 es un nombre de personaje legible → Character*. Si su vtable/layout es de Platoon → es currentPlatoon (KenshiLib). **Decide si la afirmación audit-09 sobre controlledChar es válida.**

2. **Faction+0x210/+0x218 = ¿count + array de Character*?** — player(+0x580) → +0x2A0 (participant Faction*) → leer uint32@+0x210 y ptr@+0x218. Si +0x218 apunta a un array contiguo de Character* (cada uno con nombre legible @+0x18) y +0x210 es su count → audit-09 correcto. Si choca con activePlatoons(0x208)/unloadedPlatoons(0x220) → es un lektor de Platoon*, no de Character*.

3. **Cadena de salud canónica:** char → +0x458 (medical) → +0x8 (status map). Inspeccionar el map: para un personaje vivo, los nodos deben tener HealthPartStatus con flesh(+0x40) ≈ 100 y _maxHealth(+0x54). Confirmar que la cadena CE histórica +0x2B8→+0x5F8→+0x40 da un valor DISTINTO (prueba de que es inválida). Probar también `blood` en medical+0x70.

4. **moneyChain char+0x298→+0x78→+0x88:** comparar el resultado con getMoney() (RVA 0x790400). Si coinciden, ✅; si no, buscar la cadena real.

5. **writable position char→animClass→+0xC0→+0x320→+0x20:** mover el personaje y ver si escribir ahí lo teletransporta. animClass = char+0x448 (`animation`, AnimationClass*).

6. **GameData string-id / name offset:** inspeccionar un GameData* (p.ej. char+0x40) y localizar dónde está el record-id FCS (string). building_hooks lo necesita; +0x8 es `validity`, no sirve.

7. **Inventory width/height (+0x20/+0x24) y maxStackMult (+0x30):** dudosos (caen en el rango del map `sections`). Confirmar o marcar -1.

8. **FactionRelations layout interno:** Faction(+0x78) → FactionRelations. El map `_factionRelations` @+0x20, defaultFactionRelation @+0x60. Confirmar el offset de `RelationData.relation` (KenshiLib dice +0x4 dentro de RelationData) vs lo que el mod lee (nodeValue +0x18). Importante para el FIX-HOSTILITY.

9. **Character::attackTarget (0x5CB0A0) vs KenshiLib (0x5CA920) y addOrder (0x5D20D0 vs KenshiLib 0x5D1950):** audit-12 los verificó por bytes; si el combate falla, cruzar estos dos RVAs (puede que apunten a addJob vs addOrder o a variantes).

---

## 5. Discrepancias internas del proyecto (el mod se contradice a sí mismo)

| Tema | game_types.h | offsets.json | patterns.json/patterns.h | HOOKS.md | Realidad (KenshiLib/bytes) |
|------|--------------|--------------|--------------------------|----------|-----------------------------|
| **faction.relations** | 0x78 ✅ | **0x50 ⛔** | — | — | **0x78** (Faction.h:128). offsets.json desactualizado. |
| **inventory.owner** | 0x88 ✅ | 0x88 ✅ | — | **0x28 ⛔** (doc) | **0x88** (Inventory.h:236). HOOKS.md doc obsoleta; el código usa 0x88. |
| **Item offsets** | 0x38/0x40/0x48 ⛔ | (no lista item) | — | — | quality 0x11C, weight 0x120, quantity 0x12C. game_types tiene los VIEJOS. |
| **character.rotation** | 0x58 ⛔ | 0x58 ⛔ | — | 0x58 ⛔ | **RootObject+0xB0** (es `hand handle` en 0x58). Las 3 fuentes coinciden en el error. |
| **BuildingRepair RVA** | — | — | 0x555650 | **0x5C9E70 ⛔** | El código (orchestrator) usa 0x555650; HOOKS.md miente. |
| **BuyItem signature** | (combat) | — | — | 4 params ⛔ | 3 params (buyer,item,seller) — el .cpp ya corregido, HOOKS.md no. |
| **GameWorldSingleton** | — | 0x02134110 ✅ (embebida) | — | — | ✅ instancia embebida, vtable 0x1722608 (offsets.json correcto). |
| **faction.id** | -1 ✅ | -1 ✅ | — | "uint32" (doc obsoleta) | -1 correcto (no existe id plano). |

**Resumen de contradicciones:** las más sangrantes son (a) `faction.relations` 0x50 en offsets.json mientras game_types ya está en 0x78, y (b) la sección "Struct Offsets" de HOOKS.md que conserva los offsets pre-audit-02 (inventory+0x28, character+0x58=rotation). Conviene sincronizar offsets.json y HOOKS.md con game_types.h, y luego corregir game_types.h donde está mal (Item/Stats/Building/rotation).

---

## 6. Fuentes y método

- **Binario:** `kenshi_x64.exe` Steam 1.0.68, leído READ-ONLY con `pefile` (mapeo RVA→file offset vía secciones PE) y desensamblado con `iced-x86` (modo 64-bit, sintaxis Intel). Helper recreado en `C:/Users/Zero/ke_re.py` (READ-ONLY: hex/dis/str/find_pattern/prev_bytes).
- **Segunda fuente (HECHOS de offset de campo y RVA de método):** KenshiLib (`tools/KenshiLib-reference/KenshiLib/Include/kenshi/`): RootObjectBase.h, RootObject.h, Character.h, Faction.h, FactionRelations.h, GameWorld.h, PlayerInterface.h, Inventory.h, Item.h, CharStats.h, MedicalSystem.h, Platoon.h, Building/Building.h, GameData.h. KenshiLib da offsets de campo como comentarios `// 0xNN Member` y RVAs de método (cambian por versión, usados solo como pista cruzada).
- **Verificación de bytes realizada:** prólogos de las 17+ funciones de hooks (padding CC/C3 previo = inicio real), AOB de patterns.json (raw_bytes casan exacto en su RVA), AI::create 0x6221AF (`mov [rbx+0x20],rax` confirma aiPackage=char+0x20), SetControlledChar 0x80254E/0x802563 (escribe participant PI+0x2A0, refuta controlledChar@0x2A8), SquadAddMember 0x928423 (mid-función, confirma disabled), BuildingRepair 0x5C9E70 vs 0x555650.
- **Cruces con audits previos:** audit-12 (combate, offsets de campo verificados en bytes — dados por buenos), audit-13 (3 hooks GOG: SquadSpawnBypass/SquadSpawnCall/CharAnimUpdate muertos en Steam).
- **Honestidad:** ✅ = verificado en bytes Steam Y/O confirmado en KenshiLib. ⛔ = contradicho por KenshiLib o por bytes (con valor correcto probado). 🟡 = no verificable estáticamente o KenshiLib y bytes/audit-09 se contradicen → requiere CE en vivo. ❓ = sin 2ª fuente. Donde audit-09 (bytes) y KenshiLib chocan (memberArrayReal, controlledChar), lo marco 🟡 y NO doy por refutado el hallazgo runtime: pido validación CE.

**Limitación:** las RVAs de método de KenshiLib son de OTRA versión de Kenshi (no 1.0.68 Steam), por eso solo las uso como pista, nunca como verdad de RVA. Los offsets de CAMPO de KenshiLib SÍ son fiables entre versiones (el prompt y audit-12 lo confirman). Donde solo tengo KenshiLib (sin poder verificar el campo en bytes de un getter pequeño), lo marco según corresponda y sugiero CE.
