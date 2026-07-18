#pragma once
// SEH-protected trampoline call wrappers.
//
// Each hook's trampoline (original function) call is wrapped in __try/__except
// so that if the function signature is wrong (wrong parameter count, wrong types),
// the game doesn't crash — instead we catch the exception, log it, and return
// a safe default.
//
// MSVC restriction: __try/__except cannot be used in functions that contain
// C++ objects with destructors. These wrapper functions use only C types.

#include <cstdint>
#include <cstdio>
#include <atomic>
#include <Windows.h>

namespace kmp {

// Per-hook health status. If a hook's trampoline crashes, it gets marked
// as failed and the hook stops calling the trampoline.
//
// [FIX-AGREGADO 2026-07] Constructor explícito OBLIGATORIO: sin él, esto era un
// AGREGADO en C++17 y `static HookHealth h{"Nombre"};` inicializaba el PRIMER
// miembro (trampolineFailed) con el puntero al string (puntero→bool = true),
// dejando name vacío. Resultado: los 12 hooks nacían con trampolineFailed=true
// y los guards de SafeCall_* cortaban SIEMPRE sin llamar jamás al original del
// juego (confirmado en vivo: failCount=0, name=""). El constructor elimina la
// propiedad de agregado (a propósito) y enruta el string a `name`.
struct HookHealth {
    std::atomic<bool> trampolineFailed{false};
    std::atomic<int>  failCount{0};
    const char* name = "";
    std::atomic<bool> guardLogged{false}; // one-shot: ya se logueó el corte del guard

    HookHealth() = default;
    explicit HookHealth(const char* n) : name(n) {}
};

// [HARDENING 2026-07] Guard de los SafeCall_*: devuelve true si el hook está
// marcado como fallido (el guard corta y NO se llama al original). Loguea UNA
// sola vez por hook la primera vez que corta — antes cortaba en silencio total,
// lo que mantuvo invisible el bug del agregado durante 3+ semanas. Sin heap ni
// objetos C++ con destructor (seguro de llamar desde funciones con SEH).
inline bool HookGuardTripped(HookHealth* health) {
    if (!health || !health->trampolineFailed.load()) return false;
    bool expected = false;
    if (health->guardLogged.compare_exchange_strong(expected, true)) {
        char buf[192];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "[KMP][SafeHook] guard CORTA el hook '%s' (trampolineFailed=1, failCount=%d) — "
                    "el original del juego NO se llama\n",
                    (health->name && health->name[0]) ? health->name : "<sin nombre>",
                    health->failCount.load());
        OutputDebugStringA(buf);
    }
    return true;
}

// ── Safe call wrappers for different function signatures ──
// These are __declspec(noinline) C-style functions that wrap trampoline calls
// in SEH. They must NOT contain any C++ objects with destructors.

// void fn(void*)
__declspec(noinline)
inline bool SafeCall_Void_Ptr(void* fn, void* a1, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*);
    __try {
        reinterpret_cast<Fn>(fn)(a1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void* fn(void*, void*) — CharacterCreate
__declspec(noinline)
inline void* SafeCall_Ptr_PtrPtr(void* fn, void* a1, void* a2, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return nullptr;
    using Fn = void*(__fastcall*)(void*, void*);
    __try {
        return reinterpret_cast<Fn>(fn)(a1, a2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return nullptr;
    }
}

// void* fn(void*, void*, void*) — resolvedores de 3 punteros que DEVUELVEN puntero
// (guard del retorno de BuyItem / FIX-UAF-BUYITEM). Si el original revienta DENTRO (p.ej.
// al desreferenciar la vtable de un objeto reciclado antes de devolverlo), el except
// devuelve nullptr — un retorno seguro que el caller del juego ya sabe tratar.
__declspec(noinline)
inline void* SafeCall_Ptr_PtrPtrPtr(void* fn, void* a1, void* a2, void* a3, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return nullptr;
    using Fn = void*(__fastcall*)(void*, void*, void*);
    __try {
        return reinterpret_cast<Fn>(fn)(a1, a2, a3);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return nullptr;
    }
}

// void fn(void*, float, float, float) — SetPosition
__declspec(noinline)
inline bool SafeCall_Void_PtrFFF(void* fn, void* a1, float a2, float a3, float a4, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, float, float, float);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3, a4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void fn(void*, float) — CombatClass::update(float) (DIAG-COMBATSEED)
// El this va en rcx (a1) y el dt (float) en xmm1 (a2); el trampolín estándar de MinHook
// preserva xmm1 (la función 0x60D650 tiene prólogo limpio, sin el fix MovRaxRsp).
__declspec(noinline)
inline bool SafeCall_Void_PtrF(void* fn, void* a1, float a2, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, float);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void fn(void*, float, float, float, int) — MoveTo
__declspec(noinline)
inline bool SafeCall_Void_PtrFFFI(void* fn, void* a1, float a2, float a3, float a4, int a5, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, float, float, float, int);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3, a4, a5);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void fn(void*, void*, int, float, float, float) — ApplyDamage
__declspec(noinline)
inline bool SafeCall_Void_PtrPtrIFFF(void* fn, void* a1, void* a2, int a3,
                                       float a4, float a5, float a6, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, void*, int, float, float, float);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3, a4, a5, a6);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void fn(void*, void*) — CharacterDeath
__declspec(noinline)
inline bool SafeCall_Void_PtrPtr(void* fn, void* a1, void* a2, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, void*);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void fn(void*, void*, void*) — StartAttack(attacker, target, weapon)
__declspec(noinline)
inline bool SafeCall_Void_PtrPtrPtr(void* fn, void* a1, void* a2, void* a3, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, void*, void*);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void fn(void*, int, int) — ZoneLoad/ZoneUnload
__declspec(noinline)
inline bool SafeCall_Void_PtrII(void* fn, void* a1, int a2, int a3, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, int, int);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void fn(void*, void*, float, float, float) — BuildingPlace
__declspec(noinline)
inline bool SafeCall_Void_PtrPtrFFF(void* fn, void* a1, void* a2,
                                      float a3, float a4, float a5, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, void*, float, float, float);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3, a4, a5);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void fn(void*, const char*) — SaveGame/LoadGame
__declspec(noinline)
inline bool SafeCall_Void_PtrStr(void* fn, void* a1, const char* a2, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, const char*);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// bool fn(void*, int, void*) — AddOrderBackend (validador de órdenes 0x5D1940)
// Devuelve en *outRet el bool del original (true = orden ABORTADA/tragada,
// false = orden continúa). Si el trampoline falla, deja *outRet = false para que
// el caller del juego siga el pipeline normal (degradación segura: sin validación,
// pero sin tragar la orden ni crashear).
__declspec(noinline)
inline bool SafeCall_Bool_PtrIPtr(void* fn, void* a1, int a2, void* a3,
                                    bool* outRet, HookHealth* health) {
    if (outRet) *outRet = false;
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = bool(__fastcall*)(void*, int, void*);
    __try {
        bool r = reinterpret_cast<Fn>(fn)(a1, a2, a3);
        if (outRet) *outRet = r;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

// void fn(void*, void*, int) — CharacterKO
__declspec(noinline)
inline bool SafeCall_Void_PtrPtrI(void* fn, void* a1, void* a2, int a3, HookHealth* health) {
    if (!fn || HookGuardTripped(health)) return false;
    using Fn = void(__fastcall*)(void*, void*, int);
    __try {
        reinterpret_cast<Fn>(fn)(a1, a2, a3);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (health) {
            health->trampolineFailed.store(true);
            health->failCount.fetch_add(1);
        }
        return false;
    }
}

} // namespace kmp
