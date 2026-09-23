// ES: Implementación de VisualProxy: alta/baja de proxies, actualizaciones de estado
//     desde la red e interpolación por frame. Todo protegido por m_mutex.
// EN: VisualProxy implementation: proxy creation/removal, state updates from the network
//     and per-frame interpolation. Everything guarded by m_mutex.
#include "visual_proxy.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>

namespace kmp::sdk {

// ES: Activa el sistema (en fase 1 no hay nada más que preparar).
// EN: Enables the system (nothing else to set up in phase 1).
bool VisualProxy::Initialize() {
    m_initialized = true;
    spdlog::info("VisualProxy: Initialized (state tracking active, Ogre rendering pending)");
    return true;
}

// ES: Destruye todos los proxies y desactiva el sistema.
// EN: Destroys every proxy and disables the system.
void VisualProxy::Shutdown() {
    DestroyAll();
    m_initialized = false;
}

// ES: Interpola todos los proxies hacia su objetivo.
// EN: Interpolates every proxy towards its target.
void VisualProxy::Update(float deltaTime) {
    if (!m_initialized) return;

    std::lock_guard lock(m_mutex);
    for (auto& [id, proxy] : m_proxies) {
        UpdateProxy(proxy, deltaTime);
    }
}

// ES: GESTIÓN DE PROXIES.
// ═══════════════════════════════════════════════════════════════════════════
//  PROXY MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

// ES: Crea un proxy nuevo (falla si ya existe uno con ese netId).
// EN: Creates a new proxy (fails if one with that netId already exists).
bool VisualProxy::CreateProxy(EntityID netId, PlayerID owner,
                               const std::string& displayName,
                               const std::string& meshName,
                               const Vec3& position, const Quat& rotation) {
    if (!m_initialized) return false;

    std::lock_guard lock(m_mutex);

    if (m_proxies.count(netId)) {
        spdlog::warn("VisualProxy: Proxy {} already exists, skipping create", netId);
        return false;
    }

    ProxyData proxy;
    proxy.state.netId = netId;
    proxy.state.owner = owner;
    proxy.state.name = displayName;
    proxy.state.meshName = meshName;
    proxy.state.position = position;
    proxy.state.targetPosition = position;
    proxy.state.rotation = rotation;
    proxy.state.targetRotation = rotation;

    m_proxies[netId] = std::move(proxy);

    spdlog::info("VisualProxy: Created proxy {} '{}' (pos=({:.0f},{:.0f},{:.0f}))",
                 netId, displayName, position.x, position.y, position.z);
    return true;
}

// ES: Destruye un proxy por netId.
// EN: Destroys a proxy by netId.
void VisualProxy::DestroyProxy(EntityID netId) {
    if (!m_initialized) return;

    std::lock_guard lock(m_mutex);
    auto it = m_proxies.find(netId);
    if (it == m_proxies.end()) return;

    spdlog::info("VisualProxy: Destroyed proxy {} '{}'", netId, it->second.state.name);
    m_proxies.erase(it);
}

// ES: Destruye todos los proxies de un jugador (recoge los IDs con el lock y luego los
//     borra uno a uno, porque DestroyProxy vuelve a tomar el mutex).
// EN: Destroys all of a player's proxies (collects IDs under the lock, then deletes them
//     one by one, because DestroyProxy takes the mutex again).
void VisualProxy::DestroyPlayerProxies(PlayerID owner) {
    if (!m_initialized) return;

    std::vector<EntityID> toRemove;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& [id, proxy] : m_proxies) {
            if (proxy.state.owner == owner) {
                toRemove.push_back(id);
            }
        }
    }

    for (EntityID id : toRemove) {
        DestroyProxy(id);
    }

    if (!toRemove.empty()) {
        spdlog::info("VisualProxy: Destroyed {} proxies for player {}", toRemove.size(), owner);
    }
}

// ES: Destruye todos los proxies.
// EN: Destroys every proxy.
void VisualProxy::DestroyAll() {
    std::lock_guard lock(m_mutex);
    size_t count = m_proxies.size();
    m_proxies.clear();

    if (count > 0) {
        spdlog::info("VisualProxy: Destroyed all {} proxies", count);
    }
}

// ES: ACTUALIZACIONES DE ESTADO.
// ═══════════════════════════════════════════════════════════════════════════
//  STATE UPDATES
// ═══════════════════════════════════════════════════════════════════════════

// ES: Fija el objetivo de interpolación (llega una actualización de posición por red).
// EN: Sets the interpolation target (a position update arrived from the network).
void VisualProxy::SetTargetPosition(EntityID netId, const Vec3& pos,
                                     const Quat& rot, float moveSpeed,
                                     uint8_t animState) {
    std::lock_guard lock(m_mutex);
    auto it = m_proxies.find(netId);
    if (it == m_proxies.end()) return;

    auto& state = it->second.state;
    state.targetPosition = pos;
    state.targetRotation = rot;
    state.moveSpeed = moveSpeed;
    state.animState = animState;
}

// ES: Teletransporta el proxy: posición y objetivo iguales, sin interpolar.
// EN: Teleports the proxy: position and target equal, no interpolation.
void VisualProxy::SnapPosition(EntityID netId, const Vec3& pos, const Quat& rot) {
    std::lock_guard lock(m_mutex);
    auto it = m_proxies.find(netId);
    if (it == m_proxies.end()) return;

    auto& state = it->second.state;
    state.position = pos;
    state.targetPosition = pos;
    state.rotation = rot;
    state.targetRotation = rot;
}

// ES: Setters simples de visibilidad, vivo y nombre.
// EN: Simple setters for visibility, alive and name.
void VisualProxy::SetVisible(EntityID netId, bool visible) {
    std::lock_guard lock(m_mutex);
    auto it = m_proxies.find(netId);
    if (it == m_proxies.end()) return;
    it->second.state.visible = visible;
}

void VisualProxy::SetAlive(EntityID netId, bool alive) {
    std::lock_guard lock(m_mutex);
    auto it = m_proxies.find(netId);
    if (it == m_proxies.end()) return;
    it->second.state.alive = alive;
}

void VisualProxy::SetName(EntityID netId, const std::string& name) {
    std::lock_guard lock(m_mutex);
    auto it = m_proxies.find(netId);
    if (it == m_proxies.end()) return;
    it->second.state.name = name;
}

// ES: CONSULTAS (todas bajo el mutex).
// ═══════════════════════════════════════════════════════════════════════════
//  QUERIES
// ═══════════════════════════════════════════════════════════════════════════

bool VisualProxy::HasProxy(EntityID netId) const {
    std::lock_guard lock(m_mutex);
    return m_proxies.count(netId) > 0;
}

size_t VisualProxy::GetProxyCount() const {
    std::lock_guard lock(m_mutex);
    return m_proxies.size();
}

Vec3 VisualProxy::GetPosition(EntityID netId) const {
    std::lock_guard lock(m_mutex);
    auto it = m_proxies.find(netId);
    if (it == m_proxies.end()) return {};
    return it->second.state.position;
}

std::vector<EntityID> VisualProxy::GetAllProxyIds() const {
    std::lock_guard lock(m_mutex);
    std::vector<EntityID> ids;
    ids.reserve(m_proxies.size());
    for (const auto& [id, _] : m_proxies) {
        ids.push_back(id);
    }
    return ids;
}

bool VisualProxy::GetProxyState(EntityID netId, ProxyState& out) const {
    std::lock_guard lock(m_mutex);
    auto it = m_proxies.find(netId);
    if (it == m_proxies.end()) return false;
    out = it->second.state;
    return true;
}

// ES: INTERPOLACIÓN.
// ═══════════════════════════════════════════════════════════════════════════
//  INTERPOLATION
// ═══════════════════════════════════════════════════════════════════════════

// ES: Si el objetivo está a más de SNAP_DISTANCE salta directamente; si no, avanza una
//     fracción INTERP_SPEED*dt (máx. 1) en posición (lineal) y rotación (slerp).
// EN: If the target is further than SNAP_DISTANCE it jumps straight there; otherwise it
//     advances a fraction INTERP_SPEED*dt (max 1) in position (linear) and rotation (slerp).
void VisualProxy::UpdateProxy(ProxyData& proxy, float deltaTime) {
    auto& state = proxy.state;

    float dx = state.targetPosition.x - state.position.x;
    float dy = state.targetPosition.y - state.position.y;
    float dz = state.targetPosition.z - state.position.z;
    float distSq = dx * dx + dy * dy + dz * dz;

    if (distSq > SNAP_DISTANCE * SNAP_DISTANCE) {
        // ES: Saltar si está demasiado lejos.
        // Snap if too far
        state.position = state.targetPosition;
        state.rotation = state.targetRotation;
    } else if (distSq > 0.001f) {
        // ES: Interpolación suave.
        // Smooth interpolation
        float t = std::min(1.0f, INTERP_SPEED * deltaTime);
        state.position.x += dx * t;
        state.position.y += dy * t;
        state.position.z += dz * t;
        state.rotation = Quat::Slerp(state.rotation, state.targetRotation, t);
    }

    // ES: Futuro: aplicar aquí al SceneNode de Ogre cuando se active el render.
    // Future: apply to Ogre SceneNode here when rendering is enabled
}

} // namespace kmp::sdk
