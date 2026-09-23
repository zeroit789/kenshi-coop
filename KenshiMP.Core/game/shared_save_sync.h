// ES: shared_save_sync.h - Modo de sincronización "partida compartida": ambos jugadores cargan la
//     misma partida y cada uno controla un personaje ya existente (encontrado por nombre según su
//     facción). Cada tick se envía la posición propia al servidor y se escribe la del otro jugador
//     en su personaje; también se sincroniza la velocidad de juego.
// EN: shared_save_sync.h - "Shared save" sync mode: both players load the same save and each one
//     controls an already existing character (found by name from its faction). Every tick the own
//     position is sent to the server and the other player's position is written to their
//     character; game speed is synced too.
#pragma once
#include "kmp/types.h"
#include <string>

namespace kmp::shared_save_sync {

// ES: Inicializar tras conectar y recibir facción: decide el nombre del personaje propio y del otro.
// EN:
// Initialize after connection + faction assignment.
// Determines own character name and other player's character name from faction.
void Init();

// ES: Reiniciar al desconectar.
// EN:
// Reset on disconnect.
void Reset();

// ES: Se llama cada tick desde Core::OnGameTick: busca los personajes por nombre, envía la
//     posición propia, aplica la del otro jugador y sincroniza la velocidad de juego.
// EN:
// Called every game tick from Core::OnGameTick.
// - Discovers characters by name via char_tracker_hooks
// - Reads own position, sends to server
// - Receives other player position, writes to their character
// - Syncs game speed
void Update(float deltaTime);

// ES: Llega del servidor la posición del otro jugador.
// EN:
// Called when we receive a position update from the server for the other player.
void OnRemotePositionReceived(const Vec3& pos);

// ES: Llega del servidor la velocidad de juego.
// EN:
// Called when we receive a game speed update from the server.
void OnRemoteGameSpeedReceived(float speed);

// ES: Consultas de estado para el HUD/diagnóstico.
// EN:
// Status queries for HUD/diagnostics
bool IsOwnCharacterFound();
bool IsOtherCharacterFound();
const std::string& GetOwnCharacterName();
const std::string& GetOtherCharacterName();

} // namespace kmp::shared_save_sync
