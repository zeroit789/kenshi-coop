// ES: game_world.cpp - Acceso al estado global del mundo (objeto GameWorld de Kenshi): hora del
//     día, velocidad de juego, clima y pausa. Contiene WorldAccessor (singleton antiguo basado en
//     un puntero fijado a mano) y la implementación de GameWorldAccessor (el API de game_types.h),
//     que resuelve bien el layout de instancia embebida de Steam 1.0.68.
// EN: game_world.cpp - Access to global world state (Kenshi's GameWorld object): time of day,
//     game speed, weather and pause. Holds WorldAccessor (older singleton based on a manually set
//     pointer) and the GameWorldAccessor implementation (the game_types.h API), which correctly
//     resolves the embedded-instance layout of Steam 1.0.68.
#include "game_types.h"
#include "kmp/memory.h"
#include "kmp/constants.h"
#include <spdlog/spdlog.h>
#include <Windows.h>  // IMAGE_DOS_HEADER / IMAGE_NT_HEADERS para ResolveWorldObject()

namespace kmp::game {

// Declaración forward del puntero al setter oficial GameWorld::setPaused (RVA 0x787D40).
// Definido en game_character.cpp (bridge SetGameSetPausedFn). Firma __fastcall(gw, bool).
// Lo usa GameWorldAccessor::SetPaused para refrescar caches de subsistemas + HUD, en vez
// de escribir GameWorld+0x8B9 a pelo (que dejaba la "pausa fantasma").
// EN: Forward declaration of the official GameWorld::setPaused setter pointer (RVA 0x787D40),
//     defined in game_character.cpp (SetGameSetPausedFn bridge). Signature __fastcall(gw, bool).
//     GameWorldAccessor::SetPaused uses it to refresh subsystem caches + HUD instead of writing
//     GameWorld+0x8B9 directly (which left the "ghost pause").
using SetPausedFn = void(__fastcall*)(void* gameWorld, bool paused);
SetPausedFn GetGameSetPausedFn_Internal();

// ES: Accessor singleton del mundo basado en m_worldPtr (fijado con SetWorldPointer). Lee/escribe
//     hora, velocidad y clima con WorldOffsets. OJO: timeOfDay y weatherState valen -1 (la hora
//     vive en TimeManager), así que esas llamadas devuelven el valor por defecto; y no maneja el
//     layout de instancia embebida como GameWorldAccessor.
// EN: World singleton accessor based on m_worldPtr (set via SetWorldPointer). Reads/writes time,
//     speed and weather with WorldOffsets. NOTE: timeOfDay and weatherState are -1 (time lives in
//     TimeManager), so those calls return defaults; and it does not handle the embedded-instance
//     layout like GameWorldAccessor does.
class WorldAccessor {
public:
    static WorldAccessor& Get() {
        static WorldAccessor instance;
        return instance;
    }

    // ES: Métodos de lectura (valor por defecto si falta offset o puntero).
    // EN:
    // ── Read Methods ──

    float GetTimeOfDay() const {
        auto& offsets = GetOffsets();
        if (offsets.world.timeOfDay < 0 || m_worldPtr == 0) return 0.5f;

        float tod = 0.5f;
        Memory::Read(m_worldPtr + offsets.world.timeOfDay, tod);
        return tod;
    }

    float GetGameSpeed() const {
        auto& offsets = GetOffsets();
        if (offsets.world.gameSpeed < 0 || m_worldPtr == 0) return 1.0f;

        float speed = 1.0f;
        Memory::Read(m_worldPtr + offsets.world.gameSpeed, speed);
        return speed;
    }

    int GetWeatherState() const {
        auto& offsets = GetOffsets();
        if (offsets.world.weatherState < 0 || m_worldPtr == 0) return 0;

        int weather = 0;
        Memory::Read(m_worldPtr + offsets.world.weatherState, weather);
        return weather;
    }

    // ES: Métodos de escritura (para aplicar el estado que manda el servidor).
    // EN:
    // ── Write Methods (for applying server state) ──

    bool SetTimeOfDay(float timeOfDay) {
        auto& offsets = GetOffsets();
        if (offsets.world.timeOfDay < 0 || m_worldPtr == 0) return false;

        return Memory::Write(m_worldPtr + offsets.world.timeOfDay, timeOfDay);
    }

    bool SetGameSpeed(float speed) {
        auto& offsets = GetOffsets();
        if (offsets.world.gameSpeed < 0 || m_worldPtr == 0) return false;

        return Memory::Write(m_worldPtr + offsets.world.gameSpeed, speed);
    }

    bool SetWeatherState(int weather) {
        auto& offsets = GetOffsets();
        if (offsets.world.weatherState < 0 || m_worldPtr == 0) return false;

        return Memory::Write(m_worldPtr + offsets.world.weatherState, weather);
    }

    // ES: Utilidades: zona de una posición y gestión del puntero al mundo.
    // EN:
    // ── Utility ──

    // ES: Convierte una posición del mundo a coordenada de zona (celdas de KMP_ZONE_SIZE).
    // EN: Converts a world position to a zone coordinate (KMP_ZONE_SIZE cells).
    ZoneCoord GetZone(const Vec3& worldPos) const {
        return ZoneCoord::FromWorldPos(worldPos, KMP_ZONE_SIZE);
    }

    // ES: Fija el puntero al singleton del mundo (lo encuentra el escáner al iniciar).
    // EN:
    // Set the world singleton pointer (found by scanner at init)
    void SetWorldPointer(uintptr_t ptr) {
        m_worldPtr = ptr;
        spdlog::info("WorldAccessor: World pointer set to 0x{:X}", ptr);
    }

    uintptr_t GetWorldPointer() const { return m_worldPtr; }

    // ES: Intento de descubrir el puntero del mundo: hoy no hace nada, solo dice si ya hay uno fijado.
    // EN:
    // Try to discover the world pointer from known offsets
    bool DiscoverWorldPointer() {
        uintptr_t base = Memory::GetModuleBase();

        // Try the player base chain — the GameWorld singleton is often
        // accessible from the player base or nearby globals.
        // Known pattern: base+01AC8A90 is the player base pointer,
        // and the world singleton is typically at a nearby .data address.
        //
        // For now, we set a null world pointer and rely on the hook-based approach
        // where the time update hook gives us the timeManager pointer directly.
        return m_worldPtr != 0;
    }

private:
    WorldAccessor() = default;
    uintptr_t m_worldPtr = 0;
};

// ES: Implementación de GameWorldAccessor (API pública de game_types.h): lee campos del
//     OBJETO GameWorld resuelto por ResolveWorldObject().
// EN:
// ── GameWorldAccessor method implementations ──
// These are the public API declared in game_types.h.
// They read game-world fields relative to the resolved GameWorld OBJECT.
//
// CRITICO Steam 1.0.68: GameWorld es una INSTANCIA embebida en .data, no un puntero. Por eso
// NO se puede dereferenciar m_addr a ciegas (eso leeria la vtable y los offsets darian basura).
// ResolveWorldObject() decide si m_addr es el objeto (instancia directa) o un puntero a el.
// EN: CRITICAL on Steam 1.0.68: GameWorld is an INSTANCE embedded in .data, not a pointer, so
//     m_addr cannot be blindly dereferenced (that would read the vtable and give garbage offsets).

// ── ResolveWorldObject ──
// Devuelve la direccion del OBJETO GameWorld real, manejando ambos layouts:
//   (a) puntero clasico  : *m_addr es heap-ptr a un objeto con vtable en .text -> ese objeto
//   (b) instancia directa: *m_addr ES la vtable en .text -> el objeto es m_addr mismo
// Devuelve 0 si m_addr es 0 o el contenido no encaja en ningun layout valido.
// EN: Returns the real GameWorld OBJECT address handling both layouts: (a) classic pointer,
//     *m_addr is a heap pointer to an object with a vtable in the module; (b) direct instance,
//     *m_addr IS the vtable, so the object is m_addr. Returns 0 if neither fits.
//     Note: the "text" range used is modBase+0x1000 .. modBase+SizeOfImage (the whole image, not
//     only the .text section).
uintptr_t GameWorldAccessor::ResolveWorldObject() const {
    if (m_addr == 0) return 0;
    uintptr_t modBase = Memory::GetModuleBase();
    size_t    modSize = 0x4000000; // fallback 64MB
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                modSize = nt->OptionalHeader.SizeOfImage;
        }
    }
    uintptr_t textStart = modBase + 0x1000;
    uintptr_t textEnd   = modBase + modSize;

    uintptr_t first = 0;
    if (!Memory::Read(m_addr, first) || first == 0) return 0;

    // Caso (b): instancia directa — el primer qword YA es la vtable en .text.
    if (first >= textStart && first < textEnd) return m_addr;

    // Caso (a): puntero clasico — first debe ser heap-ptr (fuera del modulo) a un objeto
    // cuya primera qword sea una vtable en .text.
    if (first < 0x10000 || first >= 0x00007FFFFFFFFFFF) return 0;
    if (first >= modBase && first < modBase + modSize) return 0;
    uintptr_t vtable = 0;
    if (Memory::Read(first, vtable) && vtable >= textStart && vtable < textEnd)
        return first;
    return 0;
}

// ES: Getters/escritores de hora, velocidad (+0x700) y clima sobre el objeto resuelto.
//     La hora y el clima no tienen offset en GameWorld (-1): devuelven 0.5 / 0 / false.
// EN: Time, speed (+0x700) and weather getters/writers on the resolved object.
//     Time and weather have no GameWorld offset (-1): they return 0.5 / 0 / false.
float GameWorldAccessor::GetTimeOfDay() const {
    auto& offsets = GetOffsets();
    if (offsets.world.timeOfDay < 0) return 0.5f;
    uintptr_t worldObj = ResolveWorldObject();
    if (worldObj == 0) return 0.5f;

    float tod = 0.5f;
    Memory::Read(worldObj + offsets.world.timeOfDay, tod);
    return tod;
}

float GameWorldAccessor::GetGameSpeed() const {
    auto& offsets = GetOffsets();
    if (offsets.world.gameSpeed < 0) return 1.0f;
    uintptr_t worldObj = ResolveWorldObject();
    if (worldObj == 0) return 1.0f;

    float speed = 1.0f;
    Memory::Read(worldObj + offsets.world.gameSpeed, speed);
    return speed;
}

int GameWorldAccessor::GetWeatherState() const {
    auto& offsets = GetOffsets();
    if (offsets.world.weatherState < 0) return 0;
    uintptr_t worldObj = ResolveWorldObject();
    if (worldObj == 0) return 0;

    int weather = 0;
    Memory::Read(worldObj + offsets.world.weatherState, weather);
    return weather;
}

bool GameWorldAccessor::WriteTimeOfDay(float time) {
    auto& offsets = GetOffsets();
    if (offsets.world.timeOfDay < 0) return false;
    uintptr_t worldObj = ResolveWorldObject();
    if (worldObj == 0) return false;

    return Memory::Write(worldObj + offsets.world.timeOfDay, time);
}

bool GameWorldAccessor::WriteGameSpeed(float speed) {
    auto& offsets = GetOffsets();
    if (offsets.world.gameSpeed < 0) return false;
    uintptr_t worldObj = ResolveWorldObject();
    if (worldObj == 0) return false;

    return Memory::Write(worldObj + offsets.world.gameSpeed, speed);
}

// ── GetPausedRaw ──
// Lee el flag paused (GameWorld+0x8B9, byte) del OBJETO GameWorld real.
// Devuelve 1 (pausado), 0 (despausado) o -1 (no se pudo resolver/leer).
// CRÍTICO: usa ResolveWorldObject() — en Steam 1.0.68 el singleton es la instancia
// embebida, así que dereferenciarlo a ciegas leería la vtable y daría un offset basura.
// EN: Reads the paused flag (GameWorld+0x8B9, byte) of the real GameWorld object.
//     Returns 1 (paused), 0 (unpaused) or -1 (could not resolve/read).
int GameWorldAccessor::GetPausedRaw() const {
    auto& offsets = GetOffsets();
    if (offsets.world.paused < 0) return -1;
    uintptr_t worldObj = ResolveWorldObject();
    if (worldObj == 0) return -1;

    uint8_t paused = 0;
    if (!Memory::Read(worldObj + offsets.world.paused, paused)) return -1;
    return paused != 0 ? 1 : 0;
}

// ── SetPaused ──
// Cambia el estado de pausa del juego.
//
// PREFERENTE: llama al setter OFICIAL GameWorld::setPaused (RVA 0x787D40) si está
// resuelto. Ese setter no solo escribe GameWorld+0x8B9, sino que ADEMÁS refresca los
// caches de pausa de 3 subsistemas (obj+0xB8), oculta/enseña el cartel "PAUSED" del HUD
// (updatePauseUI @0x6E20D0) y emite el evento "Resume_Game"/"Pause_Game". Esto es lo que
// arregla la "pausa fantasma": escribir el byte a pelo despausaba SOLO la simulación,
// dejando a los subsistemas creyendo que seguía pausado → bloqueaban las órdenes del
// jugador (atacar/hablar) y el HUD mostraba "PAUSED" para siempre.
//
// FALLBACK: si el setter no está resuelto (AOB no encontrado), escribe el byte crudo
// (comportamiento antiguo) para no perder al menos la despausa de simulación.
//
// THREAD SAFETY: se debe llamar desde el hilo que conduce OnGameTick (Present/render),
// porque el setter toca el HUD de MyGUI. OnGameTick ya corre en ese hilo. SEH protege
// contra el caso de que la cadena de notificación toque memoria liberada durante carga.
// EN: Changes the game pause state. PREFERRED: call the official GameWorld::setPaused setter
//     (RVA 0x787D40), which besides writing +0x8B9 refreshes the pause caches of 3 subsystems
//     (obj+0xB8), shows/hides the HUD "PAUSED" label (updatePauseUI @0x6E20D0) and fires
//     "Resume_Game"/"Pause_Game". That fixes the "ghost pause". FALLBACK: if the setter is not
//     resolved (AOB pattern not found) or crashes, write the raw byte (only unpauses the simulation).
//     Must run on the OnGameTick (render) thread because the setter touches the MyGUI HUD; SEH-protected.
bool GameWorldAccessor::SetPaused(bool paused) {
    auto& offsets = GetOffsets();
    if (offsets.world.paused < 0) return false;
    uintptr_t worldObj = ResolveWorldObject();
    if (worldObj == 0) return false;

    // ── Vía PREFERENTE: setter oficial ──
    SetPausedFn officialSetter = GetGameSetPausedFn_Internal();
    if (officialSetter != nullptr) {
        bool ok = false;
        __try {
            officialSetter(reinterpret_cast<void*>(worldObj), paused);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            spdlog::error("GameWorldAccessor::SetPaused: setter oficial 0x{:X} CRASHED "
                          "(worldObj=0x{:X}, paused={}) — fallback a write crudo",
                          reinterpret_cast<uintptr_t>(officialSetter), worldObj, paused);
            ok = false;
        }
        if (ok) return true;
        // Si el setter petó, caemos al write crudo de abajo como último recurso.
    }

    // ── Vía FALLBACK: write crudo del byte (solo despausa la simulación) ──
    uint8_t val = paused ? 1 : 0;
    return Memory::Write(worldObj + offsets.world.paused, val);
}

} // namespace kmp::game
