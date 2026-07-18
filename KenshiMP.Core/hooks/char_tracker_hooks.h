#pragma once
#include "kmp/types.h"
#include <string>
#include <functional>

namespace kmp::char_tracker_hooks {

bool Install();
void Uninstall();

struct TrackedChar {
    void* animClassPtr;     // AnimationClassHuman*
    void* characterPtr;     // CharacterHuman* (at animClass+0x2D8)
    std::string name;
    Vec3 position;
    uint64_t lastSeenTick;
};

const TrackedChar* FindByName(const std::string& name);
const TrackedChar* FindByPtr(void* characterPtr);

// Purga una entrada concreta del tracker (llamado desde el destroy-hook del motor).
void RemoveByPtr(void* ptr);
// Vacía todo el tracker (llamado en desconexión / recarga de save en caliente).
void Clear();
void* GetLocalPlayerAnimClass();
void* GetRemotePlayerAnimClass(const std::string& name);
void SetOnNewCharacter(std::function<void(const TrackedChar&)> callback);
int GetTrackedCount();
void DumpTrackedChars();

// Process deferred character discoveries from safe game-tick context.
// Called from Core::OnGameTick — NOT from inside the inline hook.
void ProcessDeferredDiscovery();

} // namespace kmp::char_tracker_hooks
