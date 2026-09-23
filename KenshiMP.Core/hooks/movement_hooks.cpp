// ES: Implementación de los hooks de movimiento. Contiene los detours Hook_SetPosition
//     (enviaría C2S_PositionUpdate no fiable con posición, rotación comprimida, animación y
//     velocidad del personaje local, limitado al tick de red) y Hook_MoveTo (bloquearía las
//     órdenes de movimiento de la IA en personajes remotos y enviaría C2S_MoveCommand para
//     los locales). Ambos existen pero Install() NO los engancha (ver motivos en Install()).
// EN: Implementation of the movement hooks. Contains the Hook_SetPosition detour (would
//     send an unreliable C2S_PositionUpdate with the local character's position, compressed
//     rotation, animation and speed, throttled to the network tick) and Hook_MoveTo (would
//     block AI move orders on remote characters and send C2S_MoveCommand for local ones).
//     Both exist but Install() does NOT hook them (see reasons in Install()).
#include "movement_hooks.h"
#include "ai_hooks.h"
#include "../core.h"
#include "../game/game_types.h"
#include "kmp/hook_manager.h"
#include "kmp/protocol.h"
#include "kmp/constants.h"
#include "kmp/safe_hook.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace kmp::movement_hooks {

// ES: Firmas: SetPosition(personaje, x, y, z) y MoveTo(personaje, x, y, z, tipo de movimiento).
//     moveType es el 5º parámetro y va en la pila (x64 solo pasa 4 por registro).
// EN: Signatures: SetPosition(character, x, y, z) and MoveTo(character, x, y, z, move type).
//     moveType is the 5th parameter and goes on the stack (x64 only passes 4 in registers).
// ── Function Types ──
using SetPositionFn = void(__fastcall*)(void* character, float x, float y, float z);
using MoveToFn = void(__fastcall*)(void* character, float x, float y, float z, int moveType);

// ES: Trampolines a los originales (nullptr mientras no se instalen).
// EN: Trampolines to the originals (nullptr while not installed).
static SetPositionFn s_origSetPosition = nullptr;
static MoveToFn      s_origMoveTo      = nullptr;

// ES: Salud de cada hook para SafeCall (se autodesactiva si el trampolín crashea).
// EN: Per-hook health for SafeCall (self-disables if the trampoline crashes).
// ── Hook Health ──
static HookHealth s_setPosHealth{"CharacterSetPosition"};
static HookHealth s_moveToHealth{"CharacterMoveTo"};

// ES: Contadores de diagnóstico.
// ── Diagnostic Counters ──
static std::atomic<int> s_setPosCount{0};
static std::atomic<int> s_moveToCount{0};
static std::atomic<int> s_posUpdatesSent{0};

// ES: Marca de tiempo del último envío de posición (limitador de frecuencia).
// Position send throttle
static auto s_lastPositionSend = std::chrono::steady_clock::now();

// ── Hooks ──

// ES: Detour de SetPosition (NO instalado). Llama al original con SEH y después, si hay
//     conexión y el personaje es del jugador local, limita a KMP_TICK_INTERVAL_MS y a un
//     cambio mínimo de posición, lee rotación/velocidad/animación (con respaldo calculado)
//     y envía C2S_PositionUpdate no fiable. Correría en el hilo de física/lógica del juego.
// EN: SetPosition detour (NOT installed). Calls the original under SEH and then, when
//     connected and the character belongs to the local player, throttles to
//     KMP_TICK_INTERVAL_MS and a minimum position change, reads rotation/speed/animation
//     (with a computed fallback) and sends an unreliable C2S_PositionUpdate. Would run on
//     the game's physics/logic thread.
static void __fastcall Hook_SetPosition(void* character, float x, float y, float z) {
    int callNum = s_setPosCount.fetch_add(1) + 1;

    // ES: GUARDA DE POSICIÓN DE PERSONAJES REMOTOS. Idea: que la física/IA del juego no pise
    //     la posición de los remotos (viene de la interpolación de red). OJO: el bloque de abajo
    //     está vacío, no bloquea ni loguea nada; la llamada siempre sigue adelante.
    // ═══ REMOTE CHARACTER POSITION GUARD ═══
    // Don't let the game's physics/AI overwrite positions of remote characters.
    // Their positions come from network interpolation only.
    // Allow the call ONLY during our own WritePosition (which uses the raw function
    // pointer, bypassing this hook). This catches engine-initiated position resets.
    if (ai_hooks::IsRemoteControlled(character)) {
        // ES: Se permite la llamada: la física la necesita en el spawn inicial y la interpolación
        //     de red la sobrescribe en el siguiente frame.
        // Still allow the call — but log it for diagnostics.
        // We can't fully block SetPosition because the physics engine needs it
        // during initial spawn. The network interpolation will overwrite next frame.
    }

    // ES: Loguear las 50 primeras llamadas y luego 1 de cada 1000.
    // Log first 50 calls and then every 1000th call for diagnostics
    if (callNum <= 50 || callNum % 1000 == 0) {
        spdlog::debug("movement_hooks: SetPosition #{} ptr=0x{:X} pos=({:.1f},{:.1f},{:.1f})",
                      callNum, reinterpret_cast<uintptr_t>(character), x, y, z);
    }

    // ES: Llamada al trampolín protegida con SEH; si falla, el hook queda desactivado.
    // SEH-protected trampoline call
    if (!SafeCall_Void_PtrFFF(reinterpret_cast<void*>(s_origSetPosition),
                               character, x, y, z, &s_setPosHealth)) {
        if (s_setPosHealth.trampolineFailed.load()) {
            spdlog::error("movement_hooks: SetPosition trampoline CRASHED! Hook disabled. "
                          "Total calls before crash: {}", callNum);
        }
        return;
    }

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    // ES: Solo se envían personajes con netId cuyo dueño sea el jugador local.
    // Check if this is a local player character
    auto& registry = core.GetEntityRegistry();
    EntityID netId = registry.GetNetId(character);
    if (netId == INVALID_ENTITY) return;

    auto info = registry.GetInfo(netId);
    if (!info.has_value() || info->ownerPlayerId != core.GetLocalPlayerId()) return;

    // ES: Limitar los envíos a la frecuencia del tick de red.
    // Throttle position updates to tick rate
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastPositionSend);
    if (elapsed.count() < KMP_TICK_INTERVAL_MS) return;

    // ES: Enviar solo si la posición cambió más que el umbral KMP_POS_CHANGE_THRESHOLD.
    // Check if position changed enough to send
    Vec3 newPos(x, y, z);
    if (newPos.DistanceTo(info->lastPosition) < KMP_POS_CHANGE_THRESHOLD) return;

    s_lastPositionSend = now;

    // ES: Leer rotación y datos de movimiento del objeto personaje; la rotación se comprime a 32 bits.
    // Read rotation and movement data from the character object
    game::CharacterAccessor accessor(character);
    Quat rotation = accessor.GetRotation();
    uint32_t compQuat = rotation.Compress();

    // ES: Velocidad calculada a partir del desplazamiento (respaldo si el offset de velocidad es -1).
    // Compute moveSpeed from position delta (reliable fallback when offset = -1)
    float dist = newPos.DistanceTo(info->lastPosition);
    float elapsedSec = elapsed.count() / 1000.f;
    float computedSpeed = (elapsedSec > 0.001f) ? dist / elapsedSec : 0.f;

    // ES: Primero se intenta leer la velocidad de memoria; si no, se usa la calculada.
    // Try memory read first; fall back to computed speed
    float moveSpeed = accessor.GetMoveSpeed();
    if (moveSpeed <= 0.f && computedSpeed > 0.f) {
        moveSpeed = computedSpeed;
    }

    // ES: Deducir el estado de animación a partir de la velocidad si no hay offset (1 andar, 2 correr).
    // Derive animation state from speed when offset is unavailable
    uint8_t animState = accessor.GetAnimState();
    if (animState == 0 && moveSpeed > 0.5f) {
        animState = (moveSpeed > 5.0f) ? 2 : 1; // 1=walking, 2=running
    }

    // ES: Cuantizar la velocidad (0..15 m/s) a un byte (0..255).
    // Map move speed (0..15 m/s) to uint8 (0..255)
    uint8_t moveSpeedU8 = static_cast<uint8_t>(
        std::min(255.f, moveSpeed / 15.f * 255.f));

    // ES: Banderas del paquete: bit 0 = corriendo (> 3 m/s).
    // Determine flags
    uint16_t flags = 0;
    if (moveSpeed > 3.0f) flags |= 0x01; // running

    // ES: Construir y enviar C2S_PositionUpdate (canal no fiable, un solo personaje).
    // Send position update
    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_PositionUpdate);
    writer.WriteU8(1); // one character
    writer.WriteU32(netId);
    writer.WriteF32(x);
    writer.WriteF32(y);
    writer.WriteF32(z);
    writer.WriteU32(compQuat);
    writer.WriteU8(animState);
    writer.WriteU8(moveSpeedU8);
    writer.WriteU16(flags);

    core.GetClient().SendUnreliable(writer.Data(), writer.Size());

    int sent = s_posUpdatesSent.fetch_add(1) + 1;
    if (sent <= 20 || sent % 100 == 0) {
        spdlog::debug("movement_hooks: Sent position update #{} netId={} pos=({:.1f},{:.1f},{:.1f}) speed={:.1f}",
                      sent, netId, x, y, z, moveSpeed);
    }

    // ES: Actualizar la posición/rotación conocidas en el registro local de entidades.
    // Update local tracking
    registry.UpdatePosition(netId, newPos);
    registry.UpdateRotation(netId, rotation);
}

// ES: Detour de MoveTo (NO instalado). Para personajes remotos NO llama al original
//     (suprime la decisión de movimiento de la IA). Para el resto llama al original con
//     SEH y, si el personaje es del jugador local, envía C2S_MoveCommand fiable.
// EN: MoveTo detour (NOT installed). For remote characters it does NOT call the original
//     (suppresses the AI's move decision). Otherwise it calls the original under SEH and,
//     if the character belongs to the local player, sends a reliable C2S_MoveCommand.
static void __fastcall Hook_MoveTo(void* character, float x, float y, float z, int moveType) {
    int callNum = s_moveToCount.fetch_add(1) + 1;

    // ES: BLOQUEO DE PERSONAJES REMOTOS: su movimiento lo marcan las posiciones de red del
    //     cliente dueño; sin este bloqueo la IA (que se mantiene viva) daría sus propias
    //     órdenes y pelearía con la interpolación.
    // ═══ REMOTE CHARACTER BLOCK ═══
    // Block AI-issued move commands for remote characters. Their movement
    // is driven by network position updates from the owning client.
    // Without this block, the AI (which we kept alive in Phase 1) would
    // issue its own move commands that fight with interpolated positions.
    if (ai_hooks::IsRemoteControlled(character)) {
        static int s_blockCount = 0;
        if (++s_blockCount <= 10 || s_blockCount % 500 == 0) {
            spdlog::debug("movement_hooks: BLOCKED MoveTo for remote char 0x{:X} "
                          "(AI decision overridden, total blocked: {})",
                          (uintptr_t)character, s_blockCount);
        }
        return; // Don't call original — suppress the AI's movement decision
    }

    if (callNum <= 50 || callNum % 1000 == 0) {
        spdlog::debug("movement_hooks: MoveTo #{} ptr=0x{:X} target=({:.1f},{:.1f},{:.1f}) type={}",
                      callNum, reinterpret_cast<uintptr_t>(character), x, y, z, moveType);
    }

    // ES: Llamada al trampolín protegida con SEH.
    // SEH-protected trampoline call
    if (!SafeCall_Void_PtrFFFI(reinterpret_cast<void*>(s_origMoveTo),
                                character, x, y, z, moveType, &s_moveToHealth)) {
        if (s_moveToHealth.trampolineFailed.load()) {
            spdlog::error("movement_hooks: MoveTo trampoline CRASHED! Hook disabled.");
        }
        return;
    }

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    EntityID netId = core.GetEntityRegistry().GetNetId(character);
    if (netId == INVALID_ENTITY) return;

    auto info = core.GetEntityRegistry().GetInfo(netId);
    if (!info.has_value() || info->ownerPlayerId != core.GetLocalPlayerId()) return;

    // ES: Enviar la orden de movimiento C2S_MoveCommand (canal fiable).
    // Send move command
    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_MoveCommand);
    writer.WriteU32(netId);
    writer.WriteF32(x);
    writer.WriteF32(y);
    writer.WriteF32(z);
    writer.WriteU8(static_cast<uint8_t>(moveType));

    core.GetClient().SendReliable(writer.Data(), writer.Size());
}

// ── Install/Uninstall ──

// ES: No engancha ninguno de los dos hooks (motivos abajo); solo deja constancia en el log.
// EN: Hooks neither function (reasons below); it only leaves a log record.
bool Install() {
    auto& core = Core::Get();
    auto& hookMgr = HookManager::Get();
    auto& funcs = core.GetGameFunctions();

    // ES: NO hookear CharacterSetPosition: empieza por `mov rax, rsp` y se corrompe al pasar
    //     por el trampolín de MinHook (rax captura un puntero de pila equivocado). El rodeo
    //     HookBypass (desactivar-llamar-reactivar) es demasiado caro para una función que se
    //     llama cientos de veces por frame. Las posiciones se sondean desde Core::OnGameTick.
    // NOTE: Do NOT hook CharacterSetPosition — it starts with `mov rax, rsp`
    // which corrupts when called through MinHook's trampoline (rax captures the
    // wrong stack pointer). HookBypass (disable-call-reenable) is too expensive
    // for a function called hundreds of times per frame.
    // Position updates are sent from Core::OnGameTick via polling instead.
    if (funcs.CharacterSetPosition) {
        spdlog::info("movement_hooks: SetPosition at 0x{:X} — NOT hooked (mov rax,rsp trampoline issue). "
                     "Positions polled from OnGameTick instead.",
                     reinterpret_cast<uintptr_t>(funcs.CharacterSetPosition));
    }

    // ES: NO hookear CharacterMoveTo: empieza por `mov rax, rsp` Y tiene un 5º parámetro en
    //     pila (moveType) que el detour naked MovRaxRsp no reenvía bien, así que el trampolín
    //     crashea en CADA llamada y rompe el clic-para-moverse. El movimiento se sincroniza por
    //     sondeo y la supresión de IA de remotos va en ai_hooks.
    // NOTE: Do NOT hook CharacterMoveTo — it starts with `mov rax, rsp` AND has
    // a 5th stack parameter (moveType). The MovRaxRsp naked detour mechanism cannot
    // correctly forward stack parameters, causing the trampoline to crash on EVERY call.
    // This prevents all character movement (click-to-move).
    // Movement sync works via position polling from OnGameTick.
    // Remote character AI suppression is handled by ai_hooks.
    if (funcs.CharacterMoveTo) {
        spdlog::info("movement_hooks: CharacterMoveTo at 0x{:X} — NOT hooked (mov rax,rsp + stack params = crash). "
                     "Movement sync via position polling.",
                     reinterpret_cast<uintptr_t>(funcs.CharacterMoveTo));
    }

    spdlog::info("movement_hooks: Installed (setPos={}, moveTo={})",
                 funcs.CharacterSetPosition != nullptr, funcs.CharacterMoveTo != nullptr);
    return true;
}

// ES: Quita ambos hooks por nombre.
// EN: Removes both hooks by name.
void Uninstall() {
    HookManager::Get().Remove("CharacterSetPosition");
    HookManager::Get().Remove("CharacterMoveTo");
}

} // namespace kmp::movement_hooks
