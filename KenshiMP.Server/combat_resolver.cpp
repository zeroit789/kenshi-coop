// ES: combat_resolver.cpp - Resolución de combate en el servidor (ResolveCombat).
//     Modelo simplificado inspirado en Kenshi: elige parte del cuerpo al azar ponderado,
//     calcula daño corte/contundente, bloqueo, KO y muerte, y descuenta vida a la entidad objetivo.
//     La llama GameServer::HandleCombatAttack (server.cpp), que la declara con su propio prototipo.
// EN: combat_resolver.cpp - Server-side combat resolution (ResolveCombat).
//     Simplified Kenshi-inspired model: weighted random body part, cut/blunt damage, block,
//     KO and death, and subtracts health from the target entity.
//     Called by GameServer::HandleCombatAttack (server.cpp), which declares its own prototype.
#include "server.h"
#include <spdlog/spdlog.h>
#include <random>
#include <algorithm>
#include <cmath>

namespace kmp {

// ES: Resolución de combate aproximada a Kenshi para el servidor dedicado. Resumen del modelo:
//     - Los ataques golpean una parte del cuerpo (cabeza, pecho, estómago, brazos, piernas).
//     - El daño se divide en corte y contundente (perforante opcional).
//     - La defensa reduce el daño: daño_efectivo = ataque * (1 - defensa/100).
//     - KO: cualquier parte por debajo de -50 a -100.
//     - Muerte: pecho o cabeza <= -100 (en Kenshi la muerte es rara; en MP se trata el KO crítico como muerte).
//     - La elección de parte es ponderada (pecho lo más probable, cabeza rara).
// EN: Kenshi-approximated combat resolution for the dedicated server.
// This implements a simplified version of Kenshi's combat model:
//
// Kenshi combat basics:
// - Attacks target specific body parts (head, chest, stomach, arms, legs)
// - Damage is split into cut and blunt (with optional pierce)
// - Defense reduces damage: effective_dmg = attack * (1 - defense/100)
// - KO threshold: any body part below -50 to -100 triggers knockout
// - Death threshold: chest or head <= -100 (permanent death in Kenshi is rare,
//   but for MP we treat critical KO as death)
// - Body part selection is weighted (chest most likely, head rare)

// ES: Resultado de un ataque: parte golpeada, daño por tipo, vida resultante de esa parte
//     y banderas de bloqueo/KO/muerte. server.cpp lo usa para rellenar los paquetes S2C de combate.
// EN: Result of one attack: hit part, damage per type, resulting health of that part and
//     block/KO/death flags. server.cpp uses it to fill the S2C combat packets.
struct CombatResult {
    BodyPart hitPart;
    float cutDamage;
    float bluntDamage;
    float pierceDamage;
    float resultHealth;
    bool wasBlocked;
    bool wasKO;
    bool wasDeath;
};

// ES: Generador aleatorio del módulo, sembrado una vez con random_device.
// EN: Module-wide random generator, seeded once from random_device.
static std::mt19937 s_rng(std::random_device{}());

// ES: Elige la parte del cuerpo golpeada con pesos aproximados a Kenshi (suman 100):
//     pecho 30, estómago 20, cabeza y cada extremidad 10.
// EN: Body part selection weights (Kenshi-approximated)
static BodyPart SelectBodyPart() {
    static const struct { BodyPart part; int weight; } weights[] = {
        {BodyPart::Chest,    30},
        {BodyPart::Stomach,  20},
        {BodyPart::Head,     10},
        {BodyPart::LeftArm,  10},
        {BodyPart::RightArm, 10},
        {BodyPart::LeftLeg,  10},
        {BodyPart::RightLeg, 10},
    };
    static const int totalWeight = 100;

    std::uniform_int_distribution<int> dist(1, totalWeight);
    int roll = dist(s_rng);

    int cumulative = 0;
    for (auto& w : weights) {
        cumulative += w.weight;
        if (roll <= cumulative) return w.part;
    }
    return BodyPart::Chest;
}

// ES: Resuelve un ataque entre dos entidades y modifica la vida (y alive) de target.
//     OJO: attacker y attackType no se usan todavía; ataque (20) y defensa (10) son fijos
//     y el reparto corte/contundente es siempre 50/50.
// EN: Resolve a combat attack between two entities (mutates target health and alive).
//     NOTE: attacker and attackType are not used yet; attack (20) and defense (10) are
//     hard-coded and the cut/blunt split is always 50/50.
// Resolve a combat attack between two entities
CombatResult ResolveCombat(const ServerEntity& attacker, ServerEntity& target,
                           uint8_t attackType) {
    CombatResult result{};
    result.hitPart = SelectBodyPart();

    // ES: Estadística de ataque efectiva del atacante. En Kenshi sale de la habilidad de
    //     ataque cuerpo a cuerpo + daño del arma; aquí se aproxima con valores base fijos.
    // EN: Get attacker's effective attack stat
    // In Kenshi, attack stat comes from the melee attack skill + weapon damage
    // For the server, we approximate with a base stat stored on the entity
    float attackStat = 20.f; // Default base attack
    float defenseStat = 10.f; // Default base defense

    // ES: El tipo de arma decidiría el reparto corte/contundente (0 = equilibrado, 1 = corte,
    //     2 = contundente). Sin implementar: siempre 50/50.
    // EN: Weapon type determines cut/blunt split
    // 0 = balanced (50/50), 1 = cutting, 2 = blunt
    float cutRatio = 0.5f;
    float bluntRatio = 0.5f;

    // ES: Fórmula de daño: ataque * factor aleatorio [0.8, 1.2] * (1 - defensa/100),
    //     con la reducción por defensa limitada al 90 %.
    // EN: Damage formula: base = attackStat * randomFactor * (1 - defense/100)
    std::uniform_real_distribution<float> damageDist(0.8f, 1.2f);
    float randomFactor = damageDist(s_rng);
    float defenseReduction = 1.f - std::clamp(defenseStat / 100.f, 0.f, 0.9f);

    float totalDamage = attackStat * randomFactor * defenseReduction;

    result.cutDamage = totalDamage * cutRatio;
    result.bluntDamage = totalDamage * bluntRatio;
    result.pierceDamage = 0.f;

    // ES: Probabilidad de bloqueo simplificada: 20 %; un golpe bloqueado hace el 30 % del daño.
    // EN: Block chance (simplified): 20% base block
    std::uniform_real_distribution<float> blockDist(0.f, 1.f);
    if (blockDist(s_rng) < 0.2f) {
        result.wasBlocked = true;
        result.cutDamage *= 0.3f;   // Blocked hits do 30% damage
        result.bluntDamage *= 0.3f;
    }

    // ES: Aplica el daño a la parte golpeada (7 partes del cuerpo) y guarda la vida resultante.
    // EN: Apply damage to target health
    int partIdx = static_cast<int>(result.hitPart);
    if (partIdx >= 0 && partIdx < 7) {
        target.health[partIdx] -= (result.cutDamage + result.bluntDamage + result.pierceDamage);
        result.resultHealth = target.health[partIdx];
    }

    // ES: Umbral de KO: cualquier parte en -50 o menos.
    // EN: Check KO threshold: any part below -50
    result.wasKO = false;
    for (int i = 0; i < 7; i++) {
        if (target.health[i] <= -50.f) {
            result.wasKO = true;
            break;
        }
    }

    // ES: Umbral de muerte: pecho o cabeza en -100 o menos; marca la entidad como no viva.
    // EN: Check death threshold: chest or head <= -100
    result.wasDeath = false;
    if (target.health[static_cast<int>(BodyPart::Chest)] <= -100.f ||
        target.health[static_cast<int>(BodyPart::Head)] <= -100.f) {
        result.wasDeath = true;
        target.alive = false;
    }

    return result;
}

} // namespace kmp
