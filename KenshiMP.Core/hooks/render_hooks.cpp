// ES: Implementación de los hooks de render. No se busca ningún patrón AOB del juego: la
//     dirección de IDXGISwapChain::Present se obtiene creando un device + swap chain D3D11
//     temporales y leyendo su vtable (índice 8), y se desvía con MinHook (HookManager).
//     HookPresent corre en el HILO DE RENDER del juego (el que llama a Present, que en Kenshi
//     es el hilo principal/Ogre); HookWndProc corre en el hilo que bombea los mensajes de la
//     ventana. No se dibuja nada con ImGui/GDI: toda la UI es MyGUI nativo (NativeHud).
// EN: Render hooks implementation. No game AOB pattern is scanned: the address of
//     IDXGISwapChain::Present is obtained by creating a temporary D3D11 device + swap chain and
//     reading its vtable (index 8), and it is detoured with MinHook (HookManager).
//     HookPresent runs on the game's RENDER THREAD (the one calling Present, which in Kenshi is
//     the main/Ogre thread); HookWndProc runs on the thread pumping the window's messages.
//     Nothing is drawn with ImGui/GDI: all UI is native MyGUI (NativeHud).
#include "render_hooks.h"
#include "../core.h"
#include "entity_hooks.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace kmp::render_hooks {

// ES: Mensaje propio de ventana para procesar la cola de spawn — OBSOLETO. Se consumía la cola
//     antes de que el replay in-place (entity_hooks) pudiera usarla; hoy se ignora.
// EN: Custom message for spawn queue processing — DEPRECATED.
// ProcessSpawnQueue() consumed the queue before the in-place replay (entity_hooks)
// could use it. The in-place replay is the ONLY safe spawn mechanism.
static constexpr UINT WM_KMP_SPAWN = WM_USER + 100;

// ── State ──
// ES: Ventana del juego (sacada del swap chain) y WndProc original para encadenar.
// EN: Game window (taken from the swap chain) and the original WndProc to chain to.
static HWND                  s_hwnd = nullptr;
static WNDPROC               s_originalWndProc = nullptr;

// ── Types ──
// ES: Firma de IDXGISwapChain::Present(this, SyncInterval, Flags) y trampolín a la original.
// EN: Signature of IDXGISwapChain::Present(this, SyncInterval, Flags) and trampoline to the original.
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
static PresentFn s_originalPresent = nullptr;

// ── SEH wrapper for spawn queue processing from WndProc ──
// DISABLED: ProcessSpawnQueue() consumed requests before the in-place replay
// (entity_hooks) could use them. In-place replay is the only safe spawn mechanism.

// ── SEH wrapper for OnGameTick ──
// ES: Llama a Core::OnGameTick(dt) protegido con SEH (__try/__except) para que una excepción
//     nativa (p.ej. acceso a memoria inválida del juego) no tumbe Kenshi. Registra el paso en
//     el que falló (GetLastCompletedStep); limita el log a los 10 primeros y luego 1 de cada 100.
// EN: Calls Core::OnGameTick(dt) under SEH (__try/__except) so a native exception (e.g. invalid
//     game memory access) does not kill Kenshi. Logs the step that failed (GetLastCompletedStep);
//     throttles logging to the first 10 and then 1 in every 100.
static void SEH_OnGameTick(float dt) {
    __try {
        Core::Get().OnGameTick(dt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_crashCount = 0;
        s_crashCount++;
        int lastStep = Core::Get().GetLastCompletedStep();
        DWORD code = GetExceptionCode();
        if (s_crashCount <= 10 || s_crashCount % 100 == 0) {
            char buf[256];
            sprintf_s(buf, "KMP: OnGameTick SEH crash #%d — exception 0x%08lX at step %d "
                           "(dt=%.4f)\n", s_crashCount, code, lastStep, dt);
            OutputDebugStringA(buf);
            spdlog::error("render_hooks: OnGameTick SEH crash #{} — exception 0x{:08X} "
                          "at step {} (dt={:.4f})", s_crashCount, code, lastStep, dt);
        }
    }
}

// ES: Rectángulo del botón MULTIPLAYER del menú principal en coordenadas normalizadas (0..1),
//     copiado del layout MyGUI Kenshi_MainMenu.layout. Se usa para detectar el clic en WndProc.
// EN: MULTIPLAYER button rectangle on the main menu in normalized coordinates (0..1), copied
//     from the MyGUI layout Kenshi_MainMenu.layout. Used to detect the click in WndProc.
// ── MULTIPLAYER button bounds (from Kenshi_MainMenu.layout position_real) ──
static constexpr float MP_BTN_X = 0.260417f;
static constexpr float MP_BTN_Y = 0.582407f;  // Must match Kenshi_MainMenu.layout MultiplayerButton position
static constexpr float MP_BTN_W = 0.15625f;
static constexpr float MP_BTN_H = 0.0638889f;

// ── Startup timestamp: don't allow native menu until main menu is likely loaded ──
static auto s_firstPresentTime = std::chrono::steady_clock::time_point{};
static bool s_firstPresentRecorded = false;

// ES: true si han pasado al menos 15 s desde el primer Present (fin aproximado del logo/splash,
//     cuando los recursos MyGUI ya están cargados). Evita abrir el menú nativo demasiado pronto.
// EN: true once at least 15 s have passed since the first Present (approximate end of the
//     logo/splash, when MyGUI resources are loaded). Prevents opening the native menu too early.
static bool IsMainMenuReady() {
    // Don't allow native menu for the first 15 seconds after first Present.
    // The logo/splash screen runs during this time — MyGUI resources aren't loaded yet.
    if (!s_firstPresentRecorded) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s_firstPresentTime);
    return elapsed.count() >= 15;
}

// ── WndProc Hook (pure Win32 input — no ImGui) ──
// Inner function does the actual work — called from SEH wrapper.
// ES: Procesa los mensajes de ventana antes que el juego. Devuelve 0 para "consumir" el mensaje
//     (el juego no lo ve) o llama al WndProc original para dejarlo pasar. Orden: teclas globales
//     (F1 menú, Tab lista de jugadores, Insert log, ` debug, Esc cerrar, Enter chat), después
//     las compuertas modales (con chat o menú abiertos se traga todo el teclado) y por último el
//     clic izquierdo (panel nativo o botón MULTIPLAYER del menú principal).
//     El test `!(lParam & 0x40000000)` ignora la autorrepetición (bit 30 = tecla ya pulsada).
// EN: Handles window messages before the game does. Returns 0 to "consume" the message (the
//     game never sees it) or calls the original WndProc to let it through. Order: global keys
//     (F1 menu, Tab player list, Insert log, ` debug, Esc close, Enter chat), then the modal
//     gates (with chat or menu open all keyboard input is swallowed) and finally left click
//     (native panel or main-menu MULTIPLAYER button).
//     The `!(lParam & 0x40000000)` test ignores auto-repeat (bit 30 = key was already down).
static LRESULT WndProcInner(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // WM_KMP_SPAWN: no longer used — spawn queue is handled by in-place replay only
    if (uMsg == WM_KMP_SPAWN) {
        return 0;
    }

    // F1 key: toggle native menu (ignore auto-repeat: bit 30 of lParam = previous key state)
    if (uMsg == WM_KEYDOWN && wParam == VK_F1 && !(lParam & 0x40000000)) {
        auto& overlay = Core::Get().GetOverlay();
        auto& nativeMenu = overlay.GetNativeMenu();
        if (nativeMenu.IsVisible()) {
            nativeMenu.Hide();
        } else if (!Core::Get().IsGameLoaded() && !IsMainMenuReady()) {
            OutputDebugStringA("KMP: F1 pressed too early (logo/splash) — ignoring\n");
        } else {
            // Works on main menu AND in-game
            nativeMenu.Show();
        }
        return 0; // consume the key
    }

    // Tab key: toggle player list on native HUD (ignore auto-repeat)
    if (uMsg == WM_KEYDOWN && wParam == VK_TAB && !(lParam & 0x40000000)) {
        if (Core::Get().IsGameLoaded() && Core::Get().IsConnected()) {
            Core::Get().GetNativeHud().TogglePlayerList();
            return 0; // consume — prevent Kenshi Tab action (inventory switch)
        }
    }

    // Insert key: toggle loading/debug log panel (native MyGUI)
    if (uMsg == WM_KEYDOWN && wParam == VK_INSERT && !(lParam & 0x40000000)) {
        Core::Get().GetNativeHud().ToggleLogPanel();
        return 0;
    }

    // Backtick key: toggle debug info on native HUD (ignore auto-repeat)
    if (uMsg == WM_KEYDOWN && wParam == VK_OEM_3 && !(lParam & 0x40000000)) {
        if (Core::Get().IsGameLoaded() && Core::Get().IsConnected()) {
            Core::Get().GetNativeHud().ToggleDebugInfo();
            return 0; // consume — prevent Kenshi console/debug action
        }
    }

    // Escape key: close chat input or native menu (ignore auto-repeat)
    if (uMsg == WM_KEYDOWN && wParam == VK_ESCAPE && !(lParam & 0x40000000)) {
        auto& nativeHud = Core::Get().GetNativeHud();
        if (nativeHud.IsChatInputActive()) {
            nativeHud.CloseChatInput();
            return 0;
        }
        auto& nativeMenu = Core::Get().GetOverlay().GetNativeMenu();
        if (nativeMenu.IsVisible()) {
            nativeMenu.OnKeyDown(VK_ESCAPE);
            nativeMenu.Hide();
            return 0;
        }
    }

    // Enter key: toggle chat input (when game loaded, no menu open)
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN && !(lParam & 0x40000000)) {
        auto& nativeMenu = Core::Get().GetOverlay().GetNativeMenu();
        if (!nativeMenu.IsVisible() && Core::Get().IsGameLoaded()) {
            auto& nativeHud = Core::Get().GetNativeHud();
            if (nativeHud.IsChatInputActive()) {
                nativeHud.OnChatKeyDown(VK_RETURN);
            } else {
                nativeHud.OpenChatInput();
            }
            return 0;
        }
    }

    // ── Modal input gates ──
    // ES: Con el chat o el menú activos se consume TODO el teclado para que el juego
    //     (OIS/MyGUI/DirectInput) no procese también las teclas (evita doble escritura).
    // EN: (see below)
    // When chat or menu is active, consume ALL keyboard input to prevent
    // the game (OIS/MyGUI/DirectInput) from also processing keystrokes.
    // This fixes double-typing and prevents game actions while UI is open.
    bool chatActive = Core::Get().GetNativeHud().IsChatInputActive();
    bool menuVisible = Core::Get().GetOverlay().GetNativeMenu().IsVisible();

    // WM_CHAR: forward printable characters to active UI, always consume when modal
    if (uMsg == WM_CHAR) {
        if (chatActive) {
            Core::Get().GetNativeHud().OnChatChar(static_cast<wchar_t>(wParam));
            return 0;
        }
        if (menuVisible) {
            auto& nativeMenu = Core::Get().GetOverlay().GetNativeMenu();
            if (nativeMenu.HasActiveEditBox()) {
                nativeMenu.OnChar(static_cast<wchar_t>(wParam));
            }
            return 0; // always consume when menu is visible (modal)
        }
    }

    // WM_KEYDOWN: forward control keys to active UI, always consume when modal
    if (uMsg == WM_KEYDOWN && wParam != VK_F1 && wParam != VK_ESCAPE) {
        if (chatActive) {
            if (wParam == VK_BACK || wParam == VK_RETURN) {
                Core::Get().GetNativeHud().OnChatKeyDown(static_cast<int>(wParam));
            }
            return 0; // consume ALL keydowns when chat is active
        }
        if (menuVisible) {
            auto& nativeMenu = Core::Get().GetOverlay().GetNativeMenu();
            if (wParam == VK_BACK || wParam == VK_RETURN || wParam == VK_TAB) {
                nativeMenu.OnKeyDown(static_cast<int>(wParam));
            }
            return 0; // consume ALL keydowns when menu is visible (modal)
        }
    }

    // WM_KEYUP: consume when chat or menu is active to prevent unpaired key-up
    // events reaching OIS (which would desync its internal key state tracking)
    if (uMsg == WM_KEYUP) {
        if (chatActive || menuVisible) return 0;
    }

    // ES: Clic izquierdo: si el panel nativo está abierto se le reenvía (sin consumir); en el
    //     menú principal se comprueba si cae dentro del botón MULTIPLAYER (coords normalizadas).
    // EN: Left click: if the native panel is open it is forwarded to it (not consumed); on the
    //     main menu it checks whether it hits the MULTIPLAYER button (normalized coords).
    // Mouse click handling
    if (uMsg == WM_LBUTTONDOWN) {
        int mx = LOWORD(lParam);
        int my = HIWORD(lParam);

        auto& nativeMenu = Core::Get().GetOverlay().GetNativeMenu();

        if (nativeMenu.IsVisible()) {
            // Native panel is open — forward click to its handler
            nativeMenu.OnClick(mx, my);
        } else if (!Core::Get().IsGameLoaded() && IsMainMenuReady()) {
            // On main menu — check if click hit our MULTIPLAYER button
            RECT clientRect;
            if (GetClientRect(hWnd, &clientRect)) {
                float screenW = static_cast<float>(clientRect.right - clientRect.left);
                float screenH = static_cast<float>(clientRect.bottom - clientRect.top);

                if (screenW > 0 && screenH > 0) {
                    float nx = static_cast<float>(mx) / screenW;
                    float ny = static_cast<float>(my) / screenH;

                    if (nx >= MP_BTN_X && nx <= (MP_BTN_X + MP_BTN_W) &&
                        ny >= MP_BTN_Y && ny <= (MP_BTN_Y + MP_BTN_H)) {
                        spdlog::info("render_hooks: MULTIPLAYER button clicked ({}, {})", mx, my);
                        nativeMenu.Show();
                        return 0; // consume the click
                    }
                }
            }
        }
    }

    // ES: Mensaje no consumido: se entrega al WndProc original del juego.
    // EN: Message not consumed: hand it to the game's original WndProc.
    return CallWindowProcA(s_originalWndProc, hWnd, uMsg, wParam, lParam);
}

// ES: WndProc instalado con SetWindowLongPtrA. Envuelve WndProcInner en SEH: si nuestro código
//     falla, se loguea (máx. 10 veces) y el mensaje sigue al WndProc original del juego.
// EN: WndProc installed via SetWindowLongPtrA. Wraps WndProcInner in SEH: if our code crashes,
//     it is logged (max 10 times) and the message still goes to the game's original WndProc.
// SEH wrapper — a crash in our WndProc must not kill the game
static LRESULT CALLBACK HookWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    __try {
        return WndProcInner(hWnd, uMsg, wParam, lParam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_count = 0;
        if (++s_count <= 10) {
            char buf[128];
            sprintf_s(buf, "KMP: SEH CRASH in WndProc (msg=0x%X, wp=0x%llX)\n",
                      uMsg, (unsigned long long)wParam);
            OutputDebugStringA(buf);
        }
        return CallWindowProcA(s_originalWndProc, hWnd, uMsg, wParam, lParam);
    }
}

// ── Present Hook ──
// ES: Present en modo "paso": descubre el HWND, instala el hook de WndProc y conduce
//     OnGameTick. NO se renderiza ImGui (conflicto Ogre3D/DX11 que crashea).
//     s_lastFrameTime: instante del último OnGameTick, para calcular dt.
// EN: see below.
// Passthrough: HWND discovery + WndProc hook + OnGameTick fallback.
// NO ImGui rendering — Ogre3D/DX11 conflict causes crash.
static std::chrono::steady_clock::time_point s_lastFrameTime{};
static bool s_hasLastFrameTime = false;

// ── SEH wrappers for per-frame calls ──
// ES: Envoltorios SEH de las actualizaciones por frame (Overlay y NativeHud): un fallo en
//     uno no mata el juego ni impide ejecutar el otro. Log limitado a 5 veces.
// EN: SEH wrappers for the per-frame updates (Overlay and NativeHud): a crash in one does
//     not kill the game nor stop the other from running. Logging capped at 5 times.
static void SEH_OverlayUpdate() {
    __try {
        Core::Get().GetOverlay().Update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_count = 0;
        if (++s_count <= 5) OutputDebugStringA("KMP: SEH CRASH in Overlay::Update()\n");
    }
}

// All rendering via native MyGUI NativeHud — no GDI overlay.

static void SEH_NativeHudUpdate() {
    __try {
        Core::Get().GetNativeHud().Update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_count = 0;
        if (++s_count <= 5) OutputDebugStringA("KMP: SEH CRASH in NativeHud::Update()\n");
    }
}

// No GDI overlay — NativeHud handles all display.

// ES: Instante del Present anterior, para medir el hueco entre frames (detección de cargas).
// EN: Previous Present timestamp, to measure the gap between frames (load detection).
// Track frame timing for loading gap detection
static std::chrono::steady_clock::time_point s_prevPresentTime{};
static bool s_hasPrevPresentTime = false;

// ES: Detección de fin de carga por frames fluidos: tras entrar en Loading, si hay N segundos
//     seguidos sin huecos >2 s, se considera que la carga terminó. (El comentario inglés dice
//     5 s pero el código de HookPresent usa 8 s.) s_createsAtLoadingStart guarda el contador de
//     creaciones de personajes de entity_hooks al empezar la carga (el propio código no lo lee
//     en este fichero; probablemente vestigio).
// EN: Smooth-frame end-of-load detection: after entering Loading, N consecutive seconds with no
//     >2 s gaps mean loading has finished. (The English comment says 5 s but HookPresent's code
//     uses 8 s.) s_createsAtLoadingStart stores entity_hooks' character-create counter at load
//     start (nothing in this file reads it; probably a leftover).
// Smooth-frame game-load detection: after Loading starts, if we get
// 5 seconds of smooth frames (no >2s gaps), the game has finished loading.
static std::chrono::steady_clock::time_point s_loadingSmoothStart{};
static bool s_loadingSmoothStarted = false;
// Snapshot of entity_hooks::GetTotalCreates() when Loading phase started.
// We check (current - snapshot > 5) to detect real save loads vs character
// creation screens, and to avoid false positives on second loads.
static int s_createsAtLoadingStart = 0;

// ES: Detour de IDXGISwapChain::Present. Corre en el hilo de render una vez por frame, ANTES de
//     llamar al Present original (nada se hace después). Fases:
//     1) guarda la hora del primer Present; 2) transiciones de ClientPhase según el ritmo de
//     frames (Startup→MainMenu a los 5 s; hueco >2 s = carga de partida; 8 s fluidos en Loading
//     → PollForGameLoad); 3) log periódico; 4) una sola vez, HWND + hook de WndProc;
//     5) Overlay/NativeHud Update; 6) si hay conexión, OnGameTick(dt) con dt en (0, 0.5) s;
//     7) trampolín al Present original (E_FAIL si no hay trampolín).
// EN: Detour for IDXGISwapChain::Present. Runs on the render thread once per frame, BEFORE
//     calling the original Present (nothing is done afterwards). Phases:
//     1) record first Present time; 2) ClientPhase transitions from frame pacing
//     (Startup→MainMenu after 5 s; >2 s gap = save load; 8 smooth seconds in Loading →
//     PollForGameLoad); 3) periodic log; 4) once, HWND + WndProc hook;
//     5) Overlay/NativeHud Update; 6) if connected, OnGameTick(dt) with dt in (0, 0.5) s;
//     7) trampoline to the original Present (E_FAIL if there is no trampoline).
static HRESULT __stdcall HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    // Record first Present time for startup guard (logo/splash delay)
    if (!s_firstPresentRecorded) {
        s_firstPresentTime = std::chrono::steady_clock::now();
        s_firstPresentRecorded = true;
        OutputDebugStringA("KMP: First Present — recording startup time\n");
    }

    static int s_presentCount = 0;
    s_presentCount++;
    if (s_presentCount <= 3) {
        char buf[128];
        sprintf_s(buf, "KMP: HookPresent #%d\n", s_presentCount);
        OutputDebugStringA(buf);
    }

    // ── Phase transitions driven by Present timing ──
    auto& core = Core::Get();
    auto now = std::chrono::steady_clock::now();
    {
        ClientPhase phase = core.GetClientPhase();

        // Startup → MainMenu: after 5 seconds of Present firing (splash is done)
        if (phase == ClientPhase::Startup) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - s_firstPresentTime);
            if (elapsed.count() >= 5) {
                core.TransitionTo(ClientPhase::MainMenu);
            }
        }

        // ES: Detección de carga por hueco entre Present (>2 s). En el menú Present llega cada
        //     4-16 ms; al cargar partida el juego se bloquea 10-60 s. Según la fase: MainMenu →
        //     carga real; Loading → sigue cargando (reinicia temporizador); GameReady con >10 s
        //     → carga de otra partida desde el juego; Connected sin partida → conectó desde el
        //     menú y ahora carga; resto → cambio de zona, sin cambiar fase.
        // EN: Load detection via gap between Presents (>2 s). On the menu Present fires every
        //     4-16 ms; loading a save blocks the game for 10-60 s. Per phase: MainMenu → real
        //     load; Loading → still loading (reset timer); GameReady with >10 s → another save
        //     loaded in-game; Connected without a game → connected from the menu and now
        //     loading; otherwise → zone change, no phase change.
        // MainMenu → Loading: detect a long gap between Present calls (>2s).
        // During normal menu rendering, Present fires every ~4-16ms.
        // When the user clicks New Game / Continue / Load, the game blocks
        // for 10-60 seconds while loading. The first Present AFTER that gap
        // is our signal that loading just finished (or is finishing).
        //
        // IMPORTANT: Only transition from MainMenu. While Connected/GameReady,
        // zone loading also causes >2s gaps — but that's handled by entity_hooks
        // burst guard, NOT by re-entering the Loading phase.
        if (s_hasPrevPresentTime) {
            auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_prevPresentTime);
            if (gap.count() > 2000) {
                if (phase == ClientPhase::MainMenu) {
                    // Only detect loading gap from MainMenu — NOT from Startup.
                    // The initial engine loading (shaders, textures, splash screen)
                    // creates >2s gaps during Startup, which is NOT a save game load.
                    // Startup → MainMenu transition happens after 5s of smooth Present calls.
                    spdlog::info("render_hooks: Loading gap detected ({} ms between frames, phase={})",
                                 gap.count(), ClientPhaseToString(phase));
                    // Reset smooth-frame tracking for new load (prevents stale state
                    // from first load causing premature OnGameLoaded on second load)
                    s_loadingSmoothStarted = false;
                    s_createsAtLoadingStart = entity_hooks::GetTotalCreates();
                    core.OnLoadingGapDetected();
                } else if (phase == ClientPhase::Loading) {
                    // Still loading — another gap means assets are still streaming.
                    // Reset smooth-frame timer.
                    s_loadingSmoothStarted = false;
                    spdlog::debug("render_hooks: Loading gap ({} ms) — reset smooth timer", gap.count());
                } else if (phase == ClientPhase::GameReady && gap.count() > 10000) {
                    // Very long gap (>10s) during GameReady = user loaded a new save
                    // from the in-game Load menu. Zone transitions are 2-5s max.
                    // Reset state and re-enter Loading so OnGameLoaded fires again.
                    spdlog::info("render_hooks: In-game save load detected ({} ms gap during GameReady)", gap.count());
                    s_loadingSmoothStarted = false;
                    s_createsAtLoadingStart = entity_hooks::GetTotalCreates();
                    core.OnLoadingGapDetected();
                } else if (phase == ClientPhase::Connected && !core.IsGameLoaded()) {
                    // EN: CRITICAL FIX (gameLoaded stuck false): the player connected FROM THE
                    //     MENU (phase → Connected) and is NOW loading a save. This >2 s gap is
                    //     the save load, NOT a zone load. It used to fall into the "no phase
                    //     change" branch, so the load was never detected and m_gameLoaded stayed
                    //     false forever. Now it is treated as a real load: OnLoadingGapDetected()
                    //     accepts Connected and moves to Loading, re-enabling the smooth-frame
                    //     poll and PollForGameLoad timeouts. On completion OnGameLoaded() sees
                    //     m_connected==true and resumes sync normally.
                    // ES:
                    // ⚠ FIX CRÍTICO (gameLoaded=false eterno): el jugador conectó DESDE EL
                    // MENÚ (fase → Connected) y AHORA está cargando su save. Este gap >2s es
                    // la carga de la partida, NO un zone-load. Antes caía en la rama
                    // "no phase change" y la carga nunca se detectaba → m_gameLoaded eterno
                    // en false. Lo tratamos como carga real: OnLoadingGapDetected() ahora
                    // acepta Connected y transiciona a Loading, reactivando el smooth-frame
                    // poll y los timeouts de PollForGameLoad. Al completar, OnGameLoaded()
                    // ve m_connected==true y reanuda el sync con normalidad.
                    spdlog::info("render_hooks: Save load detected ({} ms gap during Connected, "
                                 "connected-then-load flow) — triggering load detection", gap.count());
                    s_loadingSmoothStarted = false;
                    s_createsAtLoadingStart = entity_hooks::GetTotalCreates();
                    core.OnLoadingGapDetected();
                } else if (phase == ClientPhase::Connected || phase == ClientPhase::GameReady) {
                    spdlog::info("render_hooks: Zone load gap detected ({} ms) during {} — no phase change",
                                 gap.count(), ClientPhaseToString(phase));
                }
            }
        }

        // ES: En Loading, tras 8 s de frames fluidos se lanza UNA vez PollForGameLoad, que
        //     comprueba con CharacterIterator que existen personajes (no llama a OnGameLoaded
        //     a ciegas para evitar falsos positivos en menú o creación de personaje).
        // EN: In Loading, after 8 s of smooth frames PollForGameLoad is fired ONCE; it checks
        //     via CharacterIterator that characters exist (OnGameLoaded is not called blindly,
        //     to avoid false positives on the menu or character creation).
        // Smooth-frame game-load detection: while in Loading phase, track how long
        // we've had smooth frames (no >2s gaps). After 8 seconds of smooth rendering,
        // trigger PollForGameLoad which checks CharacterIterator for actual characters.
        // Don't call OnGameLoaded directly — the poll validates that characters exist.
        static bool s_smoothTriggeredPoll = false;
        if (phase == ClientPhase::Loading) {
            if (!s_loadingSmoothStarted) {
                s_loadingSmoothStart = now;
                s_loadingSmoothStarted = true;
            }
            auto smoothDuration = std::chrono::duration_cast<std::chrono::seconds>(now - s_loadingSmoothStart);
            if (smoothDuration.count() >= 8 && !core.IsGameLoaded()) {
                // Don't fire OnGameLoaded blindly — let PollForGameLoad verify
                // that characters actually exist via CharacterIterator.
                // This prevents false positives on the main menu or character creation.
                if (!s_smoothTriggeredPoll) {
                    s_smoothTriggeredPoll = true;
                    spdlog::info("render_hooks: 8s of smooth frames during Loading — triggering PollForGameLoad");
                    core.PollForGameLoad();
                }
            }
        } else {
            s_smoothTriggeredPoll = false;
        }
    }
    s_prevPresentTime = now;
    s_hasPrevPresentTime = true;

    // ES: Diagnóstico periódico cada 300 frames.
    // EN: Periodic diagnostic every 300 frames.
    // ── Periodic diagnostic ──
    if (s_presentCount % 300 == 1) {
        spdlog::info("render_hooks: frame={} phase={} gameLoaded={} connected={}",
                     s_presentCount, ClientPhaseToString(core.GetClientPhase()),
                     core.IsGameLoaded(), core.IsConnected());
    }

    // ES: Una sola vez: obtener el HWND del swap chain y sustituir el WndProc de la ventana.
    //     Si SetWindowLongPtrA falla se reintenta en el siguiente frame (s_hwnd = nullptr).
    // EN: One time only: get the HWND from the swap chain and replace the window's WndProc.
    //     If SetWindowLongPtrA fails it is retried next frame (s_hwnd = nullptr).
    // One-time: grab HWND from the swap chain for WndProc hook
    if (!s_hwnd) {
        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(swapChain->GetDesc(&desc))) {
            s_hwnd = desc.OutputWindow;
            OutputDebugStringA("KMP: Got HWND from swap chain\n");

            // Install WndProc hook for input (F1, mouse clicks, etc.)
            SetLastError(0);
            s_originalWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
            if (!s_originalWndProc && GetLastError() != 0) {
                // SetWindowLongPtrA failed — don't keep our WndProc installed
                // because CallWindowProcA(nullptr) would crash
                spdlog::error("render_hooks: SetWindowLongPtrA FAILED (err={})", GetLastError());
                OutputDebugStringA("KMP: WndProc hook FAILED\n");
                s_hwnd = nullptr; // Force retry next frame
            } else {
                OutputDebugStringA("KMP: WndProc hook installed\n");
            }

            // GDI overlay removed — native MyGUI HUD (NativeHud) handles all rendering
        }
    }

    // ES: Actualización por frame del overlay (auto-conexión, estado de conexión, detección de
    //     desconexión) y del HUD nativo, cada una protegida con SEH.
    // EN: Per-frame update of the overlay (auto-connect, connection state, disconnect detection)
    //     and of the native HUD, each one SEH-protected.
    // ── Per-frame overlay update (auto-connect, connection state, disconnect detect) ──
    // Each call is SEH-protected so a crash in one doesn't kill the game.
    {
        static bool s_overlayUpdateStarted = false;
        if (!s_overlayUpdateStarted) {
            OutputDebugStringA("KMP: HookPresent — starting per-frame Update() calls\n");
            s_overlayUpdateStarted = true;
        }
        SEH_OverlayUpdate();
        // NativeHud handles all display
        SEH_NativeHudUpdate();
    }

    // ES: Motor de OnGameTick: solo con conexión activa. Se conduce siempre desde Present porque
    //     TimeUpdate (RVA 0x214B50, relativo a la base de kenshi_x64.exe) nunca se dispara en
    //     la build de Steam. dt fuera de (0, 0.5) s (primer frame, pausas largas) no se procesa.
    //     OnGameTick tiene un guard de 4 ms contra doble ejecución.
    // EN: OnGameTick driver: only while connected. Always driven from Present because
    //     TimeUpdate (RVA 0x214B50, relative to the kenshi_x64.exe base) never fires on the
    //     Steam build. dt outside (0, 0.5) s (first frame, long stalls) is skipped.
    //     OnGameTick has a 4 ms guard against double execution.
    // ── OnGameTick driver ──
    // Always drive OnGameTick from Present hook. TimeUpdate at RVA 0x214B50
    // was found to never fire on Steam builds (the game doesn't call that
    // function). The 4ms dedup guard inside OnGameTick prevents double-processing
    // if TimeUpdate ever starts working.
    if (core.IsConnected()) {
        static int s_connectedFrames = 0;
        s_connectedFrames++;

        // Log first 20 connected frames, then every 100th
        if (s_connectedFrames <= 20 || s_connectedFrames % 100 == 0) {
            spdlog::debug("render_hooks: Connected frame #{} (present #{})",
                          s_connectedFrames, s_presentCount);
        }

        auto now = std::chrono::steady_clock::now();
        if (s_hasLastFrameTime) {
            float dt = std::chrono::duration<float>(now - s_lastFrameTime).count();
            if (dt > 0.0f && dt < 0.5f) {
                SEH_OnGameTick(dt);
            }
        } else {
            OutputDebugStringA("KMP: First OnGameTick from Present hook\n");
            spdlog::info("render_hooks: First OnGameTick from Present hook (connected frame #{})",
                         s_connectedFrames);
        }
        s_lastFrameTime = now;
        s_hasLastFrameTime = true;
    }

    // ES: Llamada al Present original vía trampolín de MinHook.
    // EN: Call the original Present through the MinHook trampoline.
    if (s_originalPresent) {
        return s_originalPresent(swapChain, syncInterval, flags);
    }
    return E_FAIL;
}

// ── Get DXGI VTable ──
// Create a temporary D3D11 device + swap chain to read the vtable
// ES: Crea una ventana oculta 100x100 y un device + swap chain D3D11 temporales solo para leer
//     la vtable de IDXGISwapChain (compartida con el swap chain real del juego). Libera todo
//     y devuelve la vtable en `vtable`; false si D3D11CreateDeviceAndSwapChain falla.
// EN: Creates a hidden 100x100 window and a temporary D3D11 device + swap chain just to read
//     the IDXGISwapChain vtable (shared with the game's real swap chain). Releases everything
//     and returns the vtable in `vtable`; false if D3D11CreateDeviceAndSwapChain fails.
static bool GetDXGIVTable(void**& vtable) {
    WNDCLASSEXA wc = {sizeof(WNDCLASSEXA), CS_CLASSDC, DefWindowProcA, 0, 0,
                     GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                     "KMP_TEMP", nullptr};
    RegisterClassExA(&wc);
    HWND tempHwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                                 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = tempHwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* tempDevice = nullptr;
    IDXGISwapChain* tempSwapChain = nullptr;
    ID3D11DeviceContext* tempContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &scd, &tempSwapChain, &tempDevice, &featureLevel, &tempContext);

    if (FAILED(hr)) {
        DestroyWindow(tempHwnd);
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return false;
    }

    vtable = *reinterpret_cast<void***>(tempSwapChain);

    tempSwapChain->Release();
    tempContext->Release();
    tempDevice->Release();
    DestroyWindow(tempHwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    return true;
}

// ── Install/Uninstall ──

// ES: Resuelve la vtable DXGI y desvía Present (índice 8) con HookManager::InstallAt, guardando
//     el trampolín en s_originalPresent. El WndProc se instala más tarde, en el primer Present.
// EN: Resolves the DXGI vtable and detours Present (index 8) with HookManager::InstallAt,
//     storing the trampoline in s_originalPresent. The WndProc is installed later, on the first Present.
bool Install() {
    void** vtable = nullptr;
    if (!GetDXGIVTable(vtable)) {
        spdlog::error("render_hooks: Failed to get DXGI vtable");
        return false;
    }

    auto& hookMgr = HookManager::Get();

    // Present is vtable index 8
    if (!hookMgr.InstallAt("DXGI_Present",
                           reinterpret_cast<uintptr_t>(vtable[8]),
                           &HookPresent, &s_originalPresent)) {
        spdlog::error("render_hooks: Failed to hook Present");
        return false;
    }

    spdlog::info("render_hooks: Installed successfully (passthrough + WndProc)");
    return true;
}

// ES: Restaura el WndProc original de la ventana y elimina el hook de Present.
// EN: Restores the window's original WndProc and removes the Present hook.
void Uninstall() {
    if (s_originalWndProc && s_hwnd) {
        SetWindowLongPtrA(s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_originalWndProc));
    }

    HookManager::Get().Remove("DXGI_Present");
}

// ES: Publica WM_KMP_SPAWN en la cola de la ventana. Hoy WndProcInner lo ignora (ver arriba).
// EN: Posts WM_KMP_SPAWN to the window queue. WndProcInner currently ignores it (see above).
void PostSpawnTrigger() {
    if (s_hwnd) {
        PostMessageA(s_hwnd, WM_KMP_SPAWN, 0, 0);
    }
}

} // namespace kmp::render_hooks
