// ES: game_state.h - GameStateManager: reloj del mundo (hora del día, número de día), clima
//     aleatorio y detección de transición día/noche. Se compila en el servidor pero GameServer
//     no lo usa: lleva su propio m_timeOfDay/m_weatherState en server.cpp.
// EN: game_state.h - GameStateManager: world clock (time of day, day number), random weather
//     and day/night transition detection. Compiled into the server but GameServer does not use
//     it: it keeps its own m_timeOfDay/m_weatherState in server.cpp.
#pragma once
#include "kmp/types.h"
#include <string>

namespace kmp {

// ES: Utilidades de estado de partida para el servidor dedicado: avance del tiempo,
//     cambios de clima y eventos del mundo.
// EN: Game state management utilities for the dedicated server.
// Handles time progression, weather transitions, and world events.
class GameStateManager {
public:
    // ES: ── Gestión del tiempo ──
    // EN: ── Time management ──

    // ES: Avanza el reloj (deltaTime en segundos reales, escalado por gameSpeed) y el clima.
    // EN: Advances the clock (deltaTime in real seconds, scaled by gameSpeed) and the weather.
    void Update(float deltaTime, float gameSpeed);

    // ES: Hora del día normalizada [0, 1) (0 = medianoche, 0.5 = mediodía). SetTimeOfDay la envuelve a ese rango.
    // EN: Normalized time of day [0, 1) (0 = midnight, 0.5 = noon). SetTimeOfDay wraps it into that range.
    float GetTimeOfDay() const { return m_timeOfDay; }
    void  SetTimeOfDay(float time);

    // ES: Hora legible, p. ej. "Day 3, 14:30".
    // EN: Get human-readable time string (e.g., "Day 3, 14:30")
    std::string GetTimeString() const;

    // ES: true si es de día (hora normalizada entre 0.25 y 0.75).
    // EN: Check if it's currently daytime (0.25 - 0.75)
    bool IsDaytime() const;

    // ES: Número de día de partida actual (empieza en 1).
    // EN: Get the current game day number
    int GetDayNumber() const { return m_dayNumber; }

    // ES: ── Gestión del clima ──
    // EN: ── Weather management ──

    // ES: Estado de clima: 0 = despejado, 1 = nublado, 2 = tormenta de polvo, 3 = lluvia, 4 = lluvia ácida.
    // EN: Weather state: 0 = clear, 1 = cloudy, 2 = dust storm, 3 = rain, 4 = acid rain.
    int  GetWeatherState() const { return m_weatherState; }
    void SetWeatherState(int state) { m_weatherState = state; }

    // ES: Cambia el clima al azar cada m_weatherInterval segundos (llamar periódicamente).
    // EN: Advance weather randomly (call periodically)
    void UpdateWeather(float deltaTime);

    // ES: ── Eventos del mundo ──
    // EN: ── World events ──

    // ES: Detecta el paso día/noche; devuelve true solo en la llamada en que cambia.
    // EN: Check for day/night transitions (returns true on transition)
    bool CheckDayNightTransition();

    // ES: Tiempo encendido del servidor en segundos (suma de deltaTime).
    // EN: Get server uptime in seconds
    float GetUptime() const { return m_uptime; }

private:
    // ES: Estado interno: hora (0 medianoche, 0.5 mediodía), día, clima, uptime y temporizador de clima (cada ~10 min).
    // EN: Internal state: time (0 midnight, 0.5 noon), day, weather, uptime and weather timer (every ~10 min).
    float m_timeOfDay = 0.5f;     // 0.0 = midnight, 0.5 = noon
    int   m_dayNumber = 1;
    int   m_weatherState = 0;     // 0=clear, 1=cloudy, 2=dust, 3=rain, 4=acid
    float m_uptime = 0.f;
    float m_weatherTimer = 0.f;
    float m_weatherInterval = 600.f; // Weather change every ~10 minutes
    bool  m_wasDaytime = true;
};

} // namespace kmp
