// ES: entity_hooks.h — interfaz pública de los hooks de entidades (personajes).
//     Declara la instalación del hook sobre la fábrica de personajes del juego
//     (CharacterCreate / RootObjectFactory::createCharacter) y del destructor de
//     personajes, más utilidades para: llamar a la fábrica directamente (spawn de
//     jugadores remotos), detectar el fin de la carga por ráfagas de creates,
//     elegir la facción del jugador durante la carga y exponer contadores de
//     diagnóstico al resto del módulo (SpawnManager, game_tick_hooks, core).
// EN: entity_hooks.h — public interface of the entity (character) hooks.
//     Declares installation of the hook on the game's character factory
//     (CharacterCreate / RootObjectFactory::createCharacter) and of the character
//     destructor, plus helpers to: call the factory directly (spawning remote
//     players), detect the end of loading from bursts of creates, elect the
//     player's faction while loading, and expose diagnostic counters to the rest
//     of the module (SpawnManager, game_tick_hooks, core).
#pragma once
#include <cstdint>

// ES: Espacio de nombres de los hooks de creación/destrucción de personajes.
// EN: Namespace for the character create/destroy hooks.
namespace kmp::entity_hooks {

// ── FIX-FACTORY-EARLY (2026-06-20) ──
// ES: (texto original en español a continuación)
// EN: Enable flag for "early factory capture via the CharacterCreate hook".
//     ROOT CAUSE: InstallAt() installs the MovRaxRsp hook with bypass=1, so the
//     naked detour jumps straight to the original and the hook body (which saves
//     the factory pointer and promotes it to SpawnManager) never runs while the
//     save is loading. Bypass is only cleared in HookManager::Enable() inside
//     ResumeForNetwork/OnGameLoaded, after loading created every NPC.
//     FIX: arm the hook (bypass->0) from Install() in LOADING PASSTHROUGH mode,
//     where the body does the bare minimum (timestamp, counter, save factory,
//     promote to SpawnManager, call original). Set to false to restore the old
//     "starts bypassed" behavior if early capture ever proves unsafe.
//
// Flag de activación de la "captura temprana del factory vía el hook CharacterCreate".
//
// CAUSA RAÍZ: InstallAt() instala el hook MovRaxRsp con bypass=1 (Phase 6 de
// hook_manager.cpp -> "starts BYPASSED"). Mientras bypass=1, el naked detour
// cortocircuita ANTES de llamar a Hook_CharacterCreate: salta directo al original.
// Por eso el CUERPO del hook (incl. la captura de s_savedFactory y la promoción al
// SpawnManager) NUNCA corre durante la carga del save -> m_factory queda en 0,
// IsReady()=false, y el spawn de remotos no arranca. El bypass solo se limpia en
// HookManager::Enable() dentro de ResumeForNetwork/OnGameLoaded, DESPUÉS de que la
// carga ya creó todos los NPCs.
//
// SOLUCIÓN (Opción A, la más segura): armar el hook (bypass->0) DESDE Install(),
// dejándolo en LOADING PASSTHROUGH (s_loadingPassthrough=true). En passthrough el
// cuerpo hace lo MÍNIMO: timestamp + counter + guardar el 1er arg (factory) +
// promover al SpawnManager + delegar al original. Así captura el factory en el 1er
// create de CUALQUIER carga, sin ejecutar lógica pesada en bypass.
//
// Si en alguna build la captura temprana resultara peligrosa, poner este flag a
// false: el hook vuelve a arrancar bypassed (comportamiento previo).
extern bool kCaptureFactoryEarly;

// ES: Instala el hook de CharacterCreate sobre RootObjectFactory::process (RVA
//     0x581770, dirección resuelta por el escáner como funcs.CharacterSpawn), lo arma
//     en modo loading-passthrough y resuelve create/createRandomChar por RVA fijo.
//     El hook del destructor (CharacterDestroy) NO se instala aquí: se instala diferido
//     leyendo vtable[0] del primer Character vivo. Devuelve false si falla el hook de
//     creación. Corre en el hilo de arranque del plugin.
// EN: Installs the CharacterCreate hook on RootObjectFactory::process (RVA 0x581770,
//     address resolved by the scanner as funcs.CharacterSpawn), arms it in
//     loading-passthrough mode and resolves create/createRandomChar from fixed RVAs.
//     The destructor hook (CharacterDestroy) is NOT installed here: it is installed
//     lazily by reading vtable[0] of the first live Character. Returns false if the
//     create hook fails. Runs on the plugin startup thread.
bool Install();
// ES: Quita los hooks instalados por Install().
// EN: Removes the hooks installed by Install().
void Uninstall();

// ES: Activa el hook de CharacterCreate para multijugador. Llamar al conectar al
//     servidor para que se capturen los nuevos creates de personajes.
// EN: Enable the CharacterCreate hook for multiplayer.
// Call this when connecting to a server so new character creates are captured.
void ResumeForNetwork();

// ES: Desactiva el hook de CharacterCreate al desconectar. Evita corrupción de heap
//     del wrapper MovRaxRsp durante ráfagas de carga de zonas sin conexión.
// EN: Disable the CharacterCreate hook on disconnect.
// Prevents MovRaxRsp heap corruption from zone-load bursts while not connected.
void SuspendForDisconnect();

// ES: Activa/desactiva el bypass de spawn directo. Si es true, Hook_CharacterCreate
//     se salta toda la lógica de spawn/registro y solo llama a la original. Lo usa
//     SpawnManager al invocar la fábrica desde GameFrameUpdate para evitar recursión.
// EN: Set/clear the direct spawn bypass flag.
// When true, Hook_CharacterCreate skips all spawn/registration logic
// and just passes through to the original function. Used by SpawnManager
// when calling the factory from GameFrameUpdate to avoid recursive spawn logic.
void SetDirectSpawnBypass(bool bypass);

// ES: Indica si un spawn "in-place" (reutilizando una llamada real del juego) tuvo
//     éxito en los últimos `withinSeconds` segundos. Lo usa game_tick_hooks para no
//     competir con ese mecanismo.
// EN: Check if an in-place replay spawn succeeded recently.
// Used by game_tick_hooks to avoid competing with in-place replay.
bool HasRecentInPlaceSpawn(int withinSeconds = 30);

// ES: Número total de spawns in-place con éxito.
// EN: Get total number of successful in-place spawns
int GetInPlaceSpawnCount();

// ES: Decrementa el contador de límite de spawns por jugador cuando una entidad
//     remota muere o desaparece. Debe llamarse desde cualquier ruta que elimine una
//     entidad remota para que el límite no se sature.
// EN: Decrement the per-player spawn cap counter when a remote entity dies or despawns.
// Must be called from any code path that removes a remote entity (despawn handler,
// character destroy hook, heartbeat cleanup, etc.) to prevent the cap from saturating.
void DecrementSpawnCount(uint32_t owner);

// ES: Getters de diagnóstico (para las instantáneas del PipelineOrchestrator).
// EN: Diagnostic getters (for PipelineOrchestrator snapshot collection)
int  GetTotalCreates();
int  GetTotalDestroys();

// ── Loading detection via create events ──
// ES: Milisegundos desde el último disparo del hook CharacterCreate (en cualquier
//     modo). PollForGameLoad lo usa para saber cuándo acabó la ráfaga de carga sin
//     usar CharacterIterator (que corrompe el heap durante la carga).
// EN: Returns milliseconds since the last CharacterCreate hook fired (any mode).
// Used by PollForGameLoad to detect when the loading burst has finished
// without needing CharacterIterator (which corrupts the heap during loading).
int64_t GetTimeSinceLastCreate();

// ES: Cuántos personajes se crearon en la ráfaga de carga actual (se pone a 0 con
//     ResetLoadingCreateCount()).
// EN: Returns how many characters were created during the current loading burst.
// Resets to 0 when ResetLoadingCreateCount() is called.
int GetLoadingCreateCount();

// ES: Pone a 0 el contador de creates de carga (al pasar a la fase Loading).
// EN: Reset the loading create counter (called when transitioning to Loading phase).
void ResetLoadingCreateCount();

// ES: Activa el modo passthrough ultraligero de carga: el hook solo actualiza
//     timestamp/contador y llama a la original; sin leer memoria del juego, sin
//     votación de facción ni registro de entidades.
// EN: Enable ultra-lightweight passthrough mode for loading.
// When true, Hook_CharacterCreate only updates timestamp/counter and calls original.
// No game memory reads, no faction voting, no entity registration.
void SetLoadingPassthrough(bool enabled);

// ES: Copia en outPtrs los punteros a personajes plantilla del mod capturados durante
//     la carga (máx. 16). Devuelve cuántos copió.
// EN: Get pointers to mod template characters captured during loading.
// Returns count of captured pointers (up to 16). Fills outPtrs array.
int GetCapturedModTemplates(void** outPtrs, int maxCount);

// ES: Llama a la fábrica DIRECTAMENTE por el trampolín crudo de MinHook, saltándose
//     el hook y el wrapper MovRaxRsp. El trampolín empieza con `mov rax, rsp` y monta
//     su propio marco: sin cambio de pila ni corrupción de heap. Devuelve el
//     personaje creado o nullptr.
// EN: Call the factory function DIRECTLY via the raw MinHook trampoline,
// completely bypassing the hook and MovRaxRsp wrapper.
// The raw trampoline starts with `mov rax, rsp` and sets up its own
// frame correctly — no stack swap, no heap corruption.
// Returns the created character or nullptr.
void* CallFactoryDirect(void* factory, void* requestStruct);

// ES: Llama a RootObjectFactory::create, el despachador de alto nivel que construye
//     internamente la estructura de petición a partir de un GameData*. Evita el
//     problema de clonar structs con punteros caducados. Devuelve el personaje o nullptr.
// EN: Call RootObjectFactory::create — the high-level dispatcher that builds a request
// struct internally from a GameData* pointer. Bypasses stale-pointer struct clone issue.
// Takes (factory, GameData*) and returns created character, or nullptr.
void* CallFactoryCreate(void* factory, void* gameData);

// ES: Llama a RootObjectFactory::createRandomChar (crea un NPC aleatorio). Último
//     recurso cuando fallan las plantillas.
// EN: Call RootObjectFactory::createRandomChar — creates a random NPC character.
// Takes just factory pointer. Last-resort fallback when templates fail.
void* CallFactoryCreateRandom(void* factory);

// ES: Facción de respaldo (última facción válida vista en cualquier create). La usa
//     SEH_FixUpFaction_Core si no hay facción principal. Valida el puntero; 0 si caducó.
// EN: Get the fallback faction pointer (last valid faction seen from any character creation).
// Used by SEH_FixUpFaction_Core when primary character faction isn't available.
// Validates the pointer before returning; returns 0 if stale.
uintptr_t GetFallbackFaction();

// ES: Facción del jugador elegida durante la carga por votación multifuente (la más
//     común en los primeros N personajes, con peso extra si el nombre coincide con
//     playerName de la config o si la facción tiene isPlayerFaction). 0 si aún no hay
//     o si el puntero caducó.
// EN: Get the player faction elected during savegame loading via multi-source voting.
// Scans the first N characters and picks the most common faction, with bonus
// weight for characters whose name matches the config playerName and for
// factions with the isPlayerFaction flag set. Returns 0 if not yet elected.
// Validates the pointer before returning; returns 0 if stale.
uintptr_t GetEarlyPlayerFaction();

// ES: Revalida los punteros de facción guardados; si todos caducaron (p.ej. tras
//     guardar/cargar), vuelve a escanear la escuadra local. Devuelve true si queda
//     una facción válida.
// EN: Re-validate stored faction pointers. If all are stale (e.g., after a save/load),
// re-scans the local squad to discover a fresh valid faction. Prioritizes
// characters whose name matches config playerName and factions with isPlayerFaction.
// Returns true if a valid faction is available after the call.
bool RevalidateFaction();

// ES: Offset detectado del puntero GameData dentro de la estructura de petición de
//     la fábrica. -1 si aún no se detectó.
// EN: Get the detected GameData pointer offset within the factory request struct.
// Returns -1 if not yet detected.
int GetGameDataOffsetInStruct();

// ES: Offset detectado de la posición dentro de la estructura de petición. -1 si no.
// EN: Get the detected position offset within the factory request struct.
// Returns -1 if not yet detected.
int GetPositionOffsetInStruct();

} // namespace kmp::entity_hooks
