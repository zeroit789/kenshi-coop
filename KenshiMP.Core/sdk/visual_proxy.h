// ES: Proxies visuales de jugadores remotos: seguimiento de estado e interpolación de
//     posición/rotación independiente de las entidades del juego. Hoy solo gestiona
//     estado (fase 1); el dibujado con Ogre está pendiente (fase 2).
// EN: Visual proxies for remote players: state tracking and position/rotation
//     interpolation independent of the game's entities. Today it only manages state
//     (phase 1); Ogre rendering is pending (phase 2).
#pragma once
#include "kmp/types.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace kmp::sdk {

// ES: PROXY VISUAL: representa a un jugador remoto como entidad rastreada con
//     interpolación de posición.
//     Fase 1 (actual): solo estado + interpolación, independiente del sistema de
//     entidades del juego (sin hooks, sin entidades del juego, sin crashes).
//     Fase 2 (futuro): dibujar con SceneNodes de Ogre, cuando se descubra el SceneManager
//     en tiempo de ejecución escaneando vtables, colgando mallas de Ogre.
//     Idea clave: toda la gestión de estado funciona sin cabeceras de Ogre; Ogre solo
//     hace falta para el paso final de "hacerlo visible".
// EN:
// ═══════════════════════════════════════════════════════════════════════════
//  VISUAL PROXY
//  Represents a remote player as a tracked entity with position interpolation.
//
//  Phase 1 (current): State tracking + interpolation only.
//     Manages proxy state (position, rotation, animation) independently
//     of the game's entity system. No hooks, no game entities, no crashes.
//
//  Phase 2 (future): Ogre SceneNode rendering.
//     Once we discover the SceneManager at runtime via VTable scanning,
//     attach Ogre meshes to visualize remote players.
//
//  The key insight: all the STATE MANAGEMENT (interpolation, lifecycle,
//  ownership) works without Ogre headers. We just need Ogre for the final
//  "make it visible" step, which can be added incrementally.
// ═══════════════════════════════════════════════════════════════════════════

// ES: Estado de un proxy: IDs de red y dueño, posición/rotación actuales y objetivo
//     (hacia donde se interpola), velocidad, animación, nombre, malla (para el futuro
//     render), visible y vivo.
// EN: Proxy state: network and owner IDs, current and target position/rotation (what it
//     interpolates towards), speed, animation, name, mesh (for future rendering),
//     visible and alive.
struct ProxyState {
    EntityID    netId = INVALID_ENTITY;
    PlayerID    owner = INVALID_PLAYER;
    Vec3        position;
    Vec3        targetPosition;      // Interpolation target
    Quat        rotation;
    Quat        targetRotation;
    float       moveSpeed = 0.f;
    uint8_t     animState = 0;
    std::string name;
    std::string meshName;            // For future Ogre rendering
    bool        visible = true;
    bool        alive   = true;
};

// ES: Gestor de proxies (mapa netId -> proxy, protegido por mutex).
// EN: Proxy manager (netId -> proxy map, mutex-protected).
class VisualProxy {
public:
    VisualProxy() = default;
    ~VisualProxy() = default;

    // ES: Ciclo de vida: Initialize (sin dependencias en fase 1) y Shutdown (destruye todos).
    // EN:
    // ── Lifecycle ──

    // Initialize the proxy system. No external dependencies needed for Phase 1.
    bool Initialize();

    // Shutdown: destroy all proxies.
    void Shutdown();

    // ES: Actualización por frame: interpola posiciones hacia el objetivo (deltaTime en segundos).
    // EN:
    // ── Per-Frame Update ──
    // Interpolate positions toward targets. deltaTime in seconds.
    void Update(float deltaTime);

    // ES: Gestión de proxies: crear, destruir uno, los de un jugador o todos.
    // EN:
    // ── Proxy Management ──

    // Create a proxy for a remote entity.
    bool CreateProxy(EntityID netId, PlayerID owner,
                     const std::string& displayName,
                     const std::string& meshName,
                     const Vec3& position, const Quat& rotation);

    // Destroy a proxy.
    void DestroyProxy(EntityID netId);

    // Destroy all proxies owned by a specific player.
    void DestroyPlayerProxies(PlayerID owner);

    // Destroy all proxies.
    void DestroyAll();

    // ES: Actualizaciones de estado desde la red: objetivo de interpolación, teletransporte
    //     inmediato, visibilidad, vivo y nombre.
    // EN:
    // ── State Updates (from network) ──

    // Set interpolation target (called when position update arrives).
    void SetTargetPosition(EntityID netId, const Vec3& pos, const Quat& rot,
                           float moveSpeed, uint8_t animState);

    // Snap to position immediately (teleport).
    void SnapPosition(EntityID netId, const Vec3& pos, const Quat& rot);

    // Set visibility.
    void SetVisible(EntityID netId, bool visible);

    // Set alive state.
    void SetAlive(EntityID netId, bool alive);

    // Update display name.
    void SetName(EntityID netId, const std::string& name);

    // ES: Consultas.
    // EN: Queries.
    // ── Queries ──

    bool HasProxy(EntityID netId) const;
    size_t GetProxyCount() const;
    Vec3 GetPosition(EntityID netId) const;
    std::vector<EntityID> GetAllProxyIds() const;

    // ES: Estado completo de un proxy (depuración/diagnóstico).
    // EN:
    // Get full proxy state (for debug/diagnostics).
    bool GetProxyState(EntityID netId, ProxyState& out) const;

private:
    // ES: Datos internos: estado + punteros reservados para el futuro render con Ogre.
    // EN: Internal data: state + pointers reserved for future Ogre rendering.
    struct ProxyData {
        ProxyState state;
        void*      sceneNode  = nullptr; // Future: Ogre::SceneNode*
        void*      ogreEntity = nullptr; // Future: Ogre::Entity*
    };

    // ES: Interpola un proxy (se llama con el mutex tomado).
    // EN: Interpolates one proxy (called with the mutex held).
    void UpdateProxy(ProxyData& proxy, float deltaTime);

    mutable std::mutex m_mutex;
    std::unordered_map<EntityID, ProxyData> m_proxies;
    bool m_initialized = false;

    // ES: Velocidad de interpolación (fracción por segundo) y distancia a partir de la cual
    //     se teletransporta en vez de interpolar.
    // EN: Interpolation speed (fraction per second) and distance beyond which it snaps
    //     instead of interpolating.
    static constexpr float INTERP_SPEED = 10.0f;
    static constexpr float SNAP_DISTANCE = 50.0f;
};

} // namespace kmp::sdk
