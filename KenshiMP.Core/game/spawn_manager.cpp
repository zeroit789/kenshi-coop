#include "spawn_manager.h"
#include "game_types.h"
#include "../core.h"
#include "../hooks/entity_hooks.h"
#include "../hooks/ai_hooks.h"
#include "../sync/pipeline_state.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <Windows.h>

namespace kmp {

// ── SEH helper functions ──
// MSVC forbids __try in functions with C++ objects that need unwinding.
// These thin wrappers contain ONLY the SEH-protected code, no std:: objects.

// Reads raw string data from a Kenshi std::string at `addr` into `outBuf`.
// Returns the number of bytes copied, or 0 on failure.
static size_t SEH_ReadKenshiStringRaw(uintptr_t addr, char* outBuf, size_t bufSize) {
    __try {
        uint64_t length = 0;
        uint64_t capacity = 0;
        Memory::Read(addr + 0x10, length);
        Memory::Read(addr + 0x18, capacity);

        if (length == 0 || length > 4096) return 0;

        const char* strData = nullptr;
        if (capacity < 16) {
            strData = reinterpret_cast<const char*>(addr);
        } else {
            uintptr_t heapPtr = 0;
            Memory::Read(addr, heapPtr);
            if (heapPtr == 0) return 0;
            strData = reinterpret_cast<const char*>(heapPtr);
        }

        size_t copyLen = (length < bufSize - 1) ? static_cast<size_t>(length) : bufSize - 1;
        __try {
            memcpy(outBuf, strData, copyLen);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        outBuf[copyLen] = '\0';
        return copyLen;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Calls the factory function under SEH protection.
// Returns the created character pointer, or nullptr on exception.
static void* SEH_CallFactory(FactoryProcessFn fn, void* factory, void* templateData) {
    __try {
        return fn(factory, templateData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Reads a uintptr_t-sized value from memory under SEH protection.
// Returns true if the read succeeded.
static bool SEH_ReadPointer(const uintptr_t* src, uintptr_t& out) {
    __try {
        out = *src;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

// ── Kenshi std::string layout (MSVC x64) ──
// Offset +0x00: union { char buf[16]; char* ptr; }  (small string or heap pointer)
// Offset +0x10: size_t length
// Offset +0x18: size_t capacity
// If capacity < 16, the string is stored inline in buf[16].
// If capacity >= 16, ptr points to heap allocation.
std::string SpawnManager::ReadKenshiString(uintptr_t addr) {
    char buf[256] = {};
    size_t len = SEH_ReadKenshiStringRaw(addr, buf, sizeof(buf));
    if (len == 0) return "";
    return std::string(buf, len);
}

void SpawnManager::SetSavedRequestStruct(const uint8_t* data, size_t size) {
    std::lock_guard lock(m_templateMutex);
    m_savedRequestStruct.assign(data, data + size);
    m_hasRequestStruct = true;
    spdlog::info("SpawnManager: Saved request struct ({} bytes)", size);
}

void SpawnManager::SetPreCallData(const uint8_t* data, size_t size, uintptr_t origAddr) {
    std::lock_guard lock(m_templateMutex);
    size_t copySize = (size < sizeof(m_preCallData)) ? size : sizeof(m_preCallData);
    memcpy(m_preCallData, data, copySize);
    m_preCallDataSize = copySize;
    m_preCallOrigAddr = origAddr;
    m_hasPreCallData = true;
    spdlog::info("SpawnManager: Saved pre-call data ({} bytes, origAddr=0x{:X})", copySize, origAddr);
}

// SEH wrapper for standalone factory call
static void* SEH_CallFactoryStandalone(FactoryProcessFn fn, void* factory, void* reqStruct) {
    __try {
        return fn(factory, reqStruct);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Write the local player's faction to a newly-spawned character to prevent
// use-after-free on faction+0x250. Mod template factions may not exist in
// the local save, but the local player's faction is always valid.
static void ApplyFactionFix(void* character) {
    if (!character) return;
    uintptr_t localFaction = entity_hooks::GetEarlyPlayerFaction();
    if (localFaction == 0) localFaction = entity_hooks::GetFallbackFaction();
    if (localFaction != 0) {
        game::CharacterAccessor accessor(character);
        accessor.WriteFaction(localFaction);
        spdlog::debug("SpawnManager: Faction fix applied — wrote 0x{:X} to char 0x{:X}",
                      localFaction, reinterpret_cast<uintptr_t>(character));
    } else {
        spdlog::warn("SpawnManager: No local faction available for faction fix on char 0x{:X}",
                     reinterpret_cast<uintptr_t>(character));
    }
}

void* SpawnManager::SpawnCharacterDirect(const Vec3* desiredPosition, int modSlot) {
    // ═══ SINGLE PATH: Mod template via FactoryCreate ═══
    // Mod templates are REAL persistent GameData from kenshi-online.mod.
    // FactoryCreate builds fresh internal state (faction, squad, AI) — no stale pointers.
    // REMOVED: createRandomChar fallback (wrong appearance, no mod data integration).
    if (m_modTemplateCount.load() <= 0 || !m_factory) {
        static int s_noTemplateLog = 0;
        if (++s_noTemplateLog <= 5 || s_noTemplateLog % 100 == 0) {
            spdlog::warn("SpawnManager: SpawnCharacterDirect not ready "
                         "(modTemplates={}, factory={})",
                         m_modTemplateCount.load(), m_factory != nullptr);
        }
        return nullptr;
    }

    // Clamp modSlot to valid range
    int templateCount = m_modTemplateCount.load();
    if (modSlot < 0 || modSlot >= templateCount) modSlot = modSlot % templateCount;
    if (modSlot < 0) modSlot = 0;

    Vec3 pos = desiredPosition ? *desiredPosition : Vec3{0, 0, 0};
    void* character = SpawnWithModTemplate(modSlot, pos);
    if (character) {
        spdlog::info("SpawnManager: SpawnCharacterDirect SUCCESS — char 0x{:X} (slot {})",
                     reinterpret_cast<uintptr_t>(character), modSlot);
        return character;
    }

    spdlog::warn("SpawnManager: SpawnCharacterDirect failed (slot={})", modSlot);
    return nullptr;
}

// ── FIX-FACTORY-GW: definición del flag de activación (2026-06-20) ──
// Activado por defecto. Ver declaración en spawn_manager.h.
bool kCaptureFactoryFromGameWorld = true;

// ── theFactory: PUNTERO GLOBAL en .data (RVA 0x21345B0) ── [CORREGIDO 2026-06-20]
// RE de bytes verificado (capstone, Steam 1.0.68): theFactory es un puntero global independiente
// en .data, NO un campo de GameWorld. 9 callers distintos de RootObjectFactory::create (0x583400)
// lo cargan en rcx con `mov rcx, qword ptr [rip + disp]` resolviendo SIEMPRE a RVA 0x21345B0
// (sitios 0x36B1FB, 0x36BEE5, 0x36F559, 0x36F882, 0x4D11E7, 0x4D7773, 0x54EC3E, 0x8743DC, 0x8F750F).
// El binario estático tiene ahí una entrada de relocación (puntero), NO un float -> en runtime
// contiene el RootObjectFactory* del heap. create() pasa ese rcx a process() (0x581770) intacto.
//
// El offset relativo a la instancia GameWorld (modBase+0x2134110) es +0x4A0 (== 0x21345B0).
// ⛔ El FIX viejo usaba +0x5B0 (== 0x21346C0), que es OTRO campo y en runtime contiene un FLOAT
//    1.0f (0x3F800000) -> al pasarlo a CallFactoryCreate como factory -> CRASH.
static constexpr uintptr_t THEFACTORY_GLOBAL_RVA = 0x21345B0;  // dir. estática del puntero global
static constexpr uintptr_t GW_THEFACTORY_OFFSET  = 0x4A0;      // == 0x21345B0 - 0x2134110

// ── FIX-FACTORY-GW #1: captura del factory desde el global theFactory (RVA 0x21345B0) ──
// No depende del hook CharacterCreate. Vía PRIMARIA: leer el puntero global estático
// modBase+0x21345B0 (robusto, independiente del layout de GameWorld). Vía SECUNDARIA (fallback,
// por si en otra versión GameWorld fuese un puntero clásico): GW+0x4A0. En ambos casos valida que
// el resultado es un puntero de heap con vtable en .text (descarta el float que rompía el fix viejo).
bool SpawnManager::CaptureFactoryFromGameWorld(uintptr_t gwSingleton) {
    if (!kCaptureFactoryFromGameWorld) return false;
    if (m_factory != nullptr) return true;   // ya capturado (por hook o por una llamada previa)

    uintptr_t moduleBase = Memory::GetModuleBase();
    if (moduleBase == 0) return false;
    size_t moduleSize = 0x4000000; // fallback 64MB

    // ── Rango REAL de cada sección PE (NO toda la imagen) ──────────────────────────────
    // El fix viejo usaba textEnd = moduleBase + moduleSize (TODA la imagen ≈ 64MB) como
    // "rango .text", lo cual es demasiado laxo: aceptaría una "vtable" que en realidad
    // cayera en .rdata/.data/.bss. Aquí leemos las cabeceras de sección y calculamos:
    //   - [textStart, textEnd)  = sección .text REAL (código)            (RVA 0x1000, ~22.4MB)
    //   - [rdataStart,rdataEnd) = sección .rdata REAL (consts + VTABLES)  (RVA 0x1673000, ~5.3MB)
    // IMPORTANTE (MSVC x64): las VTABLES viven en .rdata, NO en .text. Un RootObjectFactory*
    // real tiene en su 1er qword un puntero a su vtable -> ese puntero apunta a .rdata, y las
    // ENTRADAS de esa vtable apuntan a .text. Por eso el 1er nivel de validación acepta que la
    // vtable esté en .rdata O .text (defensivo), y opcionalmente comprobamos que la 1ª entrada
    // de la vtable apunte a .text (descarta floats/basura que casualmente caigan en .rdata).
    uintptr_t textStart = moduleBase + 0x1000;     // fallback conservador
    uintptr_t textEnd   = moduleBase + moduleSize;  // fallback (toda la imagen)
    uintptr_t rdataStart = 0, rdataEnd = 0;
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                moduleSize = nt->OptionalHeader.SizeOfImage;
                textEnd = moduleBase + moduleSize; // refrescar fallback con el tamaño real
                // Recorrer las secciones para localizar .text y .rdata por nombre.
                auto* sec = IMAGE_FIRST_SECTION(nt);
                for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                    char nm[9] = {};
                    memcpy(nm, sec->Name, 8);
                    uintptr_t s = moduleBase + sec->VirtualAddress;
                    // VirtualSize puede ser 0 en binarios raros -> usar SizeOfRawData de respaldo.
                    uintptr_t vsz = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
                    uintptr_t e = s + vsz;
                    if (memcmp(nm, ".text", 5) == 0)  { textStart = s;  textEnd = e; }
                    else if (memcmp(nm, ".rdata", 6) == 0) { rdataStart = s; rdataEnd = e; }
                }
            }
        }
    }
    // Si no se halló .rdata por nombre, dejarlo vacío (la check de vtable caerá a solo-.text).
    const uintptr_t kModuleEnd = moduleBase + moduleSize;

    // ── Helper: ¿está 'a' dentro de [s, e) y el rango es válido? ──
    auto inRange = [](uintptr_t a, uintptr_t s, uintptr_t e) -> bool {
        return e > s && a >= s && a < e;
    };

    // Lambda de validación con LOGGING POR-CHECK (depuración del veredicto). Devuelve true
    // SOLO si 'p' es un RootObjectFactory* plausible. Loguea exactamente qué check falla.
    // 'tag' identifica la vía (primaria/fallback) en el log.
    auto isPlausibleFactory = [&](uintptr_t p, const char* tag) -> bool {
        // (1) No-null y dentro del rango canónico de user-space x64.
        if (p <= 0x10000 || p >= 0x00007FFFFFFFFFFF) {
            spdlog::warn("[FIX-FACTORY-GW/{}] RECHAZADO p=0x{:X}: check#1 rango user-space "
                         "(<=0x10000 o >=0x7FFF'FFFFFFFF)", tag, p);
            return false;
        }
        // (2) Alineado a 8 (todo objeto C++ con vtable lo está).
        if (p & 0x7) {
            spdlog::warn("[FIX-FACTORY-GW/{}] RECHAZADO p=0x{:X}: check#2 no alineado a 8", tag, p);
            return false;
        }
        // (3) Heap fuera del módulo (un float/relleno del propio binario NO es heap).
        if (p >= moduleBase && p < kModuleEnd) {
            spdlog::warn("[FIX-FACTORY-GW/{}] RECHAZADO p=0x{:X}: check#3 dentro del módulo "
                         "[0x{:X},0x{:X}) (no es objeto de heap)", tag, p, moduleBase, kModuleEnd);
            return false;
        }
        // (4) NUEVA — descarta el patrón "valor de 32 bits con mitad alta nula".
        // Un puntero de heap x64 bajo ASLR NUNCA tiene los 32 bits altos a cero. El valor
        // 0x590301A0 (constante, persistente) caía aquí: NO es un RootObjectFactory* real.
        // El heap de un proceso x64 vive muy por encima de 0x1'00000000. Exigimos >= 0x10000000000
        // (256 GB) que es donde residen heaps/módulos en Win x64 (módulo Kenshi ≈ 0x7FF6...).
        if ((p >> 32) == 0) {
            spdlog::warn("[FIX-FACTORY-GW/{}] RECHAZADO p=0x{:X}: check#4 mitad alta NULA "
                         "(valor de 32 bits, NO es puntero de heap x64 — p.ej. 0x590301A0)", tag, p);
            return false;
        }
        if (p < 0x10000000000ull) {
            spdlog::warn("[FIX-FACTORY-GW/{}] RECHAZADO p=0x{:X}: check#4b por debajo del rango "
                         "de heap x64 (<0x100'00000000)", tag, p);
            return false;
        }
        // (5) El 1er qword del factory debe ser su VTABLE. En MSVC x64 la vtable vive en .rdata
        // (sus entradas apuntan a .text). Aceptamos vtable en .rdata O .text (defensivo).
        uintptr_t vt = 0;
        if (!Memory::Read(p, vt)) {
            spdlog::warn("[FIX-FACTORY-GW/{}] RECHAZADO p=0x{:X}: check#5 *p ilegible (no se pudo "
                         "leer la vtable)", tag, p);
            return false;
        }
        bool vtInRdata = inRange(vt, rdataStart, rdataEnd);
        bool vtInText  = inRange(vt, textStart, textEnd);
        if (!vtInRdata && !vtInText) {
            spdlog::warn("[FIX-FACTORY-GW/{}] RECHAZADO p=0x{:X}: check#5 vtable=0x{:X} fuera de "
                         ".rdata [0x{:X},0x{:X}) y .text [0x{:X},0x{:X})",
                         tag, p, vt, rdataStart, rdataEnd, textStart, textEnd);
            return false;
        }
        // (6) Refuerzo: si la vtable está en .rdata, su 1ª entrada (1er método virtual) debe
        // apuntar a CÓDIGO (.text). Esto descarta que 'vt' sea un float/dato que casualmente
        // caiga en .rdata. Si la lectura falla, NO rechazamos (defensivo: la check#5 ya pasó).
        if (vtInRdata) {
            uintptr_t firstSlot = 0;
            if (Memory::Read(vt, firstSlot) && firstSlot != 0 && !inRange(firstSlot, textStart, textEnd)) {
                spdlog::warn("[FIX-FACTORY-GW/{}] RECHAZADO p=0x{:X}: check#6 vtable@0x{:X} en .rdata "
                             "pero su 1ª entrada=0x{:X} NO apunta a .text [0x{:X},0x{:X})",
                             tag, p, vt, firstSlot, textStart, textEnd);
                return false;
            }
        }
        spdlog::info("[FIX-FACTORY-GW/{}] ACEPTADO p=0x{:X} (vtable=0x{:X}, {}): factory plausible",
                     tag, p, vt, vtInRdata ? ".rdata" : ".text");
        return true;
    };

    // ── Vía PRIMARIA: puntero global estático theFactory @ modBase+0x21345B0 ──
    uintptr_t globalAddr = moduleBase + THEFACTORY_GLOBAL_RVA;
    uintptr_t factoryPtr = 0;
    if (Memory::Read(globalAddr, factoryPtr) && isPlausibleFactory(factoryPtr, "global")) {
        m_factoryFromGameWorld = reinterpret_cast<void*>(factoryPtr);
        m_factory = m_factoryFromGameWorld;   // IsReady() pasa a true sin depender del hook
        uintptr_t vt = 0; Memory::Read(factoryPtr, vt);
        spdlog::info("[FIX-FACTORY-GW] capturado factory=0x{:X} de theFactory global @RVA 0x{:X} "
                     "(vt=0x{:X}) — IsReady()=true", factoryPtr, THEFACTORY_GLOBAL_RVA, vt);
        return true;
    }
    spdlog::warn("SpawnManager: [FIX-FACTORY-GW] global theFactory @RVA 0x{:X} no plausible "
                 "(valor=0x{:X}) — probando fallback GW+0x{:X}",
                 THEFACTORY_GLOBAL_RVA, factoryPtr, GW_THEFACTORY_OFFSET);

    // ── DIAG-FACTORY-SLOT: volcado de vecindad del slot global ──────────────────────────
    // El slot @0x21345B0 contiene un valor no-puntero (p.ej. 0x590301A0). Dos hipótesis:
    //   (A) el factory es un singleton lazy-init aún NO construido (host conectó con la partida
    //       ya cargada -> ningún create natural llenó el slot todavía); o
    //   (B) desfase fino de offset en esta build -> el factory real está en un qword VECINO.
    // Volcamos ±0x40 alrededor del slot y marcamos qué qword SÍ pasaría isPlausibleFactory.
    // Si un vecino resulta plausible, la próxima sesión revela el offset correcto SIN parchear
    // a ciegas. Esto NO captura nada (solo diagnostica): la captura sigue gobernada por la
    // validación endurecida. Coste: 17 lecturas SEH-safe, una sola vez por intento fallido.
    {
        spdlog::info("[DIAG-FACTORY-SLOT] volcado vecindad de theFactory @RVA 0x{:X} "
                     "(modBase=0x{:X}):", THEFACTORY_GLOBAL_RVA, moduleBase);
        for (int off = -0x40; off <= 0x40; off += 8) {
            uintptr_t a = globalAddr + off;
            uintptr_t v = 0;
            bool ok = Memory::Read(a, v);
            // ¿Este vecino sería un factory plausible? (reutiliza la misma lambda, tag "diag")
            bool plausible = ok && isPlausibleFactory(v, "diag");
            spdlog::info("  [DIAG-FACTORY-SLOT] RVA 0x{:X} (off {:+d}) = 0x{:016X}{}",
                         THEFACTORY_GLOBAL_RVA + off, off, ok ? v : 0,
                         plausible ? "  <-- PLAUSIBLE (posible factory real)" : "");
        }
    }

    // ── Vía SECUNDARIA (fallback): resolver el objeto GameWorld y leer GW+0x4A0 ──
    // Solo útil si en alguna versión GameWorld fuese un puntero clásico a heap (no es el caso de
    // Steam 1.0.68, donde GW+0x4A0 == el mismo global 0x21345B0). Defensa frente a cambios de build.
    if (gwSingleton == 0) {
        spdlog::warn("SpawnManager: [FIX-FACTORY-GW] sin gwSingleton para el fallback — NO capturado");
        return false;
    }
    uintptr_t first = 0;
    uintptr_t gwObj = 0;
    if (Memory::Read(gwSingleton, first) && first != 0) {
        if (first >= textStart && first < textEnd) {
            gwObj = gwSingleton;          // instancia directa (vtable en 1er qword)
        } else if (first > 0x10000 && first < 0x00007FFFFFFFFFFF &&
                   !(first >= moduleBase && first < moduleBase + moduleSize)) {
            gwObj = first;                // puntero clásico a objeto de heap
        }
    }
    if (gwObj == 0) {
        spdlog::warn("SpawnManager: [FIX-FACTORY-GW] no se pudo resolver GameWorld desde 0x{:X} — NO capturado",
                     gwSingleton);
        return false;
    }
    factoryPtr = 0;
    if (Memory::Read(gwObj + GW_THEFACTORY_OFFSET, factoryPtr) && isPlausibleFactory(factoryPtr, "GW+off")) {
        m_factoryFromGameWorld = reinterpret_cast<void*>(factoryPtr);
        m_factory = m_factoryFromGameWorld;
        uintptr_t vt = 0; Memory::Read(factoryPtr, vt);
        spdlog::info("[FIX-FACTORY-GW] capturado factory=0x{:X} de GW+0x{:X} (gwObj=0x{:X}, vt=0x{:X}) "
                     "— IsReady()=true (vía fallback)", factoryPtr, GW_THEFACTORY_OFFSET, gwObj, vt);
        return true;
    }
    spdlog::warn("SpawnManager: [FIX-FACTORY-GW] factory de GW+0x{:X} no plausible = 0x{:X} "
                 "(gwObj=0x{:X}) — NO capturado", GW_THEFACTORY_OFFSET, factoryPtr, gwObj);
    return false;
}

// ── FIX-FACTORY-GW #3: registro/comparación del factory del hook ──
// Si el hook CharacterCreate llega a disparar, guardamos su 1er arg y lo comparamos con el
// factory que capturamos del global theFactory (RVA 0x21345B0). Confirma o refuta la captura.
void SpawnManager::NoteHookFactory(void* hookFactory) {
    if (!hookFactory) return;
    if (!m_factoryFromHook) m_factoryFromHook = hookFactory;

    // Comparar una sola vez, cuando tengamos AMBOS valores.
    if (!m_factoryMatchLogged && m_factoryFromGameWorld && m_factoryFromHook) {
        m_factoryMatchLogged = true;
        bool match = (m_factoryFromGameWorld == m_factoryFromHook);
        spdlog::info("[DIAG-FACTORY-MATCH] hook=0x{:X} gw=0x{:X} match={}",
                     reinterpret_cast<uintptr_t>(m_factoryFromHook),
                     reinterpret_cast<uintptr_t>(m_factoryFromGameWorld),
                     match ? "Y" : "N");
        if (!match) {
            // Riesgo materializado: el factory del global NO es el que usa process(). Para spawns
            // FUTUROS preferimos el del hook (validado por el motor). Desactivamos la captura
            // del global para nuevas conexiones (ResetForReconnect respeta m_factory limpio).
            spdlog::error("[DIAG-FACTORY-MATCH] DISCREPANCIA: el factory de theFactory@0x21345B0 NO "
                          "coincide con el del hook. Conmutando m_factory al del hook y desactivando "
                          "kCaptureFactoryFromGameWorld para futuras conexiones.");
            m_factory = m_factoryFromHook;
            kCaptureFactoryFromGameWorld = false;
        }
    }
}

void SpawnManager::OnGameCharacterCreated(void* factory, void* gameData, void* character) {
    // [DIAG-FACTORY-MATCH] Registrar/comparar el factory del hook con el capturado de GW+0x5B0.
    // Se hace ANTES del capture-on-first-call para no perder el valor del hook si m_factory ya
    // fue puesto por CaptureFactoryFromGameWorld.
    NoteHookFactory(factory);

    // Capture factory pointer on first call
    if (!m_factory && factory) {
        m_factory = factory;
        spdlog::info("SpawnManager: Captured RootObjectFactory at 0x{:X}",
                     reinterpret_cast<uintptr_t>(factory));
    }

    // ═══ PRIORITY 1: Extract GameData from the CHARACTER object (PREFERRED) ═══
    // The character stores a persistent reference to its GameData template,
    // managed by GameDataManager. This is safe to reuse for spawning.
    // The hook's `gameData` parameter may be a temporary/consumed request object
    // that becomes invalid after the factory call — DO NOT rely on it for spawning.
    if (character) {
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);

        // Dump first few characters' raw memory for diagnostics
        static int s_charDumpCount = 0;
        if (s_charDumpCount < 2) {
            s_charDumpCount++;
            spdlog::info("SpawnManager: Character #{} at 0x{:X} (hookParam=0x{:X}):",
                         s_charDumpCount, charPtr,
                         gameData ? reinterpret_cast<uintptr_t>(gameData) : 0);
            for (int off = 0; off <= 0x80; off += 8) {
                uintptr_t val = 0;
                Memory::Read(charPtr + off, val);
                spdlog::info("  char+0x{:02X}: 0x{:016X}", off, val);
            }
        }

        // Scan pointer-aligned offsets for a GameData backpointer.
        // GameData has: +0x10 = GameDataManager*, +0x28 = name (Kenshi std::string)
        for (int offset = 0x08; offset <= 0x200; offset += 8) {
            // Skip offsets we KNOW are other things
            if (offset == 0x10) continue;  // Faction*
            if (offset >= 0x18 && offset < 0x38) continue; // name string (32 bytes)
            if (offset >= 0x48 && offset < 0x54) continue; // position Vec3
            if (offset >= 0x58 && offset < 0x68) continue; // rotation Quat

            uintptr_t candidateGD = 0;
            if (!Memory::Read(charPtr + offset, candidateGD) || candidateGD == 0) continue;
            if (candidateGD < 0x10000 || candidateGD > 0x00007FFFFFFFFFFF) continue;

            // Validate: candidate+0x28 should have a readable ASCII name
            std::string name = ReadKenshiString(candidateGD + 0x28);
            if (name.empty() || name.length() <= 1 || name.length() >= 100) continue;

            bool validName = true;
            for (char c : name) {
                if (c < 0x20 || c > 0x7E) { validName = false; break; }
            }
            if (!validName) continue;

            // Extra validation: candidate+0x10 should be a consistent pointer
            // (GameDataManager*). If we already know the manager, check it matches.
            uintptr_t candidateMgr = 0;
            Memory::Read(candidateGD + 0x10, candidateMgr);
            if (m_managerPointer != 0 && candidateMgr != m_managerPointer) continue;
            if (candidateMgr < 0x10000 || candidateMgr > 0x00007FFFFFFFFFFF) continue;

            // Found a valid GameData from character backpointer!
            {
                std::lock_guard lock(m_templateMutex);
                m_templates[name] = reinterpret_cast<void*>(candidateGD);

                // ALSO save as factory-validated template — these GameData objects
                // were used by the factory to create actual game objects.
                m_factoryInputTemplates[name] = reinterpret_cast<void*>(candidateGD);
                m_lastFactoryInput = reinterpret_cast<void*>(candidateGD);
                m_lastFactoryInputName = name;

                // Check if this is a CHARACTER (has faction pointer at char+0x10).
                // Objects with factions are actual characters (bandits, villagers, etc.)
                // Objects WITHOUT factions are buildings, items, food, etc.
                uintptr_t factionPtr = 0;
                Memory::Read(charPtr + 0x10, factionPtr);
                bool hasCharacterFaction = (factionPtr != 0 &&
                                            factionPtr > 0x10000 &&
                                            factionPtr < 0x00007FFFFFFFFFFF);

                if (hasCharacterFaction) {
                    m_characterTemplates[name] = reinterpret_cast<void*>(candidateGD);
                    m_lastCharacterTemplate = reinterpret_cast<void*>(candidateGD);
                    m_lastCharacterTemplateName = name;
                    spdlog::info("SpawnManager: CHARACTER template '{}' from char+0x{:X} = 0x{:X} "
                                 "(faction=0x{:X}, charTemplates={})",
                                 name, offset, candidateGD, factionPtr, m_characterTemplates.size());
                }

                if (!m_characterSourcedTemplate) {
                    m_characterSourcedTemplate = reinterpret_cast<void*>(candidateGD);
                    m_managerPointer = candidateMgr;
                    spdlog::info("SpawnManager: VALIDATED template '{}' from char+0x{:X} = 0x{:X} (mgr=0x{:X})",
                                 name, offset, candidateGD, candidateMgr);
                } else {
                    spdlog::debug("SpawnManager: Additional template '{}' at 0x{:X} (factoryTotal={}, charTotal={})",
                                  name, candidateGD, m_factoryInputTemplates.size(),
                                  m_characterTemplates.size());
                }
            }
            break; // Found one valid template from this character, move on
        }
    }

    // NOTE: The hook's gameData parameter is a STACK-allocated request struct
    // (addresses like 0xEFEA60), NOT a persistent GameData object.
    // We cannot read its name or reuse it for spawning.
    // Factory-validated templates are captured from the CHARACTER BACKPOINTER
    // (char+0x40 → GameData*) in the code above.
    if (gameData && !m_defaultTemplate) {
        m_defaultTemplate = gameData;
    }
}

void SpawnManager::QueueSpawn(const SpawnRequest& request) {
    std::lock_guard lock(m_queueMutex);
    m_spawnQueue.push(request);
    spdlog::info("SpawnManager: Queued spawn for entity {} (template: '{}')",
                 request.netId, request.templateName);
    Core::Get().GetPipelineOrch().RecordEvent(
        PipelineEventType::SpawnQueued, 0, request.netId, request.owner,
        "Queued: " + request.templateName);
}

void SpawnManager::ProcessSpawnQueue() {
    // ═══ DEPRECATED — DO NOT USE ═══
    // This function consumed spawn requests from the queue and attempted to call
    // the factory with CLONED request structs. The cloned structs had internal
    // pointers relocated to new addresses, creating characters that lacked proper
    // squad membership, AI state, and internal initialization — causing crashes.
    //
    // Spawn requests are now handled EXCLUSIVELY by the in-place replay mechanism
    // in entity_hooks.cpp (Hook_CharacterCreate). The in-place replay piggybacks
    // on natural game character creation events, using the ORIGINAL stack address
    // so all internal pointers remain valid. This creates fully-initialized characters
    // that the game treats as normal squad members.
    //
    // DO NOT call this function — it intentionally does nothing to preserve the
    // spawn queue for the in-place replay.
}

int SpawnManager::ProcessSpawnQueueFromHook(void* factory) {
    // Called from inside Hook_CharacterCreate while the hook is DISABLED.
    // We can safely call the factory function directly — no HookBypass needed.
    // This runs on the game thread in the correct context (during game logic phase).

    std::queue<SpawnRequest> toProcess;
    {
        std::lock_guard lock(m_queueMutex);
        if (m_spawnQueue.empty()) return 0;
        std::swap(toProcess, m_spawnQueue);
    }

    int spawned = 0;
    std::queue<SpawnRequest> retryQueue;

    auto origFn = reinterpret_cast<FactoryProcessFn>(m_origProcess);
    if (!origFn) {
        // Re-queue everything
        std::lock_guard lock(m_queueMutex);
        std::swap(m_spawnQueue, toProcess);
        return 0;
    }

    while (!toProcess.empty()) {
        SpawnRequest req = toProcess.front();
        toProcess.pop();

        // ═══ PRIORITY 0: MOD TEMPLATE SPAWN (preferred) ═══
        // If kenshi-online.mod is loaded and we have mod templates, use them.
        // Mod templates are REAL persistent GameData objects from the game's FCS database.
        // The factory creates fully-initialized characters from these.
        void* character = nullptr;
        bool usedModTemplate = false;

        // Map owner PlayerID to mod template slot (0-based).
        // PlayerIDs start at 1, so Player 1 → slot 0, Player 2 → slot 1, etc.
        // Wraps around if more players than templates (reuses templates).
        int templateCount = m_modTemplateCount.load();
        int modSlot = 0;
        if (templateCount > 0 && req.owner > 0) {
            modSlot = (static_cast<int>(req.owner) - 1) % templateCount;
        }
        if (modSlot < 0 || modSlot >= templateCount) modSlot = 0;

        if (modSlot >= 0 && modSlot < MAX_MOD_TEMPLATES && m_modPlayerTemplates[modSlot]) {
            spdlog::info("SpawnManager: [FROM HOOK] Attempting MOD TEMPLATE spawn for entity {} "
                         "(slot={}, modGD=0x{:X}, gdOffset={}, posOffset={})",
                         req.netId, modSlot,
                         reinterpret_cast<uintptr_t>(m_modPlayerTemplates[modSlot]),
                         m_gameDataOffsetInStruct, m_positionOffsetInStruct);

            character = SpawnWithModTemplate(modSlot, req.position);
            if (character) {
                usedModTemplate = true;
                spdlog::info("SpawnManager: [FROM HOOK] MOD TEMPLATE SUCCESS — char 0x{:X} for entity {}",
                             reinterpret_cast<uintptr_t>(character), req.netId);
            } else {
                spdlog::warn("SpawnManager: [FROM HOOK] MOD TEMPLATE failed for entity {}, falling back",
                             req.netId);
            }
        }

        // ═══ FALLBACK: Original template search and spawn ═══
        if (!character) {
            void* templateData = nullptr;
            std::string templateSource;

            {
                std::lock_guard lock(m_templateMutex);

                // Priority 1: CHARACTER template matching requested name
                if (!req.templateName.empty()) {
                    auto it = m_characterTemplates.find(req.templateName);
                    if (it != m_characterTemplates.end()) {
                        templateData = it->second;
                        templateSource = "char:'" + req.templateName + "'";
                    }
                }

                // Priority 2: ANY recent character template
                if (!templateData && m_lastCharacterTemplate) {
                    templateData = m_lastCharacterTemplate;
                    templateSource = "char-last:'" + m_lastCharacterTemplateName + "'";
                }

                // Priority 3: factory-input by name (may be building/item)
                if (!templateData && !req.templateName.empty()) {
                    auto it = m_factoryInputTemplates.find(req.templateName);
                    if (it != m_factoryInputTemplates.end()) {
                        templateData = it->second;
                        templateSource = "factory:'" + req.templateName + "'";
                    }
                }

                // Priority 4: ANY factory template (last resort)
                if (!templateData && m_lastFactoryInput) {
                    templateData = m_lastFactoryInput;
                    templateSource = "factory-last:'" + m_lastFactoryInputName + "'";
                }
            }

            if (!templateData) {
                req.retryCount++;
                if (req.retryCount < MAX_SPAWN_RETRIES) {
                    if (req.retryCount % 50 == 1) {
                        spdlog::warn("SpawnManager: [FROM HOOK] No template found for entity {} "
                                     "(requested='{}', retry={}/{}, charTemplates={}, factoryTemplates={}, modTemplates={})",
                                     req.netId, req.templateName, req.retryCount, MAX_SPAWN_RETRIES,
                                     m_characterTemplates.size(), m_factoryInputTemplates.size(), m_modTemplateCount.load());
                    }
                    retryQueue.push(req);
                } else {
                    spdlog::error("SpawnManager: [FROM HOOK] DROPPING entity {} — no template '{}' "
                                  "after {} retries", req.netId, req.templateName, MAX_SPAWN_RETRIES);
                }
                continue;
            }

            spdlog::info("SpawnManager: [FROM HOOK] Fallback spawn entity {} at ({:.0f},{:.0f},{:.0f}) "
                         "factory=0x{:X} template=0x{:X} source={}",
                         req.netId, req.position.x, req.position.y, req.position.z,
                         reinterpret_cast<uintptr_t>(factory),
                         reinterpret_cast<uintptr_t>(templateData),
                         templateSource);

            // Pass the GameData template directly to the factory.
            // STRUCT CLONE REMOVED — cloned structs had stale faction pointers and
            // broken self-references that caused AV crashes at game+0x927E94.
            // The factory should accept a GameData* for character creation.
            character = SEH_CallFactory(origFn, factory, templateData);
        }

        if (character) {
            spdlog::info("SpawnManager: [FROM HOOK] SUCCESS — character 0x{:X} for entity {} (modTemplate={})",
                         reinterpret_cast<uintptr_t>(character), req.netId, usedModTemplate);

            game::CharacterAccessor accessor(character);
            if (accessor.WritePosition(req.position)) {
                spdlog::info("SpawnManager: Position set to ({:.0f},{:.0f},{:.0f})",
                             req.position.x, req.position.y, req.position.z);
            }

            // Write the local player's faction to prevent use-after-free crash
            // on faction+0x250. The mod template may reference a faction that
            // doesn't exist in the local save, but the local player's faction is
            // guaranteed valid.
            ApplyFactionFix(character);

            if (m_onSpawned) {
                m_onSpawned(req.netId, character);
            }
            spawned++;
        } else {
            spdlog::error("SpawnManager: [FROM HOOK] Factory returned null for entity {}",
                         req.netId);
            req.retryCount++;
            if (req.retryCount < MAX_SPAWN_RETRIES) {
                retryQueue.push(req);
            }
        }
    }

    if (!retryQueue.empty()) {
        std::lock_guard lock(m_queueMutex);
        while (!retryQueue.empty()) {
            m_spawnQueue.push(retryQueue.front());
            retryQueue.pop();
        }
    }

    return spawned;
}

void* SpawnManager::FindTemplate(const std::string& name) const {
    std::lock_guard lock(m_templateMutex);
    auto it = m_templates.find(name);
    return (it != m_templates.end()) ? it->second : nullptr;
}

void* SpawnManager::GetDefaultTemplate() const {
    std::lock_guard lock(m_templateMutex);
    return m_defaultTemplate;
}

size_t SpawnManager::GetTemplateCount() const {
    std::lock_guard lock(m_templateMutex);
    return m_templates.size();
}

void SpawnManager::ScanGameDataHeap() {
    // Scan the process heap for GameData objects.
    // GameData has a GameDataManager* at offset +0x10.
    // We look for the main GameDataManager pointer in memory.

    uintptr_t moduleBase = Memory::GetModuleBase();
    // Get module image size from PE header for range checks
    size_t moduleSize = 0x4000000; // 64MB fallback
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                moduleSize = nt->OptionalHeader.SizeOfImage;
        }
    }

    // ── Strategy 1: Try hardcoded offsets (fast, works if version matches) ──
    uintptr_t gdmAddress = 0;
    uintptr_t gdmValue = 0;

    uintptr_t hardcodedCandidates[] = {
        moduleBase + 0x2133060,          // GOG GameDataManagerMain
        moduleBase + 0x2133040 + 0x20,   // GOG GameWorld + dataMgr1 offset
    };

    for (auto candAddr : hardcodedCandidates) {
        uintptr_t val = 0;
        if (Memory::Read(candAddr, val) && val != 0) {
            if (val > 0x10000 && val < 0x00007FFFFFFFFFFF &&
                (val & 0x7) == 0 &&
                !(val >= moduleBase && val < moduleBase + moduleSize)) {
                // Double-dereference: a real GameDataManager should be readable
                uintptr_t check = 0;
                if (Memory::Read(val, check) && check != 0) {
                    gdmAddress = candAddr;
                    gdmValue = val;
                    spdlog::info("SpawnManager: GameDataManager found via hardcoded offset 0x{:X} -> 0x{:X}", candAddr, val);
                    break;
                }
            }
        }
    }

    // ── Strategy 2: Derive from GameWorld singleton (works on Steam + GOG) ──
    if (gdmValue == 0) {
        auto& core = Core::Get();
        uintptr_t gwAddr = core.GetGameFunctions().GameWorldSingleton;
        if (gwAddr != 0) {
            // CRITICO 1.0.68: GameWorld es una INSTANCIA embebida, NO un puntero. Resolvemos
            // el OBJETO GameWorld real manejando ambos layouts:
            //   (a) puntero clasico  : *gwAddr es heap-ptr (fuera del modulo) -> ese es el objeto
            //   (b) instancia directa: *gwAddr es la vtable (.text, dentro del modulo) -> el
            //                          objeto es gwAddr mismo
            uintptr_t first = 0;
            uintptr_t gwObj = 0;
            if (Memory::Read(gwAddr, first) && first != 0) {
                uintptr_t textStart = moduleBase + 0x1000;
                uintptr_t textEnd   = moduleBase + moduleSize;
                if (first >= textStart && first < textEnd) {
                    gwObj = gwAddr;             // (b) instancia directa
                } else if (first > 0x10000 && first < 0x00007FFFFFFFFFFF &&
                           !(first >= moduleBase && first < moduleBase + moduleSize)) {
                    gwObj = first;              // (a) puntero clasico a objeto de heap
                }
            }
            if (gwObj != 0) {
                // GameWorld+0x20 = dataMgr1 (KenshiLib verified)
                uintptr_t val = 0;
                if (Memory::Read(gwObj + 0x20, val) && val != 0 &&
                    val > 0x10000 && val < 0x00007FFFFFFFFFFF &&
                    !(val >= moduleBase && val < moduleBase + moduleSize)) {
                    gdmValue = val;
                    gdmAddress = gwObj + 0x20;
                    spdlog::info("SpawnManager: GameDataManager found via GameWorld+0x20 = 0x{:X} (gwObj=0x{:X})", val, gwObj);
                }
            }
        }
    }

    // ── Strategy 3: Scan from captured template's manager pointer ──
    if (gdmValue == 0 && !m_templates.empty()) {
        // We already have some templates. Read the manager pointer from one.
        // GameData+0x10 = GameDataManager* (KServerMod verified)
        auto it = m_templates.begin();
        uintptr_t gdPtr = reinterpret_cast<uintptr_t>(it->second);
        uintptr_t mgrPtr = 0;
        if (Memory::Read(gdPtr + 0x10, mgrPtr) && mgrPtr != 0 && mgrPtr > moduleBase) {
            gdmValue = mgrPtr;
            gdmAddress = gdPtr + 0x10;
            spdlog::info("SpawnManager: GameDataManager found via existing template '{}' = 0x{:X}",
                         it->first, mgrPtr);
        }
    }

    // ── Strategy 4: Use manager pointer from character backpointer extraction ──
    if (gdmValue == 0 && m_managerPointer != 0) {
        gdmValue = m_managerPointer;
        gdmAddress = 0; // Not from a specific address
        spdlog::info("SpawnManager: GameDataManager from character backpointer extraction = 0x{:X}",
                     m_managerPointer);
    }

    if (gdmValue == 0) {
        spdlog::warn("SpawnManager: Could not find GameDataManager, skipping heap scan");
        return;
    }

    spdlog::info("SpawnManager: GameDataManager at 0x{:X} (value 0x{:X}), scanning heap...",
                 gdmAddress, gdmValue);

    // Scan writable memory regions for pointers to gdmValue
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t scanAddr = 0;
    int found = 0;
    auto startTime = GetTickCount64();

    while (VirtualQuery(reinterpret_cast<void*>(scanAddr), &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD) &&
            mbi.RegionSize > 0 && mbi.RegionSize < 0x10000000) {

            auto* base = reinterpret_cast<const uintptr_t*>(mbi.BaseAddress);
            size_t count = mbi.RegionSize / sizeof(uintptr_t);

            for (size_t i = 0; i < count; i++) {
                uintptr_t val = 0;
                if (!SEH_ReadPointer(&base[i], val)) break; // Region became unreadable
                if (val == gdmValue) {
                    // Found a pointer to GameDataManager
                    // The GameData object starts 0x10 bytes before this
                    uintptr_t gdPtr = reinterpret_cast<uintptr_t>(&base[i]) - 0x10;

                    // Read the name from GameData+0x28
                    std::string name = ReadKenshiString(gdPtr + 0x28);
                    if (!name.empty() && name.length() > 1 && name.length() < 200) {
                        std::lock_guard lock(m_templateMutex);
                        if (m_templates.find(name) == m_templates.end()) {
                            m_templates[name] = reinterpret_cast<void*>(gdPtr);
                        }
                        // Store ALL entries for mod player names so FindModTemplates
                        // can see every candidate (faction, character, squad) and pick
                        // the right one via the numId heuristic.
                        // Match "Player 1" through "Player 16" for 16-player support.
                        if (name.size() >= 8 && name.substr(0, 7) == "Player " &&
                            name.size() <= 9) {
                            // Verify the suffix is a valid player number (1-16)
                            std::string numStr = name.substr(7);
                            bool validNum = !numStr.empty() && numStr.size() <= 2;
                            if (validNum) {
                                for (char c : numStr) { if (c < '0' || c > '9') validNum = false; }
                            }
                            if (validNum) {
                                int num = std::stoi(numStr);
                                if (num >= 1 && num <= 16) {
                                    m_modCandidates.emplace_back(name, reinterpret_cast<void*>(gdPtr));
                                }
                            }
                        }
                        found++;
                    }
                }
            }
        }

        scanAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (scanAddr < reinterpret_cast<uintptr_t>(mbi.BaseAddress)) break; // Overflow
    }

    auto elapsed = (GetTickCount64() - startTime) / 1000.0;
    spdlog::info("SpawnManager: Heap scan found {} GameData entries ({} unique templates) in {:.1f}s",
                 found, m_templates.size(), elapsed);

    // Set validated templates from heap scan results
    {
        std::lock_guard lock(m_templateMutex);
        const char* preferredTemplates[] = {
            "Greenlander", "Scorchlander", "Shek", "Hive Worker Drone",
            "greenlander", "scorchlander", "shek",
        };

        // Set character-sourced template from heap scan if not already set
        if (!m_characterSourcedTemplate) {
            for (auto tplName : preferredTemplates) {
                auto it = m_templates.find(tplName);
                if (it != m_templates.end()) {
                    m_characterSourcedTemplate = it->second;
                    spdlog::info("SpawnManager: Character-sourced template set from heap scan: '{}'", tplName);
                    break;
                }
            }
            if (!m_characterSourcedTemplate && !m_templates.empty()) {
                m_characterSourcedTemplate = m_templates.begin()->second;
                spdlog::info("SpawnManager: Character-sourced template set from heap scan: '{}' (first available)",
                             m_templates.begin()->first);
            }
        }

        // Also set default template as fallback
        if (!m_defaultTemplate && !m_templates.empty()) {
            m_defaultTemplate = m_templates.begin()->second;
        }
    }
}

void SpawnManager::SetGameDataOffset(int offset) {
    m_gameDataOffsetInStruct = offset;
    spdlog::info("SpawnManager: GameData offset in request struct = +0x{:X}", offset);
}

void SpawnManager::SetPositionOffset(int offset) {
    m_positionOffsetInStruct = offset;
    spdlog::info("SpawnManager: Position offset in request struct = +0x{:X}", offset);
}

void* SpawnManager::GetModTemplate(int playerSlot) const {
    if (playerSlot < 0 || playerSlot >= MAX_MOD_TEMPLATES) return nullptr;
    return m_modPlayerTemplates[playerSlot];
}

void SpawnManager::FindModTemplates() {
    std::lock_guard lock(m_templateMutex);

    // If we already found mod templates, don't re-search
    if (m_modTemplateCount.load() > 0) {
        return;
    }

    // If we have no mod candidates (heap scan hasn't run or found no Player entries), skip
    if (m_modCandidates.empty()) {
        spdlog::debug("SpawnManager: FindModTemplates — no mod candidates from heap scan, skipping");
        return;
    }

    // Look for "Player 1" through "Player 16" character templates from kenshi-online.mod.
    // m_modCandidates stores ALL heap-scan GameData entries named "Player N",
    // including factions, characters, and squads that share the same name.
    // We distinguish character templates by reading the numeric ID at GameData+0x08 —
    // character IDs are the highest per-player (e.g., 19/20 for Player 1/2 in the
    // original mod), so we prefer the candidate with the highest numId.
    int foundCount = 0;

    for (int slot = 0; slot < MAX_MOD_TEMPLATES; slot++) {
        std::string playerName = "Player " + std::to_string(slot + 1);
        struct Candidate { void* ptr; uint32_t numId; };
        std::vector<Candidate> candidates;

        for (auto& [name, gdPtr] : m_modCandidates) {
            if (name == playerName) {
                uint32_t numId = 0;
                Memory::Read(reinterpret_cast<uintptr_t>(gdPtr) + 0x08, numId);
                candidates.push_back({gdPtr, numId});
                spdlog::info("SpawnManager: Mod candidate '{}' at 0x{:X} numId={}",
                             playerName, reinterpret_cast<uintptr_t>(gdPtr), numId);
            }
        }

        if (candidates.empty()) {
            // Only warn for Player 1 and 2 — higher slots may not exist in smaller mods
            if (slot < 2) {
                spdlog::warn("SpawnManager: No GameData entry found for '{}' — kenshi-online.mod not loaded?",
                             playerName);
            }
            continue;
        }

        // Pick the candidate most likely to be a CHARACTER template.
        // Character IDs are the highest per-player, so prefer the highest numId.
        void* bestCandidate = nullptr;
        uint32_t bestId = 0;
        for (auto& c : candidates) {
            if (c.numId > bestId) {
                bestId = c.numId;
                bestCandidate = c.ptr;
            }
        }

        // Fallback: if all IDs are 0 (couldn't read), just use the first candidate
        if (!bestCandidate && !candidates.empty()) {
            bestCandidate = candidates[0].ptr;
            bestId = candidates[0].numId;
        }

        if (bestCandidate) {
            m_modPlayerTemplates[slot] = bestCandidate;
            foundCount++;
            spdlog::info("SpawnManager: MOD TEMPLATE slot {} = '{}' at 0x{:X} (id={})",
                         slot, playerName,
                         reinterpret_cast<uintptr_t>(bestCandidate), bestId);
        }
    }

    // Atomic store — safe for lockless reads from other threads
    m_modTemplateCount.store(foundCount);
    spdlog::info("SpawnManager: FindModTemplates complete — {} mod templates found (of {} max)",
                 foundCount, MAX_MOD_TEMPLATES);
}

void* SpawnManager::SpawnWithModTemplate(int playerSlot, const Vec3& position) {
    if (playerSlot < 0 || playerSlot >= MAX_MOD_TEMPLATES) return nullptr;
    void* modGD = m_modPlayerTemplates[playerSlot];
    if (!modGD) return nullptr;
    // Only m_factory is needed — CallFactoryCreate uses its own function pointer
    // (RVA 0x583400), not m_origProcess (the process trampoline).
    if (!m_factory) return nullptr;

    spdlog::info("SpawnManager: SpawnWithModTemplate slot={} factory=0x{:X} modGD=0x{:X} "
                 "pos=({:.0f},{:.0f},{:.0f})",
                 playerSlot, reinterpret_cast<uintptr_t>(m_factory),
                 reinterpret_cast<uintptr_t>(modGD),
                 position.x, position.y, position.z);

    // ═══ SINGLE PATH: RootObjectFactory::create ═══
    // The `create` function (RVA 0x583400) is the HIGH-LEVEL dispatcher called by
    // 11 game systems. It takes (factory, GameData*) and INTERNALLY builds a fresh
    // request struct with live pointers (faction, squad, AI), then calls process().
    // This completely bypasses the stale-pointer struct clone crash.
    //
    // REMOVED: Approaches 1-3 (raw GameData to process, struct clone, createRandomChar)
    // were crash-prone — stale faction pointers, broken self-refs, wrong appearance.
    // FactoryCreate is the ONLY safe path because it constructs fresh internal state.
    {
        void* character = entity_hooks::CallFactoryCreate(m_factory, modGD);
        if (character) {
            uintptr_t charAddr = reinterpret_cast<uintptr_t>(character);
            if (charAddr > 0x10000 && charAddr < 0x00007FFFFFFFFFFF && (charAddr & 0x7) == 0) {
                spdlog::info("SpawnManager: SpawnWithModTemplate SUCCESS — char 0x{:X}", charAddr);
                game::CharacterAccessor accessor(character);
                accessor.WritePosition(position);
                // DO NOT call ApplyFactionFix here — mod template characters have
                // persistent factions from kenshi-online.mod ("Player 1"/"Player 2").
                // Writing the LOCAL player's faction causes them to appear in the
                // squad panel, flooding it with remote characters and crashing.
                // Mod factions are always loaded (in GameDataManager), so no use-after-free.
                return character;
            }
        }
        spdlog::warn("SpawnManager: FactoryCreate returned null/invalid for slot {}", playerSlot);
    }

    spdlog::warn("SpawnManager: SpawnWithModTemplate failed for slot {}", playerSlot);
    return nullptr;
}

bool SpawnManager::VerifyReadiness() const {
    bool hasFactory = (m_factory != nullptr);
    bool hasOrigProcess = (m_origProcess != nullptr);
    bool hasPreCall = m_hasPreCallData;
    bool hasCharTemplates = false;
    bool hasAnyTemplates = false;
    size_t charCount = 0, factoryCount = 0, totalCount = 0;

    {
        std::lock_guard lock(m_templateMutex);
        charCount = m_characterTemplates.size();
        factoryCount = m_factoryInputTemplates.size();
        totalCount = m_templates.size();
        hasCharTemplates = (charCount > 0);
        hasAnyTemplates = (totalCount > 0);
    }

    spdlog::info("SpawnManager::VerifyReadiness:");
    spdlog::info("  Factory pointer:     {} (0x{:X})", hasFactory ? "YES" : "NO",
                 reinterpret_cast<uintptr_t>(m_factory));
    spdlog::info("  OrigProcess fn:      {} (0x{:X})", hasOrigProcess ? "YES" : "NO",
                 reinterpret_cast<uintptr_t>(m_origProcess));
    spdlog::info("  Pre-call data:       {} ({} bytes)", hasPreCall ? "YES" : "NO",
                 m_preCallDataSize);
    spdlog::info("  Character templates: {} ({})", hasCharTemplates ? "YES" : "NO", charCount);
    spdlog::info("  Factory templates:   {}", factoryCount);
    spdlog::info("  Total templates:     {}", totalCount);
    spdlog::info("  Manager pointer:     0x{:X}", m_managerPointer);
    int modCount = m_modTemplateCount.load();
    spdlog::info("  Mod templates:       {}", modCount);
    spdlog::info("  GameData offset:     {}", m_gameDataOffsetInStruct >= 0
                 ? ("0x" + std::to_string(m_gameDataOffsetInStruct)) : "NOT DETECTED");
    spdlog::info("  Position offset:     {}", m_positionOffsetInStruct >= 0
                 ? ("0x" + std::to_string(m_positionOffsetInStruct)) : "NOT DETECTED");

    // In-place replay path: needs factory + origProcess + preCallData
    bool inPlacePath = hasFactory && hasOrigProcess && hasPreCall;
    // Direct spawn path: needs origProcess + preCallData
    bool directPath = hasOrigProcess && hasPreCall;
    // Mod template path: needs mod templates + factory (GameData offset optional — has fallback)
    bool modTemplatePath = (modCount > 0) && hasFactory && hasOrigProcess;

    spdlog::info("  Spawn paths available:");
    spdlog::info("    Mod template (preferred): {}", modTemplatePath ? "READY" : "NOT READY");
    spdlog::info("    In-place replay (hook): {}", inPlacePath ? "READY" : "NOT READY");
    spdlog::info("    Direct spawn (fallback): {}", directPath ? "READY" : "NOT READY");

    if (!inPlacePath && !directPath && !modTemplatePath) {
        spdlog::warn("SpawnManager: NO spawn paths available! Remote characters cannot be created.");
        spdlog::warn("  This means the CharacterCreate hook did not fire during loading.");
    }

    return inPlacePath || directPath || modTemplatePath;
}

} // namespace kmp
