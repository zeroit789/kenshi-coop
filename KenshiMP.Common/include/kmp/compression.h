// ES: Utilidades de compresión de datos de red (solo cabecera).
//     Convierte floats a half-float (16 bits), codifica posiciones como deltas
//     respecto a la última posición conocida y empaqueta velocidades en 3 bytes.
//     Sirve para reducir el tamaño de los paquetes de sincronización de posición.
// EN: Network data compression helpers (header-only).
//     Converts floats to half-floats (16 bits), encodes positions as deltas
//     from the last known position and packs velocities into 3 bytes.
//     Used to shrink position-sync packets.
#pragma once
#include "types.h"
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace kmp {

// ES: Convierte un float de 32 bits a half-float (float16) para deltas de posición compactos.
//     Conversión simplificada: los valores demasiado pequeños (subnormales) se redondean a cero
//     con signo y los demasiado grandes se convierten en infinito. La mantisa se trunca (sin redondeo).
// EN: Half-float (float16) for compact position deltas.
//     Simplified conversion: values too small (subnormals) flush to signed zero and
//     values too large become infinity. The mantissa is truncated (no rounding).
inline uint16_t FloatToHalf(float value) {
    uint32_t f;
    std::memcpy(&f, &value, 4);
    uint32_t sign = (f >> 16) & 0x8000;
    int32_t exponent = ((f >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = f & 0x7FFFFF;

    // ES: Exponente fuera de rango por abajo: se devuelve cero con signo.
    // EN: Exponent underflow: return signed zero.
    if (exponent <= 0) {
        return static_cast<uint16_t>(sign);
    }
    // ES: Exponente fuera de rango por arriba: se devuelve infinito con signo.
    // EN: Exponent overflow: return signed infinity.
    if (exponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00); // Inf
    }
    return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
}

// ES: Convierte un half-float (float16) de vuelta a float de 32 bits.
//     Soporta cero, subnormales (los normaliza), infinito/NaN y valores normales.
// EN: Converts a half-float (float16) back to a 32-bit float.
//     Handles zero, subnormals (normalizes them), infinity/NaN and normal values.
inline float HalfToFloat(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;

    if (exponent == 0) {
        // ES: Cero con signo.
        // EN: Signed zero.
        if (mantissa == 0) {
            uint32_t result = sign;
            float f;
            std::memcpy(&f, &result, 4);
            return f;
        }
        // ES: Subnormal: se desplaza la mantisa hasta normalizarla ajustando el exponente.
        // EN: Denormalized
        exponent = 1;
        while (!(mantissa & 0x400)) {
            mantissa <<= 1;
            exponent--;
        }
        mantissa &= 0x3FF;
    } else if (exponent == 31) {
        // ES: Infinito o NaN: exponente máximo en float32.
        // EN: Infinity or NaN: maximum exponent in float32.
        uint32_t result = sign | 0x7F800000 | (mantissa << 13);
        float f;
        std::memcpy(&f, &result, 4);
        return f;
    }

    // ES: Valor normal: se reajusta el sesgo del exponente (15 -> 127).
    // EN: Normal value: rebias the exponent (15 -> 127).
    uint32_t result = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &result, 4);
    return f;
}

// ES: Codificación delta de posición: guarda el desplazamiento respecto a la última
//     posición conocida como 3 half-floats (6 bytes en vez de 12).
// EN: Delta position encoding: store offset from last known position as 3x float16
struct DeltaPosition {
    uint16_t dx, dy, dz;

    // ES: Calcula el delta (current - previous) y lo comprime a float16.
    // EN: Computes the delta (current - previous) and compresses it to float16.
    static DeltaPosition Encode(const Vec3& current, const Vec3& previous) {
        return {
            FloatToHalf(current.x - previous.x),
            FloatToHalf(current.y - previous.y),
            FloatToHalf(current.z - previous.z)
        };
    }

    // ES: Reconstruye la posición absoluta sumando el delta a la posición anterior.
    // EN: Rebuilds the absolute position by adding the delta to the previous position.
    Vec3 Decode(const Vec3& previous) const {
        return {
            previous.x + HalfToFloat(dx),
            previous.y + HalfToFloat(dy),
            previous.z + HalfToFloat(dz)
        };
    }
};

// ES: Empaquetado de velocidad: 3 int8 escalados al rango [-15, 15] m/s
//     (valores fuera de rango se recortan). Precisión ~0,12 m/s.
// EN: Velocity packing: 3x int8 scaled to [-15, 15] m/s
//     (out-of-range values are clamped). Precision ~0.12 m/s.
struct PackedVelocity {
    int8_t vx, vy, vz;

    // ES: Escala cada componente de m/s a [-127, 127].
    // EN: Scales each component from m/s to [-127, 127].
    static PackedVelocity Encode(const Vec3& vel) {
        return {
            static_cast<int8_t>(std::clamp(vel.x / 15.f * 127.f, -127.f, 127.f)),
            static_cast<int8_t>(std::clamp(vel.y / 15.f * 127.f, -127.f, 127.f)),
            static_cast<int8_t>(std::clamp(vel.z / 15.f * 127.f, -127.f, 127.f))
        };
    }

    // ES: Deshace el escalado y devuelve la velocidad en m/s.
    // EN: Undoes the scaling and returns the velocity in m/s.
    Vec3 Decode() const {
        return {
            static_cast<float>(vx) / 127.f * 15.f,
            static_cast<float>(vy) / 127.f * 15.f,
            static_cast<float>(vz) / 127.f * 15.f
        };
    }
};

} // namespace kmp
