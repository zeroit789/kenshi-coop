// ES: Interpolación de entidades remotas. Para cada entidad guarda un buffer
//     circular de snapshots (posición, rotación, velocidad, animación) recibidos de la
//     red y calcula dónde dibujarla "un poco en el pasado" (retardo adaptativo según el
//     jitter), interpolando entre dos snapshots, extrapolando un poco si faltan datos y
//     suavizando los saltos con una corrección progresiva (snap correction).
//     Se compila también en KenshiMP.UnitTest.
// EN: Remote entity interpolation. For each entity it keeps a ring buffer of
//     snapshots (position, rotation, velocity, animation) received from the network and
//     computes where to draw it "slightly in the past" (adaptive delay based on
//     jitter), interpolating between two snapshots, extrapolating a little when data is
//     missing and smoothing jumps with a gradual snap correction.
//     Also compiled into KenshiMP.UnitTest.
#pragma once
#include "kmp/types.h"
#include "kmp/constants.h"
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cmath>

namespace kmp {

// ES: Estimador de jitter por entidad: media móvil exponencial (EMA) de la variación
//     del tiempo entre paquetes (spec §2.5). Traduce el jitter a un retardo de
//     interpolación adaptativo.
// Per-entity jitter estimator — EMA of inter-packet timing variance (spec §2.5)
struct JitterEstimator {
    float lastArrivalTime = 0.f;
    float jitterEma       = 0.02f; // Start at 20ms
    float adaptiveDelay   = KMP_INTERP_DELAY_SEC;
    bool  initialized     = false;

    // ES: Se llama al llegar cada paquete: mide el intervalo frente al esperado (tick de
    //     50 ms a 20 Hz), actualiza la EMA del jitter y recalcula el retardo adaptativo.
    // EN: Called on each packet arrival: measures the interval against the expected one
    //     (50 ms tick at 20 Hz), updates the jitter EMA and recomputes the adaptive delay.
    void OnPacketArrived(float arrivalTime) {
        if (!initialized) {
            lastArrivalTime = arrivalTime;
            initialized = true;
            return;
        }
        float interval = arrivalTime - lastArrivalTime;
        float expectedInterval = KMP_TICK_INTERVAL_SEC; // 50ms at 20Hz
        float jitter = std::abs(interval - expectedInterval);
        jitterEma = KMP_JITTER_EMA_ALPHA * jitter + (1.f - KMP_JITTER_EMA_ALPHA) * jitterEma;
        lastArrivalTime = arrivalTime;

        // ES: Mapear jitter a retardo: 20 ms de jitter → retardo mínimo, 80 ms → retardo máximo
        //     (lineal entre KMP_INTERP_DELAY_MIN y KMP_INTERP_DELAY_MAX).
        // Map jitter to delay: 20ms jitter→50ms delay, 80ms jitter→200ms delay
        float t = (jitterEma - 0.02f) / (0.08f - 0.02f);
        t = std::clamp(t, 0.f, 1.f);
        adaptiveDelay = KMP_INTERP_DELAY_MIN + t * (KMP_INTERP_DELAY_MAX - KMP_INTERP_DELAY_MIN);
    }

    // ES: Retardo de interpolación actual (segundos).
    // EN: Current interpolation delay (seconds).
    float GetDelay() const { return adaptiveDelay; }
};

// ES: Estado de corrección de "salto" por entidad (spec §2.5): cuando la posición
//     recibida se aleja de la prevista, el error se reparte en el tiempo en vez de
//     teletransportar.
// Per-entity snap correction state (spec §2.5)
struct SnapCorrection {
    bool  active      = false;
    Vec3  errorOffset;          // Remaining error to blend out
    float blendTimer  = 0.f;    // Time remaining in blend
    float blendTotal  = 0.f;    // Total blend duration

    // ES: Empieza una corrección según el tamaño del error: > máximo = teletransporte
    //     (no se mezcla); entre mínimo y máximo = mezcla durante KMP_SNAP_CORRECT_SEC;
    //     < mínimo = no hace nada.
    // EN: Starts a correction depending on the error size: > max = teleport (no blend);
    //     between min and max = blend over KMP_SNAP_CORRECT_SEC; < min = does nothing.
    void StartCorrection(const Vec3& error) {
        float dist = error.Length();
        if (dist > KMP_SNAP_THRESHOLD_MAX) {
            // ES: Teletransporte: no hace falta mezclar, el que llama mueve al instante.
            // Teleport — no blend needed, caller handles instant move
            active = false;
            return;
        }
        if (dist > KMP_SNAP_THRESHOLD_MIN) {
            errorOffset = error;
            blendTimer = KMP_SNAP_CORRECT_SEC;
            blendTotal = KMP_SNAP_CORRECT_SEC;
            active = true;
        }
        // ES: Por debajo del umbral: sin corrección, la interpolación normal lo absorbe.
        // Below threshold: no correction needed, normal interpolation handles it
    }

    // ES: Avanza el temporizador dt segundos y devuelve la parte del error que aún queda
    //     (decrece linealmente hasta 0).
    // EN: Advances the timer by dt seconds and returns the remaining part of the error
    //     (decreases linearly to 0).
    Vec3 Apply(float dt) {
        if (!active) return Vec3(0.f, 0.f, 0.f);
        blendTimer -= dt;
        if (blendTimer <= 0.f) {
            active = false;
            return Vec3(0.f, 0.f, 0.f);
        }
        // ES: Proporción de error que queda.
        // Proportion of error remaining
        float ratio = blendTimer / blendTotal;
        return errorOffset * ratio;
    }
};

// ES: Interpolador de todas las entidades remotas. Thread-safe con un mutex.
// EN: Interpolator for all remote entities. Thread-safe with one mutex.
class Interpolation {
public:
    // ES: Un snapshot recibido: tiempo, tick, posición, velocidad (calculada a partir de
    //     snapshots consecutivos), rotación, velocidad de movimiento y estado de animación.
    // EN: One received snapshot: time, tick, position, velocity (computed from
    //     consecutive snapshots), rotation, move speed and animation state.
    struct Snapshot {
        float      timestamp  = 0.f;
        TickNumber tick       = 0;
        Vec3       position;
        Vec3       velocity;   // Computed from consecutive snapshots
        Quat       rotation;
        uint8_t    moveSpeed  = 0;
        uint8_t    animState  = 0;
    };

    // ES: Añade un snapshot de una entidad (crea su estado si no existía).
    // EN: Adds a snapshot for an entity (creates its state if it did not exist).
    void AddSnapshot(EntityID entityId, float timestamp, const Vec3& pos, const Quat& rot,
                     uint8_t moveSpeed = 0, uint8_t animState = 0);

    // ES: Devuelve la posición/rotación interpolada para dibujar (false si no hay datos).
    // Get interpolated position/rotation for rendering
    bool GetInterpolated(EntityID entityId, float renderTime,
                         Vec3& outPos, Quat& outRot) const;

    // ES: Igual, pero además devuelve velocidad de movimiento y estado de animación.
    // Get interpolated position/rotation with movement data
    bool GetInterpolated(EntityID entityId, float renderTime,
                         Vec3& outPos, Quat& outRot,
                         uint8_t& outMoveSpeed, uint8_t& outAnimState) const;

    // ES: Quitar una entidad, vaciar todo, y avanzar el reloj interno (dt del frame).
    // EN: Remove one entity, clear everything, and advance the internal clock (frame dt).
    void RemoveEntity(EntityID entityId);
    void Clear();
    void Update(float deltaTime);

private:
    // ES: Estado por entidad: buffer circular de snapshots, estimador de jitter y
    //     corrección de salto.
    // Per-entity state
    struct EntityInterpState {
        Snapshot       buffer[KMP_MAX_SNAPSHOTS]; // Ring buffer (spec: 8 entries)
        int            head     = 0;    // Next write index
        int            count    = 0;    // Number of valid entries
        JitterEstimator jitter;
        SnapCorrection  snap;

        // ES: Escribe en la posición head y avanza (sobrescribe el más viejo si está lleno).
        // EN: Writes at the head position and advances (overwrites the oldest when full).
        void PushSnapshot(const Snapshot& s) {
            buffer[head] = s;
            head = (head + 1) % KMP_MAX_SNAPSHOTS;
            if (count < KMP_MAX_SNAPSHOTS) count++;
        }

        // ES: Snapshot por antigüedad (0 = el más nuevo, count-1 = el más viejo).
        // Get snapshot by age (0=newest, count-1=oldest)
        const Snapshot* Get(int age) const {
            if (age < 0 || age >= count) return nullptr;
            int idx = (head - 1 - age + KMP_MAX_SNAPSHOTS * 2) % KMP_MAX_SNAPSHOTS;
            return &buffer[idx];
        }
    };

    // ES: Núcleo del cálculo de interpolación/extrapolación para una entidad.
    // EN: Core interpolation/extrapolation computation for one entity.
    bool InterpolateEntity(const EntityInterpState& state, float renderTime,
                           Vec3& outPos, Quat& outRot,
                           uint8_t* outMoveSpeed, uint8_t* outAnimState) const;

    // ES: Mutex, estados por entidad (mutable porque GetInterpolated, que es const,
    //     avanza la corrección de salto), reloj acumulado y último dt.
    // EN: Mutex, per-entity states (mutable because the const GetInterpolated advances
    //     the snap correction), accumulated clock and last dt.
    mutable std::mutex m_mutex;
    mutable std::unordered_map<EntityID, EntityInterpState> m_entities;
    float m_currentTime = 0.f;
    float m_deltaTime   = 0.f;
};

} // namespace kmp
