// ES: Implementación del puente con MyGUI: envoltorios SEH para cada llamada a la DLL,
//     resolución de símbolos exportados por nombre mangled y la API pública del puente.
// EN: MyGUI bridge implementation: SEH wrappers for every DLL call, resolution of the
//     exported symbols by mangled name and the bridge's public API.
#include "mygui_bridge.h"
#include <spdlog/spdlog.h>

namespace kmp {

// ES: Envoltorios SEH (no se permiten objetos C++ con destructor dentro). MSVC prohíbe
//     __try en funciones que necesiten deshacer objetos, por eso los std::string/vector
//     se pasan como void* y se reinterpretan dentro. Cada uno devuelve nullptr/false si
//     la llamada a MyGUI provoca una excepción estructurada (p.ej. un AV).
// EN:
// ── SEH wrappers (no C++ objects with destructors allowed) ──
// MSVC forbids __try in functions that require object unwinding.

// ES: Llama a una función sin argumentos (getters de singleton).
// EN: Calls a no-argument function (singleton getters).
static void* SEH_Call0(void* fn) {
    __try {
        return reinterpret_cast<void* (__cdecl*)()>(fn)();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ES: Gui::findWidgetT protegido.
// EN: Guarded Gui::findWidgetT.
static void* SEH_FindWidgetT(void* fn, void* gui, const void* name, bool throwExc) {
    __try {
        using Fn = void* (__fastcall*)(void*, const std::string&, bool);
        return reinterpret_cast<Fn>(fn)(gui, *reinterpret_cast<const std::string*>(name), throwExc);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ES: Widget::findWidget protegido.
// EN: Guarded Widget::findWidget.
static void* SEH_FindWidget(void* fn, void* widget, const void* name) {
    __try {
        using Fn = void* (__fastcall*)(void*, const std::string&);
        return reinterpret_cast<Fn>(fn)(widget, *reinterpret_cast<const std::string*>(name));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ES: Widget::setVisible protegido.
// EN: Guarded Widget::setVisible.
static bool SEH_SetVisible(void* fn, void* widget, bool visible) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, bool)>(fn)(widget, visible);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: TextBox::setCaptionWithReplacing protegido.
// EN: Guarded TextBox::setCaptionWithReplacing.
static bool SEH_SetCaption(void* fn, void* widget, const void* text) {
    __try {
        using Fn = void(__fastcall*)(void*, const std::string&);
        reinterpret_cast<Fn>(fn)(widget, *reinterpret_cast<const std::string*>(text));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: TextBox::getCaption protegido (devuelve el puntero crudo que deja en RAX).
// EN: Guarded TextBox::getCaption (returns the raw pointer left in RAX).
static void* SEH_GetCaption(void* fn, void* widget) {
    __try {
        return reinterpret_cast<void* (__fastcall*)(void*)>(fn)(widget);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ES: LayoutManager::unloadLayout protegido.
// EN: Guarded LayoutManager::unloadLayout.
static bool SEH_UnloadLayout(void* fn, void* layoutMgr, void* widgets) {
    __try {
        using Fn = void(__fastcall*)(void*, std::vector<void*>&);
        reinterpret_cast<Fn>(fn)(layoutMgr, *reinterpret_cast<std::vector<void*>*>(widgets));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Widget::setProperty protegido.
// EN: Guarded Widget::setProperty.
static bool SEH_SetProperty(void* fn, void* widget, const void* key, const void* value) {
    __try {
        using Fn = void(__fastcall*)(void*, const std::string&, const std::string&);
        reinterpret_cast<Fn>(fn)(widget,
            *reinterpret_cast<const std::string*>(key),
            *reinterpret_cast<const std::string*>(value));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Widget::setAlpha protegido.
// EN: Guarded Widget::setAlpha.
static bool SEH_SetAlpha(void* fn, void* widget, float alpha) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, float)>(fn)(widget, alpha);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Widget::createWidgetRealT protegido (crear hijo).
// EN: Guarded Widget::createWidgetRealT (create child).
static void* SEH_CreateWidgetReal(void* fn, void* parent, const void* type, const void* skin,
                                   float x, float y, float w, float h, int align, const void* name) {
    __try {
        using Fn = void* (__fastcall*)(void*, const std::string&, const std::string&,
                                       float, float, float, float, int, const std::string&);
        return reinterpret_cast<Fn>(fn)(parent,
            *reinterpret_cast<const std::string*>(type),
            *reinterpret_cast<const std::string*>(skin),
            x, y, w, h, align,
            *reinterpret_cast<const std::string*>(name));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ES: Gui::createWidgetRealT protegido (crear widget raíz en una capa).
// EN: Guarded Gui::createWidgetRealT (create root widget on a layer).
static void* SEH_CreateRootWidgetReal(void* fn, void* gui, const void* type, const void* skin,
                                       float x, float y, float w, float h, int align,
                                       const void* layer, const void* name) {
    __try {
        using Fn = void* (__fastcall*)(void*, const std::string&, const std::string&,
                                       float, float, float, float, int,
                                       const std::string&, const std::string&);
        return reinterpret_cast<Fn>(fn)(gui,
            *reinterpret_cast<const std::string*>(type),
            *reinterpret_cast<const std::string*>(skin),
            x, y, w, h, align,
            *reinterpret_cast<const std::string*>(layer),
            *reinterpret_cast<const std::string*>(name));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ES: loadLayout necesita un trato especial: el vector de retorno se pasa como
//     parámetro oculto (retval) justo después de 'this'.
// EN:
// loadLayout needs special handling — hidden return param for vector
static bool SEH_LoadLayout(void* fn, void* layoutMgr, void* retval,
                            const void* file, const void* prefix, void* parent) {
    __try {
        using Fn = std::vector<void*>* (__fastcall*)(
            void*, std::vector<void*>*, const std::string&, const std::string&, void*);
        reinterpret_cast<Fn>(fn)(
            layoutMgr,
            reinterpret_cast<std::vector<void*>*>(retval),
            *reinterpret_cast<const std::string*>(file),
            *reinterpret_cast<const std::string*>(prefix),
            parent);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Implementación de MyGuiBridge.
// ── MyGuiBridge implementation ──

// ES: Singleton de Meyers.
// EN: Meyers singleton.
MyGuiBridge& MyGuiBridge::Get() {
    static MyGuiBridge instance;
    return instance;
}

// ES: Localiza MyGUIEngine_x64.dll ya cargada, resuelve los símbolos exportados y
//     comprueba que los singletons existen. Idempotente (sale si ya está listo).
// EN: Finds the already-loaded MyGUIEngine_x64.dll, resolves the exported symbols
//     and checks the singletons exist. Idempotent (returns early if ready).
bool MyGuiBridge::Init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ready) return true;

    m_hMyGUI = GetModuleHandleA("MyGUIEngine_x64.dll");
    if (!m_hMyGUI) {
        // ES: No llenar el log: es normal durante el arranque temprano.
        // Don't spam — this is normal during early init
        return false;
    }

    spdlog::info("MyGuiBridge: Found MyGUIEngine_x64.dll at 0x{:X}", (uintptr_t)m_hMyGUI);

    // ES: Entrada de la tabla de símbolos: nombre mangled de MSVC, destino donde guardar
    //     el puntero resuelto y nombre legible para el log.
    // EN: Symbol table entry: MSVC mangled name, target where the resolved pointer is
    //     stored and a readable name for the log.
    struct SymbolEntry {
        const char* mangledName;
        void** target;
        const char* friendlyName;
    };

    // ES: Símbolos exportados por MyGUIEngine_x64.dll (nombres decorados de MSVC x64:
    //     "@@QEAA" = método público no-const, "@@UEAA" = método virtual, "@@SA" = estático).
    // EN: Symbols exported by MyGUIEngine_x64.dll (MSVC x64 decorated names:
    //     "@@QEAA" = public non-const method, "@@UEAA" = virtual method, "@@SA" = static).
    SymbolEntry symbols[] = {
        {
            "?getInstance@?$Singleton@VLayoutManager@MyGUI@@@MyGUI@@SAAEAVLayoutManager@2@XZ",
            reinterpret_cast<void**>(&m_fnLayoutMgrInstance),
            "LayoutManager::getInstance"
        },
        {
            "?getInstance@?$Singleton@VGui@MyGUI@@@MyGUI@@SAAEAVGui@2@XZ",
            reinterpret_cast<void**>(&m_fnGuiInstance),
            "Gui::getInstance"
        },
        {
            "?setVisible@Widget@MyGUI@@UEAAX_N@Z",
            reinterpret_cast<void**>(&m_fnSetVisible),
            "Widget::setVisible"
        },
        {
            "?setCaptionWithReplacing@TextBox@MyGUI@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
            reinterpret_cast<void**>(&m_fnSetCaption),
            "TextBox::setCaptionWithReplacing"
        },
        {
            "?getCaption@TextBox@MyGUI@@QEBA?BV?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@XZ",
            reinterpret_cast<void**>(&m_fnGetCaption),
            "TextBox::getCaption"
        },
        {
            "?findWidgetT@Gui@MyGUI@@QEAAPEAVWidget@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z",
            reinterpret_cast<void**>(&m_fnFindWidgetT),
            "Gui::findWidgetT"
        },
        {
            "?findWidget@Widget@MyGUI@@QEAAPEAV12@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
            reinterpret_cast<void**>(&m_fnFindWidget),
            "Widget::findWidget"
        },
        {
            "?loadLayout@LayoutManager@MyGUI@@QEAA?AV?$vector@PEAVWidget@MyGUI@@V?$allocator@PEAVWidget@MyGUI@@@std@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@4@0PEAVWidget@2@@Z",
            reinterpret_cast<void**>(&m_fnLoadLayout),
            "LayoutManager::loadLayout"
        },
        {
            "?unloadLayout@LayoutManager@MyGUI@@QEAAXAEAV?$vector@PEAVWidget@MyGUI@@V?$allocator@PEAVWidget@MyGUI@@@std@@@std@@@Z",
            reinterpret_cast<void**>(&m_fnUnloadLayout),
            "LayoutManager::unloadLayout"
        },
        {
            "?setProperty@Widget@MyGUI@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0@Z",
            reinterpret_cast<void**>(&m_fnSetProperty),
            "Widget::setProperty"
        },
        {
            "?setAlpha@Widget@MyGUI@@QEAAXM@Z",
            reinterpret_cast<void**>(&m_fnSetAlpha),
            "Widget::setAlpha"
        },
        {
            "?createWidgetRealT@Gui@MyGUI@@QEAAPEAVWidget@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0MMMMUAlign@2@00@Z",
            reinterpret_cast<void**>(&m_fnGuiCreateWidgetReal),
            "Gui::createWidgetRealT"
        },
        {
            "?createWidgetRealT@Widget@MyGUI@@QEAAPEAV12@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0MMMMUAlign@2@0@Z",
            reinterpret_cast<void**>(&m_fnWidgetCreateWidgetReal),
            "Widget::createWidgetRealT"
        },
    };

    // ES: Resolver cada símbolo con GetProcAddress y registrar el resultado.
    // EN: Resolve each symbol with GetProcAddress and log the result.
    int resolved = 0;
    int total = sizeof(symbols) / sizeof(symbols[0]);

    for (auto& sym : symbols) {
        FARPROC proc = GetProcAddress(m_hMyGUI, sym.mangledName);
        if (proc) {
            *sym.target = reinterpret_cast<void*>(proc);
            spdlog::info("MyGuiBridge: Resolved {} at 0x{:X}", sym.friendlyName, (uintptr_t)proc);
            resolved++;
        } else {
            spdlog::warn("MyGuiBridge: FAILED to resolve {}", sym.friendlyName);
        }
    }

    spdlog::info("MyGuiBridge: Resolved {}/{} symbols", resolved, total);

    // ES: getCaption, setProperty, setAlpha y createWidgetRealT son opcionales; el resto
    //     es obligatorio para activar la UI nativa.
    // EN: (setProperty, setAlpha and createWidgetRealT are optional as well.)
    // getCaption is optional, rest are required
    bool coreOk = m_fnLayoutMgrInstance && m_fnGuiInstance && m_fnSetVisible &&
                  m_fnSetCaption && m_fnFindWidgetT && m_fnFindWidget &&
                  m_fnLoadLayout && m_fnUnloadLayout;

    if (!coreOk) {
        spdlog::error("MyGuiBridge: Missing critical symbols, native UI disabled");
        return false;
    }

    // ES: Verificar que los singletons existen (MyGUI puede estar cargada pero sin
    //     inicializar todavía).
    // Verify singletons are alive (MyGUI may be loaded but not yet initialized)
    void* lm = SEH_Call0(reinterpret_cast<void*>(m_fnLayoutMgrInstance));
    void* gui = SEH_Call0(reinterpret_cast<void*>(m_fnGuiInstance));
    if (!lm || !gui) {
        spdlog::warn("MyGuiBridge: Singletons not ready yet (LayoutMgr={}, Gui={})",
                      lm ? "OK" : "null", gui ? "OK" : "null");
        return false;
    }

    m_ready = true;
    spdlog::info("MyGuiBridge: Ready (LayoutMgr=0x{:X}, Gui=0x{:X})", (uintptr_t)lm, (uintptr_t)gui);
    // ES: Log de comprobación de ABI: tamaños de std::string / std::vector y nivel de
    //     depuración de iteradores, que deben coincidir con los de MyGUI para poder pasar
    //     estos objetos a la DLL.
    // EN: ABI check log: std::string / std::vector sizes and iterator debug level,
    //     which must match MyGUI's so these objects can be passed to the DLL.
    spdlog::info("MyGuiBridge: ABI check — sizeof(string)={}, sizeof(vector<void*>)={}, _ITERATOR_DEBUG_LEVEL={}",
                 sizeof(std::string), sizeof(std::vector<void*>),
#ifdef _ITERATOR_DEBUG_LEVEL
                 _ITERATOR_DEBUG_LEVEL
#else
                 -1
#endif
                 );
    return true;
}

// ES: Getters de los singletons (llamada protegida por SEH).
// EN: Singleton getters (SEH-guarded call).
void* MyGuiBridge::GetLayoutManager() {
    if (!m_fnLayoutMgrInstance) return nullptr;
    return SEH_Call0(reinterpret_cast<void*>(m_fnLayoutMgrInstance));
}

void* MyGuiBridge::GetGui() {
    if (!m_fnGuiInstance) return nullptr;
    return SEH_Call0(reinterpret_cast<void*>(m_fnGuiInstance));
}

// ES: Carga un .layout (descarga antes la copia anterior del mismo fichero) y guarda
//     sus widgets raíz. Devuelve false si MyGUI crashea o no encuentra el fichero.
// EN: Loads a .layout (unloading any previous copy of the same file first) and stores
//     its root widgets. Returns false if MyGUI crashes or cannot find the file.
bool MyGuiBridge::LoadLayout(const std::string& layoutFile, const std::string& prefix) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready) return false;

    void* layoutMgr = GetLayoutManager();
    if (!layoutMgr) {
        spdlog::error("MyGuiBridge: LayoutManager singleton is null");
        return false;
    }

    // ES: Descargar este layout si ya estaba cargado (el lock ya está tomado).
    // Unload this specific layout if it was already loaded (lock already held)
    UnloadLayoutImpl(layoutFile);

    spdlog::info("MyGuiBridge: Loading layout '{}' (fn=0x{:X}, mgr=0x{:X})",
                 layoutFile, (uintptr_t)m_fnLoadLayout, (uintptr_t)layoutMgr);

    std::vector<void*> result;
    bool callOk = SEH_LoadLayout(reinterpret_cast<void*>(m_fnLoadLayout),
                                  layoutMgr, &result, &layoutFile, &prefix, nullptr);

    if (!callOk) {
        spdlog::error("MyGuiBridge: loadLayout CRASHED (SEH caught exception) for '{}'", layoutFile);
        return false;
    }

    spdlog::info("MyGuiBridge: loadLayout returned {} widgets for '{}'",
                 result.size(), layoutFile);

    if (result.empty()) {
        spdlog::error("MyGuiBridge: loadLayout returned empty — file not found in resource system");
        return false;
    }

    spdlog::info("MyGuiBridge: Loaded {} root widgets from '{}'", result.size(), layoutFile);
    m_loadedLayouts[layoutFile] = std::move(result);
    return true;
}

// ES: Descarga un layout concreto (versión pública con lock).
// EN: Unloads a given layout (public, locking version).
void MyGuiBridge::UnloadLayout(const std::string& layoutFile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    UnloadLayoutImpl(layoutFile);
}

// ES: Descarga un layout concreto sin tomar el lock y lo quita del mapa.
// EN: Unloads a given layout without locking and removes it from the map.
void MyGuiBridge::UnloadLayoutImpl(const std::string& layoutFile) {
    auto it = m_loadedLayouts.find(layoutFile);
    if (it == m_loadedLayouts.end() || it->second.empty()) return;
    if (!m_ready) return;

    void* layoutMgr = GetLayoutManager();
    if (layoutMgr) {
        if (!SEH_UnloadLayout(reinterpret_cast<void*>(m_fnUnloadLayout), layoutMgr, &it->second)) {
            spdlog::error("MyGuiBridge: unloadLayout crashed for '{}'", layoutFile);
        }
    }
    m_loadedLayouts.erase(it);
}

// ES: Descarga todos los layouts cargados.
// EN: Unloads every loaded layout.
void MyGuiBridge::UnloadAllLayouts() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready) return;

    void* layoutMgr = GetLayoutManager();
    for (auto& [name, widgets] : m_loadedLayouts) {
        if (!widgets.empty() && layoutMgr) {
            if (!SEH_UnloadLayout(reinterpret_cast<void*>(m_fnUnloadLayout), layoutMgr, &widgets)) {
                spdlog::error("MyGuiBridge: unloadLayout crashed for '{}'", name);
            }
        }
    }
    m_loadedLayouts.clear();
}

// ES: Busca un widget por nombre en toda la GUI (sin lanzar excepción si no existe).
// EN: Finds a widget by name in the whole GUI (without throwing if missing).
void* MyGuiBridge::FindWidget(const std::string& name) {
    if (!m_ready || !m_fnFindWidgetT) return nullptr;

    void* gui = GetGui();
    if (!gui) return nullptr;

    return SEH_FindWidgetT(reinterpret_cast<void*>(m_fnFindWidgetT), gui, &name, false);
}

// ES: Busca un widget hijo por nombre dentro de 'parent'.
// EN: Finds a child widget by name inside 'parent'.
void* MyGuiBridge::FindChildWidget(void* parent, const std::string& name) {
    if (!m_ready || !parent || !m_fnFindWidget) return nullptr;
    return SEH_FindWidget(reinterpret_cast<void*>(m_fnFindWidget), parent, &name);
}

// ES: Muestra u oculta un widget.
// EN: Shows or hides a widget.
void MyGuiBridge::SetVisible(void* widget, bool visible) {
    if (!m_ready || !widget || !m_fnSetVisible) return;
    if (!SEH_SetVisible(reinterpret_cast<void*>(m_fnSetVisible), widget, visible)) {
        spdlog::error("MyGuiBridge: setVisible crashed");
    }
}

// ES: Pone el texto de un widget de texto.
// EN: Sets a text widget's caption.
void MyGuiBridge::SetCaption(void* widget, const std::string& text) {
    if (!m_ready || !widget || !m_fnSetCaption) return;
    if (!SEH_SetCaption(reinterpret_cast<void*>(m_fnSetCaption), widget, &text)) {
        spdlog::error("MyGuiBridge: setCaptionWithReplacing crashed");
    }
}

// ES: Decodifica el layout de basic_string<unsigned short> (SSO si capacidad < 8) y
//     copia solo los caracteres ASCII imprimibles (32-127); el resto se descarta.
// EN: Decodes the basic_string<unsigned short> layout (SSO when capacity < 8) and
//     copies only printable ASCII characters (32-127); anything else is dropped.
std::string MyGuiBridge::DecodeUString(const UStringLayout* ustr) {
    if (!ustr) return "";

    size_t len = ustr->size;
    if (len == 0 || len > 4096) return "";

    // ES: SSO de basic_string<unsigned short>: capacidad < 8 = buffer interno, >= 8 = puntero al heap.
    // basic_string<unsigned short> SSO: capacity < 8 means inline buf, >= 8 means heap ptr
    const unsigned short* data;
    if (ustr->capacity >= 8) {
        data = ustr->ptr;
    } else {
        data = ustr->buf;
    }

    if (!data) return "";

    // ES: Comprobar que el puntero parece válido.
    // Validate the pointer looks sane
    uintptr_t addr = reinterpret_cast<uintptr_t>(data);
    if (addr < 0x10000 || addr > 0x00007FFFFFFFFFFF) return "";

    std::string result;
    result.reserve(len);
    for (size_t i = 0; i < len; i++) {
        unsigned short ch = data[i];
        if (ch >= 32 && ch < 128) {
            result.push_back(static_cast<char>(ch));
        }
    }
    return result;
}

// ES: Lee el texto de un widget: llama a getCaption, valida el puntero devuelto y lo
//     decodifica como UString (ver la nota sobre el retorno por valor en el .h).
// EN: Reads a widget's text: calls getCaption, validates the returned pointer and
//     decodes it as a UString (see the by-value return note in the .h).
std::string MyGuiBridge::GetCaption(void* widget) {
    if (!m_ready || !widget || !m_fnGetCaption) return "";

    void* ustringPtr = SEH_GetCaption(reinterpret_cast<void*>(m_fnGetCaption), widget);
    if (!ustringPtr) return "";

    // ES: Validar el puntero al UString.
    // Validate the UString pointer
    uintptr_t addr = reinterpret_cast<uintptr_t>(ustringPtr);
    if (addr < 0x10000 || addr > 0x00007FFFFFFFFFFF) return "";

    return DecodeUString(reinterpret_cast<const UStringLayout*>(ustringPtr));
}

// ES: Fija una propiedad del widget (clave/valor).
// EN: Sets a widget property (key/value).
void MyGuiBridge::SetProperty(void* widget, const std::string& key, const std::string& value) {
    if (!m_ready || !widget || !m_fnSetProperty) return;
    if (!SEH_SetProperty(reinterpret_cast<void*>(m_fnSetProperty), widget, &key, &value)) {
        spdlog::error("MyGuiBridge: setProperty('{}', '{}') crashed", key, value);
    }
}

// ES: Fija la transparencia del widget.
// EN: Sets the widget's alpha.
void MyGuiBridge::SetAlpha(void* widget, float alpha) {
    if (!m_ready || !widget || !m_fnSetAlpha) return;
    if (!SEH_SetAlpha(reinterpret_cast<void*>(m_fnSetAlpha), widget, alpha)) {
        spdlog::error("MyGuiBridge: setAlpha crashed");
    }
}

// ES: Crea un widget hijo por código dentro de 'parent'.
// EN: Creates a child widget programmatically inside 'parent'.
void* MyGuiBridge::CreateChildWidget(void* parent, const std::string& type, const std::string& skin,
                                      float x, float y, float w, float h, int align, const std::string& name) {
    if (!m_ready || !parent || !m_fnWidgetCreateWidgetReal) return nullptr;
    void* widget = SEH_CreateWidgetReal(reinterpret_cast<void*>(m_fnWidgetCreateWidgetReal),
                                         parent, &type, &skin, x, y, w, h, align, &name);
    if (!widget) {
        spdlog::error("MyGuiBridge: createWidgetRealT failed for '{}'", name);
    }
    return widget;
}

// ES: Crea un widget raíz por código en la capa indicada.
// EN: Creates a root widget programmatically on the given layer.
void* MyGuiBridge::CreateRootWidget(const std::string& type, const std::string& skin,
                                     float x, float y, float w, float h, int align,
                                     const std::string& layer, const std::string& name) {
    if (!m_ready || !m_fnGuiCreateWidgetReal) return nullptr;
    void* gui = GetGui();
    if (!gui) return nullptr;
    void* widget = SEH_CreateRootWidgetReal(reinterpret_cast<void*>(m_fnGuiCreateWidgetReal),
                                             gui, &type, &skin, x, y, w, h, align, &layer, &name);
    if (!widget) {
        spdlog::error("MyGuiBridge: Gui::createWidgetRealT failed for '{}'", name);
    }
    return widget;
}

} // namespace kmp
