// ES: game_state.cpp - Implementación de GameStateManager (reloj, clima, día/noche).
// EN: game_state.cpp - GameStateManager implementation (clock, weather, day/night).
#include "game_state.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace kmp {

// ES: Suma uptime, avanza la hora (un día = 86400 s reales a velocidad 1) y actualiza el clima.
// EN: Adds uptime, advances the time (one day = 86400 real seconds at speed 1) and updates weather.
void GameStateManager::Update(float deltaTime, float gameSpeed) {
    m_uptime += deltaTime;

    // ES: Avanza la hora del día (ciclo de 24 h escalado por gameSpeed); al pasar de 1 empieza un día nuevo.
    // EN: Advance time of day (24-hour cycle scaled by gameSpeed)
    m_timeOfDay += deltaTime * gameSpeed / 86400.f;
    if (m_timeOfDay >= 1.f) {
        m_timeOfDay -= 1.f;
        m_dayNumber++;
    }

    // ES: Actualiza el clima periódicamente.
    // EN: Update weather periodically
    UpdateWeather(deltaTime);
}

// ES: Fija la hora quedándose solo con la parte fraccionaria (rango [0, 1)).
// EN: Sets the time keeping only the fractional part (range [0, 1)).
void GameStateManager::SetTimeOfDay(float time) {
    m_timeOfDay = time - std::floor(time); // Clamp to [0, 1)
    if (m_timeOfDay < 0.f) m_timeOfDay += 1.f;
}

// ES: Formatea "Day N, HH:MM" a partir de la hora normalizada.
// EN: Formats "Day N, HH:MM" from the normalized time.
std::string GameStateManager::GetTimeString() const {
    // ES: Convierte 0.0-1.0 a horas:minutos.
    // EN: Convert 0.0-1.0 to hours:minutes
    float hours24 = m_timeOfDay * 24.f;
    int hour = static_cast<int>(hours24) % 24;
    int minute = static_cast<int>((hours24 - std::floor(hours24)) * 60.f);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "Day %d, %02d:%02d", m_dayNumber, hour, minute);
    return buf;
}

// ES: De día entre las 06:00 (0.25) y las 18:00 (0.75).
// EN: Daytime between 06:00 (0.25) and 18:00 (0.75).
bool GameStateManager::IsDaytime() const {
    return m_timeOfDay >= 0.25f && m_timeOfDay < 0.75f;
}

// ES: Cada m_weatherInterval segundos tira un dado 0-99 y elige clima con pesos fijos
//     (40 % despejado, 25 % nublado, 15 % polvo, 15 % lluvia, 5 % ácida).
// EN: Every m_weatherInterval seconds rolls 0-99 and picks weather with fixed weights
//     (40% clear, 25% cloudy, 15% dust, 15% rain, 5% acid).
void GameStateManager::UpdateWeather(float deltaTime) {
    m_weatherTimer += deltaTime;
    if (m_weatherTimer < m_weatherInterval) return;
    m_weatherTimer = 0.f;

    // ES: Transiciones de clima aleatorias sencillas; lo común es despejado y la lluvia ácida es rara.
    // EN: Simple random weather transitions
    // Kenshi weather: 0=clear, 1=cloudy, 2=dust storm, 3=rain, 4=acid rain
    // Weighted: clear is most common, acid rain is rare
    int roll = std::rand() % 100;
    if (roll < 40)       m_weatherState = 0; // 40% clear
    else if (roll < 65)  m_weatherState = 1; // 25% cloudy
    else if (roll < 80)  m_weatherState = 2; // 15% dust
    else if (roll < 95)  m_weatherState = 3; // 15% rain
    else                 m_weatherState = 4; // 5% acid rain
}

// ES: Compara el estado día/noche actual con el último visto; true solo cuando cambia.
// EN: Compares the current day/night state with the last one seen; true only when it changes.
bool GameStateManager::CheckDayNightTransition() {
    bool isDaytime = IsDaytime();
    if (isDaytime != m_wasDaytime) {
        m_wasDaytime = isDaytime;
        return true; // Transition occurred
    }
    return false;
}

} // namespace kmp
