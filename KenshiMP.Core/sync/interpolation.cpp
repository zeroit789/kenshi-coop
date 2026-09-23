// ES: Implementación de Interpolation (ver interpolation.h).
// EN: Interpolation implementation (see interpolation.h).
#include "interpolation.h"
#include <algorithm>
#include <cmath>

namespace kmp {

// ES: Valida el snapshot, actualiza el jitter, calcula la velocidad frente al
//     snapshot anterior, decide si hay que corregir un salto y lo guarda en el buffer.
// EN: Validates the snapshot, updates jitter, computes velocity against the previous
//     snapshot, decides whether a jump must be corrected and stores it in the buffer.
void Interpolation::AddSnapshot(EntityID entityId, float timestamp,
                                const Vec3& pos, const Quat& rot,
                                uint8_t moveSpeed, uint8_t animState) {
    // ES: Rechazar posiciones NaN/Inf: un solo valor malo corrompe el buffer durante
    //     varios frames de interpolación y extrapolación.
    // Reject NaN/Inf positions — a single bad value corrupts the ring buffer
    // for multiple frames of interpolation and extrapolation.
    if (std::isnan(pos.x) || std::isnan(pos.y) || std::isnan(pos.z) ||
        std::isinf(pos.x) || std::isinf(pos.y) || std::isinf(pos.z)) {
        return;
    }
    // ES: Rechazar también componentes NaN/Inf del cuaternión de rotación.
    // Reject NaN/Inf rotation quaternion components as well
    if (std::isnan(rot.w) || std::isnan(rot.x) || std::isnan(rot.y) || std::isnan(rot.z) ||
        std::isinf(rot.w) || std::isinf(rot.x) || std::isinf(rot.y) || std::isinf(rot.z)) {
        return;
    }

    std::lock_guard lock(m_mutex);
    auto& state = m_entities[entityId];

    // ES: Actualizar el estimador de jitter con el tiempo real de llegada del paquete.
    // Update jitter estimator with actual packet arrival time
    state.jitter.OnPacketArrived(timestamp);

    // ES: Calcular la velocidad a partir del snapshot anterior (solo si dt es razonable).
    // Compute velocity from previous snapshot
    Vec3 velocity(0.f, 0.f, 0.f);
    if (state.count > 0) {
        const Snapshot* prev = state.Get(0); // Most recent
        if (prev) {
            float dt = timestamp - prev->timestamp;
            if (dt > 0.001f && dt < 2.0f) {
                velocity = (pos - prev->position) * (1.f / dt);
            }

            // ES: Comprobar si hay que corregir un salto: gran discontinuidad frente a la
            //     posición prevista (posición anterior + velocidad * dt).
            // Check for snap correction: large position discontinuity
            Vec3 expectedPos = prev->position + prev->velocity * dt;
            Vec3 error = pos - expectedPos;
            float errorDist = error.Length();

            if (errorDist > KMP_SNAP_THRESHOLD_MAX) {
                // ES: Teletransporte: anular la corrección y dejar que la posición salte.
                // Teleport — reset snap correction, let position jump
                state.snap.active = false;
            } else if (errorDist > KMP_SNAP_THRESHOLD_MIN) {
                state.snap.StartCorrection(error);
            }
        }
    }

    // ES: Guardar el snapshot nuevo (tick no se usa: siempre 0).
    // EN: Store the new snapshot (tick is unused: always 0).
    Snapshot snap;
    snap.timestamp = timestamp;
    snap.tick = 0;
    snap.position = pos;
    snap.velocity = velocity;
    snap.rotation = rot;
    snap.moveSpeed = moveSpeed;
    snap.animState = animState;
    state.PushSnapshot(snap);
}

// ES: Calcula la posición a mostrar en renderTime menos el retardo adaptativo.
//     Casos: sin datos, solo snapshots futuros, más allá del último (extrapolación) o
//     interpolación normal entre dos snapshots que rodean el instante.
// EN: Computes the position to show at renderTime minus the adaptive delay.
//     Cases: no data, only future snapshots, past the last one (extrapolation) or
//     normal interpolation between two snapshots bracketing the instant.
bool Interpolation::InterpolateEntity(const EntityInterpState& state, float renderTime,
                                       Vec3& outPos, Quat& outRot,
                                       uint8_t* outMoveSpeed, uint8_t* outAnimState) const {
    if (state.count == 0) return false;

    // ES: Usar el retardo adaptativo del estimador de jitter.
    // Use adaptive delay from jitter estimator
    float interpTime = renderTime - state.jitter.GetDelay();

    // ES: Buscar los snapshots que rodean interpTime (antes/después). El buffer está
    //     ordenado por inserción; se recorren todas las entradas válidas.
    // Find bracket snapshots (before/after interpTime)
    // Ring buffer is ordered by insertion time; scan all valid entries
    const Snapshot* before = nullptr;
    const Snapshot* after  = nullptr;

    for (int i = 0; i < state.count; i++) {
        const Snapshot* s = state.Get(i);
        if (!s) continue;

        if (s->timestamp <= interpTime) {
            if (!before || s->timestamp > before->timestamp) {
                before = s;
            }
        }
        if (s->timestamp >= interpTime) {
            if (!after || s->timestamp < after->timestamp) {
                after = s;
            }
        }
    }

    // ES: Caso 1: no hay datos.
    // Case 1: No data at all
    if (!before && !after) return false;

    // ES: Caso 2: solo snapshots futuros → usar el más temprano.
    // Case 2: Only future snapshots — use earliest
    if (!before) {
        outPos = after->position;
        outRot = after->rotation;
        if (outMoveSpeed) *outMoveSpeed = after->moveSpeed;
        if (outAnimState) *outAnimState = after->animState;
        return true;
    }

    // ES: Caso 3: más allá de todos los snapshots → extrapolar (spec §2.5).
    // Case 3: Past all snapshots — extrapolate (spec §2.5)
    if (!after || after == before) {
        float overshoot = interpTime - before->timestamp;

        if (overshoot > 0.f && overshoot <= KMP_EXTRAP_MAX_SEC && before->velocity.LengthSq() > 0.001f) {
            // ES: Estimación por velocidad (dead reckoning): posición + velocidad * dt, con tope
            //     KMP_EXTRAP_MAX_SEC (250 ms según el comentario original).
            // Dead reckoning: position + velocity * dt, capped at 250ms
            outPos = before->position + before->velocity * overshoot;
        } else {
            outPos = before->position;
        }
        outRot = before->rotation;
        if (outMoveSpeed) *outMoveSpeed = before->moveSpeed;
        if (outAnimState) *outAnimState = before->animState;
        return true;
    }

    // ES: Caso 4: interpolación normal entre los dos snapshots que rodean el instante.
    // Case 4: Normal interpolation between bracket snapshots
    float span = after->timestamp - before->timestamp;
    float t = (span > 0.001f) ? (interpTime - before->timestamp) / span : 0.f;
    t = std::clamp(t, 0.f, 1.f);

    // ES: Hermite/cúbica sería lo ideal, pero una interpolación lineal basta a 20 Hz.
    // Hermite/cubic would be ideal but linear lerp is good enough for 20Hz
    outPos = before->position + (after->position - before->position) * t;
    outRot = Quat::Slerp(before->rotation, after->rotation, t);

    // ES: Estados discretos: se usa el valor del snapshot más cercano.
    // Discrete states: use nearest snapshot's values
    if (outMoveSpeed) *outMoveSpeed = (t > 0.5f) ? after->moveSpeed : before->moveSpeed;
    if (outAnimState) *outAnimState = (t > 0.5f) ? after->animState : before->animState;

    return true;
}

// ES: Versión sin datos de movimiento. Tras interpolar, resta la corrección de salto
//     pendiente. Nota: cada llamada avanza el temporizador de la corrección con el dt
//     del último frame; si se llama varias veces por frame, la corrección decae más rápido.
// EN: Version without movement data. After interpolating, it subtracts the pending
//     snap correction. Note: each call advances the correction timer by the last frame
//     dt; if called several times per frame, the correction decays faster.
bool Interpolation::GetInterpolated(EntityID entityId, float renderTime,
                                    Vec3& outPos, Quat& outRot) const {
    std::lock_guard lock(m_mutex);

    auto it = m_entities.find(entityId);
    if (it == m_entities.end()) return false;

    if (!InterpolateEntity(it->second, renderTime, outPos, outRot, nullptr, nullptr))
        return false;

    // ES: Aplicar la corrección de salto con decaimiento en tiempo real (estado mutable).
    // Apply snap correction with real-time decay (mutable state)
    if (it->second.snap.active) {
        Vec3 correction = it->second.snap.Apply(m_deltaTime);
        outPos = outPos - correction;
    }

    return true;
}

// ES: Versión con velocidad de movimiento y estado de animación (misma lógica).
// EN: Version with move speed and animation state (same logic).
bool Interpolation::GetInterpolated(EntityID entityId, float renderTime,
                                    Vec3& outPos, Quat& outRot,
                                    uint8_t& outMoveSpeed, uint8_t& outAnimState) const {
    std::lock_guard lock(m_mutex);

    auto it = m_entities.find(entityId);
    if (it == m_entities.end()) return false;

    if (!InterpolateEntity(it->second, renderTime, outPos, outRot, &outMoveSpeed, &outAnimState))
        return false;

    // ES: Aplicar la corrección de salto con decaimiento en tiempo real (estado mutable).
    // Apply snap correction with real-time decay (mutable state)
    if (it->second.snap.active) {
        Vec3 correction = it->second.snap.Apply(m_deltaTime);
        outPos = outPos - correction;
    }

    return true;
}

// ES: Olvida el estado de interpolación de una entidad.
// EN: Forgets an entity's interpolation state.
void Interpolation::RemoveEntity(EntityID entityId) {
    std::lock_guard lock(m_mutex);
    m_entities.erase(entityId);
}

// ES: Borra todas las entidades y reinicia el reloj.
// EN: Removes all entities and resets the clock.
void Interpolation::Clear() {
    std::lock_guard lock(m_mutex);
    m_entities.clear();
    m_currentTime = 0.f;
}

// ES: Avanza el reloj interno y guarda el dt del frame.
// EN: Advances the internal clock and stores the frame dt.
void Interpolation::Update(float deltaTime) {
    std::lock_guard lock(m_mutex);
    m_currentTime += deltaTime;
    m_deltaTime = deltaTime;
    // ES: Nota: los temporizadores de corrección se descuentan dentro de
    //     SnapCorrection::Apply(), que se llama desde GetInterpolated(). No se descuentan
    //     aquí: hacerlo en los dos sitios los restaría dos veces.
    // Note: snap correction timers are ticked inside SnapCorrection::Apply()
    // which is called from GetInterpolated(). No need to tick here — doing
    // both would double-decrement the timer.
}

} // namespace kmp
