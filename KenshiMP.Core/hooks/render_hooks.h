// ES: Hooks de render: intercepta IDXGISwapChain::Present (D3D11) y sustituye el WndProc de la
//     ventana de Kenshi. Present se usa como "latido" por frame: detecta fases del cliente
//     (arranque, menú, carga), actualiza overlay/HUD nativo y conduce Core::OnGameTick.
//     El WndProc captura teclas y clics propios del mod (F1, Tab, Enter, Esc, botón MULTIPLAYER).
// EN: Render hooks: intercepts IDXGISwapChain::Present (D3D11) and replaces the Kenshi window's
//     WndProc. Present is used as a per-frame "heartbeat": it detects client phases (startup,
//     menu, loading), updates the overlay/native HUD and drives Core::OnGameTick.
//     The WndProc captures the mod's own keys and clicks (F1, Tab, Enter, Esc, MULTIPLAYER button).
#pragma once

// ES: Espacio de nombres de los hooks de render/entrada de ventana.
// EN: Namespace for the render/window-input hooks.
namespace kmp::render_hooks {

// ES: Instala el hook de Present (índice 8 de la vtable de IDXGISwapChain). Devuelve false si falla.
// EN: Installs the Present hook (index 8 of the IDXGISwapChain vtable). Returns false on failure.
bool Install();
// ES: Restaura el WndProc original y quita el hook de Present.
// EN: Restores the original WndProc and removes the Present hook.
void Uninstall();

// ES: Envía un mensaje propio (WM_KMP_SPAWN) a la ventana para procesar la cola de spawn desde
//     el contexto del WndProc (el bombeo de mensajes corre ENTRE frames, no dentro de Present).
//     OJO: en el .cpp ese mensaje ya se ignora (mecanismo obsoleto; el spawn lo hace el replay
//     in-place de entity_hooks), así que hoy esta llamada no tiene efecto.
// EN: Posts a custom message (WM_KMP_SPAWN) to the window to process the spawn queue from the
//     WndProc context (the message pump runs BETWEEN frames, not during Present).
//     NOTE: the .cpp now ignores that message (deprecated mechanism; spawning is done by the
//     in-place replay in entity_hooks), so this call currently has no effect.
// EN (original): Post a custom message to trigger spawn queue processing from WndProc context.
// The message pump runs BETWEEN frames (not during DX11 Present), so the factory works.
void PostSpawnTrigger();

} // namespace kmp::render_hooks
