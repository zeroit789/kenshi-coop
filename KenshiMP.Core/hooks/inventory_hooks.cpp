#include "inventory_hooks.h"
#include "kmp/hook_manager.h"
#include "kmp/patterns.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/memory.h"
#include "kmp/safe_hook.h"
#include "../core.h"
#include "../game/game_types.h"
#include <spdlog/spdlog.h>
#include <cstring>   // memcpy / strcmp — recorrido de secciones PE del guard UAF de BuyItem
#include <Windows.h> // GetModuleHandleW / IMAGE_* para el rango de codigo del juego (guard UAF)

namespace kmp::inventory_hooks {

// ── Function typedefs ──
using ItemPickupFn = void(__fastcall*)(void* inventory, void* item, int quantity);
using ItemDropFn   = void(__fastcall*)(void* inventory, void* item);
// CORRECCIÓN crash (2026-06-18): la firma real de buyItem (RVA 0x0074A630, 1.0.68)
// verificada por RE es __fastcall de SOLO 3 punteros en orden rcx=buyer, rdx=item, r8=seller.
// La firma anterior tenía:
//   (a) un 4º param fantasma `int quantity` (r9 NUNCA se lee como entrante en la función), y
//   (b) seller e item CRUZADOS (el hook ponía seller en rdx e item en r8, al revés del binario).
// El cruce hacía que el trampoline reconstruyera la llamada con los punteros intercambiados;
// la función desreferenciaba el "item" como tienda (mov rax,[rdi]; call [rax+0x58]) -> vtable
// en offset basura -> CALL a puntero inválido -> Access Violation -> SEH -> auto-disable.
//
// CORRECCIÓN retorno + FUSIÓN guard UAF (2026-07-14): el retorno NO es `char` sino `void*`
// (puntero al item comprado, o nullptr si falla). Confirmado por RE del epílogo (un único
// `ret`, camino éxito `mov rax,rsi` con puntero de 64 bits completo, caminos fallo `xor eax,eax`)
// y por el caller nativo game+0x9A18B8 (rutina de IA de comercio 0x9A14B0, xref 'Sell_Item') que
// desreferencia el resultado como puntero con vtable (`mov r8,[rbx]` en 0x9A18DA → `call [r8+0x290]`).
// Declarar `char` TRUNCABA el puntero de 64 bits a 1 byte al devolver desde el detour → el caller
// nativo recibía un puntero basura y lo desreferenciaba → crash determinista en game+0x9A18DA.
// Además, aquí se FUSIONA el guard UAF (antes en combat_hooks.cpp como Hook_BuyItemUafGuard, que
// colisionaba con este hook sobre la MISMA RVA — MinHook deduplica por dirección y rechazaba el
// segundo MH_CreateHook, dejando uno de los dos SIN instalar en silencio).
using BuyItemFn    = void*(__fastcall*)(void* buyer, void* item, void* seller);

// ── State ──
static ItemPickupFn s_origItemPickup = nullptr;
static ItemDropFn   s_origItemDrop   = nullptr;
static BuyItemFn    s_origBuyItem    = nullptr;
static int s_pickupCount = 0;
static int s_dropCount = 0;
static int s_buyCount = 0;
static bool s_loading = false;

// ── HookHealth tracking (auto-disables trampoline on crash) ──
static HookHealth s_pickupHealth{"ItemPickup"};
static HookHealth s_dropHealth{"ItemDrop"};
static HookHealth s_buyHealth{"BuyItem"};

// ── SEH wrappers using SafeCall pattern (handles MovRaxRsp trampolines safely) ──

// void fn(void*, void*, int) — ItemPickup
static bool SEH_ItemPickup(void* inventory, void* item, int quantity) {
    return SafeCall_Void_PtrPtrI(reinterpret_cast<void*>(s_origItemPickup),
                                  inventory, item, quantity, &s_pickupHealth);
}

// void fn(void*, void*) — ItemDrop
static bool SEH_ItemDrop(void* inventory, void* item) {
    return SafeCall_Void_PtrPtr(reinterpret_cast<void*>(s_origItemDrop),
                                 inventory, item, &s_dropHealth);
}

// ── Guard UAF de BuyItem (fusionado desde combat_hooks.cpp, 2026-07-14) ──
// El original de BuyItem devuelve un puntero al item comprado (o nullptr si falla). El caller
// nativo de la IA de comercio (game+0x9A18DA) desreferencia ese puntero como objeto con vtable.
// Si BuyItem devuelve un slot RECICLADO por el RootObjectFactory (puntero no-null pero muerto),
// la desreferencia de su vtable revienta. El guard valida el puntero devuelto ANTES de que el
// caller lo use: exige que sea un heap-ptr plausible y que su vtable caiga en el rango de código
// del JUEGO ([.text, .rdata) de kenshi_x64.exe). Si no, lo tratamos como nullptr (el caller ya
// sabe manejar null: solo hacía test/jz antes de desreferenciar).

// Rango de código del JUEGO (kenshi_x64.exe): [inicio de .text, fin de .rdata). Una vtable válida
// de un objeto del juego SIEMPRE cae aquí; la de un slot reciclado/basura NO. Se calcula una vez
// en InitGameCodeRangeForBuyItem() (llamada desde Install()).
static uintptr_t s_gameTextLo = 0, s_gameRdataHi = 0;

// Calcula [.text, .rdata) del EXE principal recorriendo sus secciones PE.
static void InitGameCodeRangeForBuyItem() {
    HMODULE h = GetModuleHandleW(nullptr); // el propio kenshi_x64.exe (proceso principal, no el mod)
    auto base = reinterpret_cast<uintptr_t>(h);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec  = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {0}; memcpy(name, sec[i].Name, 8);
        uintptr_t lo = base + sec[i].VirtualAddress;
        uintptr_t hi = lo + sec[i].Misc.VirtualSize;
        if (!strcmp(name, ".text"))  s_gameTextLo  = lo;
        if (!strcmp(name, ".rdata")) s_gameRdataHi = hi;
    }
    // Fallbacks defensivos si el PE no expone esas secciones con esos nombres exactos.
    if (!s_gameTextLo)  s_gameTextLo  = base + 0x1000;
    if (!s_gameRdataHi) s_gameRdataHi = base + 0x1673000 + 0x54A4CB;
}

// ¿'v' parece un puntero de heap plausible? (rango user-mode + alineado a 8 bytes)
static inline bool IsHeapPtrForBuyItem(uintptr_t v) {
    if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false; // fuera del rango user-mode
    if ((v & 0x7) != 0) return false;                          // no alineado a puntero
    return true;
}

// ── Helpers ──

// Best-effort item template ID extraction from item pointer
static uint32_t TryGetItemTemplateId(void* item) {
    if (!item) return 0;
    static const game::ItemOffsets offsets;
    uint32_t templateId = 0;
    if (Memory::Read(reinterpret_cast<uintptr_t>(item) + offsets.templateId, templateId)) {
        return templateId;
    }
    return 0;
}

// ── Hooks ──

static void __fastcall Hook_ItemPickup(void* inventory, void* item, int quantity) {
    s_pickupCount++;

    if (!SEH_ItemPickup(inventory, item, quantity)) {
        if (s_pickupHealth.trampolineFailed.load()) {
            spdlog::error("inventory_hooks: ItemPickup trampoline CRASHED — hook auto-disabled");
        }
        return;
    }

    if (s_loading) return;

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    // Registry maps CHARACTER pointers, not inventory pointers.
    // Lee el dueño del inventario en inventory+0x88 (InventoryOffsets::owner).
    // CORRECCIÓN audit-02 (2026-06-18): antes el comentario decía +0x28 (offset INCORRECTO);
    // el owner real está en +0x88 según KenshiLib. El valor se toma de GetOffsets() (dinámico),
    // así que ya usa el +0x88 corregido en game_types.h.
    auto& registry = core.GetEntityRegistry();
    const int ownerOff = game::GetOffsets().inventory.owner;
    if (ownerOff < 0) return; // Offset not resolved
    uintptr_t ownerPtr = 0;
    Memory::Read(reinterpret_cast<uintptr_t>(inventory) + ownerOff, ownerPtr);
    void* owner = reinterpret_cast<void*>(ownerPtr);
    EntityID netId = (owner != nullptr) ? registry.GetNetId(owner) : INVALID_ENTITY;
    if (netId == INVALID_ENTITY) {
        if (s_pickupCount % 100 == 1) {
            spdlog::debug("inventory_hooks: ItemPickup #{} (inv=0x{:X}, owner=0x{:X}, not tracked)",
                           s_pickupCount, (uintptr_t)inventory, ownerPtr);
        }
        return;
    }

    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_ItemPickup);
    MsgItemPickup msg{};
    msg.entityId = netId;
    msg.itemTemplateId = TryGetItemTemplateId(item);
    msg.quantity = quantity;
    writer.WriteRaw(&msg, sizeof(msg));
    core.GetClient().SendReliable(writer.Data(), writer.Size());

    spdlog::debug("inventory_hooks: ItemPickup #{} sent (entity={}, qty={})",
                   s_pickupCount, netId, quantity);
}

static void __fastcall Hook_ItemDrop(void* inventory, void* item) {
    s_dropCount++;

    if (!SEH_ItemDrop(inventory, item)) {
        if (s_dropHealth.trampolineFailed.load()) {
            spdlog::error("inventory_hooks: ItemDrop trampoline CRASHED — hook auto-disabled");
        }
        return;
    }

    if (s_loading) return;

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    // Registry maps CHARACTER pointers, not inventory pointers.
    auto& registry = core.GetEntityRegistry();
    const int ownerOff = game::GetOffsets().inventory.owner;
    if (ownerOff < 0) return; // Offset not resolved
    uintptr_t ownerPtr = 0;
    Memory::Read(reinterpret_cast<uintptr_t>(inventory) + ownerOff, ownerPtr);
    void* owner = reinterpret_cast<void*>(ownerPtr);
    EntityID netId = (owner != nullptr) ? registry.GetNetId(owner) : INVALID_ENTITY;
    if (netId == INVALID_ENTITY) return;

    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_ItemDrop);
    MsgItemDrop msg{};
    msg.entityId = netId;
    msg.itemTemplateId = TryGetItemTemplateId(item);
    msg.posX = msg.posY = msg.posZ = 0.f;
    writer.WriteRaw(&msg, sizeof(msg));
    core.GetClient().SendReliable(writer.Data(), writer.Size());

    spdlog::debug("inventory_hooks: ItemDrop #{} sent (entity={})", s_dropCount, netId);
}

// Firma corregida a 3 punteros en orden real del binario: (buyer=rcx, item=rdx, seller=r8) y
// retorno void* (puntero al item comprado, o nullptr si falla). MinHook desvía la función
// original aquí, así que la firma del detour DEBE coincidir EXACTAMENTE con la del juego o los
// argumentos/retorno se reciben/propagan corruptos.
//
// Hook FUSIONADO (2026-07-14): combina en un solo detour sobre 0x74A630
//   (1) el guard UAF que valida el puntero devuelto (antes en combat_hooks.cpp), y
//   (2) la sincronización de red de la compra (C2S_TradeRequest) — lógica original de este fichero.
// Fusionado porque MinHook deduplica por dirección: dos hooks sobre la misma RVA hacían que el
// segundo MH_CreateHook se rechazara en silencio y uno de los dos quedara sin instalar.
static void* __fastcall Hook_BuyItem(void* buyer, void* item, void* seller) {
    s_buyCount++;

    // ── Capa 1: llamar al original bajo SEH con la firma EXACTA (buyer, item, seller) ──
    // SafeCall_Ptr_PtrPtrPtr envuelve el trampoline en __try/__except: si la función original
    // revienta DENTRO (incluida su desreferencia interna de la vtable del resultado en 0x74A6F5),
    // el except marca s_buyHealth y devuelve nullptr. Reutiliza el mecanismo SEH del proyecto
    // (kmp/safe_hook.h) en vez de duplicar un __try propio.
    void* res = SafeCall_Ptr_PtrPtrPtr(reinterpret_cast<void*>(s_origBuyItem),
                                       buyer, item, seller, &s_buyHealth);

    // Si el trampoline está marcado como fallido (crasheó ahora o en una llamada previa), el hook
    // queda auto-deshabilitado: no hay compra real que propagar ni sincronizar → devolvemos nullptr.
    if (s_buyHealth.trampolineFailed.load()) {
        static bool s_loggedBuyCrash = false;  // one-shot: no spamear el log en cada llamada
        if (!s_loggedBuyCrash) {
            s_loggedBuyCrash = true;
            spdlog::error("inventory_hooks: BuyItem trampoline CRASHED — hook auto-disabled, sync desactivada");
        }
        return nullptr;
    }

    // ── Capa 2: guard UAF del puntero devuelto ──
    // Solo aplica si res != nullptr (res == nullptr ya es "compra falló" legítima). Validamos ANTES
    // de que el caller nativo de la IA de comercio (game+0x9A18DA) lo desreferencie como objeto con
    // vtable. Si el puntero es basura/reciclado → devolvemos nullptr SIN sincronizar (una compra
    // sobre un objeto reciclado no es una compra real que valga la pena replicar por red).
    if (res != nullptr) {
        auto p = reinterpret_cast<uintptr_t>(res);
        // 2a: ¿es siquiera un puntero de heap plausible (rango user-mode + alineado a 8)?
        if (!IsHeapPtrForBuyItem(p)) return nullptr;
        // 2b: leer la vtable (primer qword) SEH-safe y exigir que caiga en el rango de código del
        // JUEGO. Un slot reciclado conserva un puntero pero su vtable ya no cae en [.text,.rdata).
        uintptr_t vtbl = 0;
        if (!Memory::Read(p + 0x0, vtbl)) return nullptr;
        if (vtbl < s_gameTextLo || vtbl >= s_gameRdataHi) return nullptr;
    }

    // ── res es un puntero GENUINO (o nullptr legítimo = compra falló): sincronización de red ──
    // A partir de aquí la lógica de sync es la ORIGINAL de este fichero, sin cambios de comportamiento.
    if (s_loading) return res;

    auto& core = Core::Get();
    if (!core.IsConnected()) return res;

    auto& registry = core.GetEntityRegistry();
    EntityID buyerNetId = registry.GetNetId(buyer);
    if (buyerNetId == INVALID_ENTITY) return res;

    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_TradeRequest);
    MsgTradeRequest msg{};
    msg.buyerEntityId = buyerNetId;
    msg.sellerEntityId = registry.GetNetId(seller);
    msg.itemTemplateId = TryGetItemTemplateId(item);
    // La función del juego no recibe cantidad como parámetro (compra unitaria por defecto
    // desde la UI de tienda). Enviamos 1; si más adelante se necesita la cantidad real,
    // habrá que leerla del estado de la UI de comercio, no de la firma de buyItem.
    msg.quantity = 1;
    msg.price = 0;
    writer.WriteRaw(&msg, sizeof(msg));
    core.GetClient().SendReliable(writer.Data(), writer.Size());

    spdlog::info("inventory_hooks: BuyItem #{} sent (buyer={})",
                  s_buyCount, buyerNetId);

    // Propagamos el puntero de retorno REAL (ya validado) de la función original sin modificar,
    // para no alterar la semántica que el juego espera (el caller de la IA lo desreferencia).
    return res;
}

// ── Install / Uninstall ──

bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hooks = HookManager::Get();
    int installed = 0;

    if (funcs.ItemPickup) {
        if (hooks.InstallAt("ItemPickup", reinterpret_cast<uintptr_t>(funcs.ItemPickup),
                            &Hook_ItemPickup, &s_origItemPickup)) {
            installed++;
            spdlog::info("inventory_hooks: ItemPickup hook installed");
        }
    }

    if (funcs.ItemDrop) {
        if (hooks.InstallAt("ItemDrop", reinterpret_cast<uintptr_t>(funcs.ItemDrop),
                            &Hook_ItemDrop, &s_origItemDrop)) {
            installed++;
            spdlog::info("inventory_hooks: ItemDrop hook installed");
        }
    }

    if (funcs.BuyItem) {
        // Calcula el rango de código del juego [.text,.rdata) que usa el guard UAF fusionado
        // para validar la vtable del puntero devuelto por BuyItem (ver Hook_BuyItem, capa 2b).
        InitGameCodeRangeForBuyItem();
        if (hooks.InstallAt("BuyItem", reinterpret_cast<uintptr_t>(funcs.BuyItem),
                            &Hook_BuyItem, &s_origBuyItem)) {
            installed++;
            spdlog::info("inventory_hooks: BuyItem hook installed (con guard UAF fusionado) — "
                         "rango codigo juego [.text=0x{:X}, .rdata_fin=0x{:X})",
                         s_gameTextLo, s_gameRdataHi);
        }
    }

    spdlog::info("inventory_hooks: {}/3 hooks installed", installed);
    return installed > 0;
}

void Uninstall() {
    auto& hooks = HookManager::Get();
    if (s_origItemPickup) hooks.Remove("ItemPickup");
    if (s_origItemDrop)   hooks.Remove("ItemDrop");
    if (s_origBuyItem)    hooks.Remove("BuyItem");
    s_origItemPickup = nullptr;
    s_origItemDrop = nullptr;
    s_origBuyItem = nullptr;
}

void SetLoading(bool loading) {
    s_loading = loading;
}

} // namespace kmp::inventory_hooks
