// ES: Puente mínimo en tiempo de ejecución con MyGUI, la librería de interfaz que usa
//     Kenshi (MyGUIEngine_x64.dll, cargada por el propio juego). En vez de enlazar con
//     sus cabeceras/librerías, resuelve con GetProcAddress los símbolos exportados
//     (nombres "mangled" de C++ de MSVC) y los llama a través de punteros a función,
//     siempre envueltos en SEH para que un fallo dentro de MyGUI no tumbe el juego.
//     Lo usan el HUD y el menú nativos (native_hud / native_menu).
// EN: Minimal runtime bridge to MyGUI, the UI library Kenshi uses
//     (MyGUIEngine_x64.dll, loaded by the game itself). Instead of linking against its
//     headers/libs, it resolves the exported symbols (MSVC C++ mangled names) with
//     GetProcAddress and calls them through function pointers, always wrapped in SEH
//     so a crash inside MyGUI does not take the game down. Used by the native HUD
//     and menu (native_hud / native_menu).
#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <Windows.h>

namespace kmp {

// ES: Puente fino con MyGUI vía GetProcAddress sobre MyGUIEngine_x64.dll. Sin cabeceras
//     ni librerías de importación: todas las llamadas van por punteros resueltos.
// EN:
// Thin runtime bridge to MyGUI via GetProcAddress on MyGUIEngine_x64.dll.
// No headers or import libs needed — all calls go through resolved function pointers.
class MyGuiBridge {
public:
    // ES: Instancia única del puente.
    // EN: The bridge's single instance.
    static MyGuiBridge& Get();

    // ES: Resuelve todos los punteros a función y comprueba que los singletons de MyGUI
    //     (Gui y LayoutManager) ya existen. Devuelve false si falta la DLL, algún símbolo
    //     crítico o MyGUI aún no está inicializado (se reintenta más tarde).
    // EN:
    // Resolve all function pointers. Returns false if DLL not found.
    bool Init();
    bool IsReady() const { return m_ready; }

    // ES: Carga un fichero .layout de MyGUI (lo busca en el sistema de recursos del juego)
    //     y guarda sus widgets raíz por nombre de fichero. Se pueden tener varios a la vez.
    //     Unload* descarga uno o todos los layouts cargados.
    // EN:
    // Load a .layout file, returns widgets. prefix/parent can be empty/null.
    // Multiple layouts can be loaded simultaneously (tracked by name).
    bool LoadLayout(const std::string& layoutFile, const std::string& prefix = "");
    void UnloadLayout(const std::string& layoutFile);
    void UnloadAllLayouts();

    // ES: Busca un widget por nombre en todo el árbol de la GUI.
    // EN:
    // Find a widget by name (searches entire GUI tree)
    void* FindWidget(const std::string& name);

    // ES: Busca un widget hijo dentro de un widget padre.
    // EN:
    // Find a child widget within a parent widget
    void* FindChildWidget(void* parent, const std::string& name);

    // ES: Muestra u oculta un widget.
    // EN:
    // Set widget visibility
    void SetVisible(void* widget, bool visible);

    // ES: Pone el texto (caption) de un TextBox o EditBox (admite etiquetas #{...} de MyGUI).
    // EN:
    // Set caption text on a TextBox or EditBox
    void SetCaption(void* widget, const std::string& text);

    // ES: Lee el texto de un EditBox (decodificando el UString interno de MyGUI).
    // EN:
    // Get caption text from an EditBox (reads from MyGUI's internal string)
    std::string GetCaption(void* widget);

    // ES: Fija cualquier propiedad de un widget por clave/valor.
    // EN:
    // Set any widget property by key/value (e.g., "TextColour" = "0.9 0.55 0.1")
    void SetProperty(void* widget, const std::string& key, const std::string& value);

    // ES: Fija la transparencia del widget (0.0 - 1.0).
    // EN:
    // Set widget alpha (0.0 - 1.0)
    void SetAlpha(void* widget, float alpha);

    // ES: Crea un widget hijo por código (alternativa si falla la carga del layout).
    // EN:
    // Create a child widget programmatically (fallback when layout loading fails)
    void* CreateChildWidget(void* parent, const std::string& type, const std::string& skin,
                            float x, float y, float w, float h, int align, const std::string& name);

    // ES: Crea un widget raíz en una capa (layer) de MyGUI.
    // EN:
    // Create a root widget on a layer
    void* CreateRootWidget(const std::string& type, const std::string& skin,
                           float x, float y, float w, float h, int align,
                           const std::string& layer, const std::string& name);

    // ES: Devuelve el singleton LayoutManager de MyGUI.
    // EN:
    // Get the LayoutManager singleton
    void* GetLayoutManager();

    // ES: Devuelve el singleton Gui de MyGUI.
    // EN:
    // Get the Gui singleton
    void* GetGui();

private:
    MyGuiBridge() = default;

    // ES: Tipos de puntero a función de los métodos de MyGUI. En x64 los métodos miembro
    //     se llaman con 'this' en RCX, así que se declaran como funciones libres con el
    //     widget/objeto como primer parámetro (__fastcall se ignora en x64).
    // EN: Function pointer types for the MyGUI methods. On x64 member functions receive
    //     'this' in RCX, so they are declared as free functions with the widget/object as
    //     first parameter (__fastcall is ignored on x64).
    // ── Function pointer types ──

    // ES: Getters estáticos de singletons (sin puntero this).
    // Static singleton getters (no this pointer)
    using FnGetInstance = void* (__cdecl*)();

    // ES: Widget::setVisible(bool).
    // Widget::setVisible(bool)
    using FnSetVisible = void(__fastcall*)(void* widget, bool visible);

    // ES: TextBox::setCaptionWithReplacing(const std::string&).
    // TextBox::setCaptionWithReplacing(const std::string&)
    using FnSetCaption = void(__fastcall*)(void* widget, const std::string& text);

    // ES: TextBox::getCaption() — el comentario original dice que devuelve const UString&
    //     (UString = basic_string<unsigned short>) y se trata el valor de retorno como
    //     puntero crudo para decodificarlo a mano. OJO: el símbolo mangled que se resuelve
    //     en el .cpp ("QEBA?BV...") indica retorno POR VALOR, que en MSVC x64 usa un
    //     parámetro oculto de retorno; si es así, esta firma sería incorrecta (sin verificar).
    // EN: TextBox::getCaption() — the original comment says it returns const UString&
    //     (UString = basic_string<unsigned short>) and the return value is treated as a raw
    //     pointer decoded by hand. NOTE: the mangled symbol resolved in the .cpp
    //     ("QEBA?BV...") indicates a BY-VALUE return, which on MSVC x64 uses a hidden
    //     return parameter; if so, this signature would be wrong (not verified).
    // TextBox::getCaption() — returns const UString& where UString = basic_string<unsigned short>
    // We return a raw pointer and decode the UString structure manually to avoid SSO mismatch.
    using FnGetCaption = void* (__fastcall*)(void* widget);

    // ES: Gui::findWidgetT(nombre, throwExc) — busca en todo el árbol.
    // Gui::findWidgetT(const std::string& name, bool throwExc)
    using FnFindWidgetT = void* (__fastcall*)(void* gui, const std::string& name, bool throwExc);

    // ES: Widget::findWidget(nombre) — busca entre los hijos.
    // Widget::findWidget(const std::string& name)
    using FnFindWidget = void* (__fastcall*)(void* widget, const std::string& name);

    // ES: LayoutManager::loadLayout(retval, fichero, prefijo, padre). Devuelve un
    //     std::vector<Widget*> mediante el parámetro oculto de retorno (convención MSVC x64
    //     para valores de retorno grandes): el llamante pasa el vector a rellenar en RDX.
    // LayoutManager::loadLayout(retval, file, prefix, parent)
    // Returns std::vector<Widget*> via hidden return param (MSVC x64 convention for large return values)
    using FnLoadLayout = std::vector<void*>* (__fastcall*)(
        void* layoutMgr, std::vector<void*>* retval,
        const std::string& file, const std::string& prefix, void* parent);

    // ES: LayoutManager::unloadLayout(std::vector<Widget*>&).
    // LayoutManager::unloadLayout(std::vector<Widget*>&)
    using FnUnloadLayout = void(__fastcall*)(void* layoutMgr, std::vector<void*>& widgets);

    // ES: Widget::setProperty(clave, valor).
    // Widget::setProperty(const std::string& key, const std::string& value)
    using FnSetProperty = void(__fastcall*)(void* widget, const std::string& key, const std::string& value);

    // ES: Widget::setAlpha(float).
    // Widget::setAlpha(float)
    using FnSetAlpha = void(__fastcall*)(void* widget, float alpha);

    // ES: Gui::createWidgetRealT(tipo, skin, x, y, w, h, align, capa, nombre) -> Widget*
    //     (coordenadas relativas 0..1 a la pantalla).
    // Gui::createWidgetRealT(type, skin, x, y, w, h, align, layer, name) -> Widget*
    using FnGuiCreateWidgetReal = void* (__fastcall*)(void* gui, const std::string& type,
        const std::string& skin, float x, float y, float w, float h, int align,
        const std::string& layer, const std::string& name);

    // ES: Widget::createWidgetRealT(tipo, skin, x, y, w, h, align, nombre) -> Widget*
    //     (coordenadas relativas 0..1 al padre).
    // Widget::createWidgetRealT(type, skin, x, y, w, h, align, name) -> Widget*
    using FnWidgetCreateWidgetReal = void* (__fastcall*)(void* parent, const std::string& type,
        const std::string& skin, float x, float y, float w, float h, int align,
        const std::string& name);

    // ES: Layout manual de basic_string<unsigned short> de MSVC para leer el UString:
    //     unión de 16 bytes (buffer SSO o puntero al heap), size_t tamaño, size_t capacidad.
    //     Con capacidad < 8 el texto está dentro del buffer; con >= 8 está en el heap.
    // Manual MSVC basic_string<unsigned short> layout for reading UString
    // MSVC x64 std::basic_string<T> layout: 16-byte union (SSO buf or heap ptr), size_t size, size_t capacity
    struct UStringLayout {
        union {
            unsigned short buf[8];   // SSO buffer: 16 bytes / sizeof(unsigned short) = 8 elements
            unsigned short* ptr;     // heap pointer when capacity >= 8
        };
        size_t size;
        size_t capacity;
    };

    // ES: Convierte un UString a std::string quedándose solo con ASCII imprimible.
    // Decode UString to std::string (ASCII extraction)
    static std::string DecodeUString(const UStringLayout* ustr);

    // ES: Descarga interna (el llamante debe tener m_mutex).
    // Internal unload (caller must hold m_mutex)
    void UnloadLayoutImpl(const std::string& layoutFile);

    // ES: Punteros resueltos a las funciones exportadas de MyGUI.
    // EN:
    // ── Resolved pointers ──
    HMODULE           m_hMyGUI = nullptr;
    FnGetInstance     m_fnLayoutMgrInstance = nullptr;
    FnGetInstance     m_fnGuiInstance = nullptr;
    FnSetVisible      m_fnSetVisible = nullptr;
    FnSetCaption      m_fnSetCaption = nullptr;
    FnGetCaption      m_fnGetCaption = nullptr;
    FnFindWidgetT     m_fnFindWidgetT = nullptr;
    FnFindWidget      m_fnFindWidget = nullptr;
    FnLoadLayout      m_fnLoadLayout = nullptr;
    FnUnloadLayout    m_fnUnloadLayout = nullptr;
    FnSetProperty     m_fnSetProperty = nullptr;
    FnSetAlpha        m_fnSetAlpha = nullptr;
    FnGuiCreateWidgetReal       m_fnGuiCreateWidgetReal = nullptr;
    FnWidgetCreateWidgetReal    m_fnWidgetCreateWidgetReal = nullptr;

    // ES: Mutex, layouts cargados (fichero -> widgets raíz) y bandera de listo.
    // EN: Mutex, loaded layouts (file -> root widgets) and ready flag.
    mutable std::mutex m_mutex;
    std::map<std::string, std::vector<void*>> m_loadedLayouts;
    bool m_ready = false;
};

} // namespace kmp
