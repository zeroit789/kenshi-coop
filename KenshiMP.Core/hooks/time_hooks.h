// ES: Interfaz del módulo de hooks de tiempo (hora del día y velocidad de juego).
//     Hookea "TimeUpdate" (RVA 0x214B50 en Steam 1.0.68) para capturar el puntero al
//     TimeManager del juego y poder leer/escribir la hora (+0x08) y la velocidad (+0x10)
//     que manda el servidor. OJO: en Steam el juego no llama nunca a esa función, así
//     que en la práctica el puntero no se captura (ver time_hooks.cpp).
// EN: Interface of the time hooks module (time of day and game speed).
//     Hooks "TimeUpdate" (RVA 0x214B50 on Steam 1.0.68) to capture the game's
//     TimeManager pointer so the server-driven time (+0x08) and speed (+0x10) can be
//     read/written. NOTE: on Steam the game never calls that function, so in practice
//     the pointer is never captured (see time_hooks.cpp).
#pragma once
// ES: Espacio de nombres de los hooks de tiempo.
// EN: Namespace for the time hooks.
namespace kmp::time_hooks {
    // ES: Instala el hook TimeUpdate si la función se resolvió; siempre devuelve true.
    // EN: Installs the TimeUpdate hook if the function was resolved; always returns true.
    bool Install();
    // ES: Quita el hook TimeUpdate.
    // EN: Removes the TimeUpdate hook.
    void Uninstall();
    // ES: Guarda la hora (0-1) y la velocidad recibidas del servidor y, si ya hay
    //     TimeManager capturado, las escribe directamente en su memoria.
    // EN: Stores the time of day (0-1) and speed received from the server and, if the
    //     TimeManager has been captured, writes them straight into its memory.
    void SetServerTime(float timeOfDay, float gameSpeed);

    // ES: Lectura/escritura de la hora actual a través del TimeManager capturado por el hook.
    // EN: Read current time from the captured TimeManager (hooked at runtime)
    float GetTimeOfDay();   // 0.0-1.0 (reads timeManager+0x08)
    float GetGameSpeed();   // reads timeManager+0x10
    bool  WriteTimeOfDay(float timeOfDay); // writes timeManager+0x08
    bool  HasTimeManager(); // true if TimeUpdate hook captured the pointer
}
