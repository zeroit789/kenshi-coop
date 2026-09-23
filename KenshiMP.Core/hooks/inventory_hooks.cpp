// ES: Implementación de los hooks de inventario. Los tres detours llaman primero al
//     original con SafeCall (SEH + HookHealth, que desactiva el hook si el trampolín crashea)
//     y después, si hay conexión y no se está cargando, traducen el puntero del juego a un
//     netId y envían el paquete fiable correspondiente. Corren en el hilo de lógica del juego.
// EN: Implementation of the inventory hooks. The three detours first call the original via
//     SafeCall (SEH + HookHealth, which disables the hook if the trampoline crashes) and then,
//     when connected and not loading, translate the game pointer to a netId and send the
//     matching reliable packet. They run on the game logic thread.
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

// ES: Firmas: ItemPickup(inventario, objeto, cantidad) e ItemDrop(inventario, objeto).
// EN: Signatures: ItemPickup(inventory, item, quantity) and ItemDrop(inventory, item).
// ── Function typedefs ──
using ItemPickupFn = void(__fastcall*)(void* inventory, void* item, int quantity);
using ItemDropFn   = void(__fastcall*)(void* inventory, void* item);
// CORRECCIÓN crash (2026-06-18): la firma real de buyItem (RVA 0x0074A630, 1.0.68)
// verificada por RE es __fastcall de SOLO 3 punteros en orden rcx=buyer, rdx=item, r8=seller.
// EN: CRASH FIX (2026-06-18): the real buyItem signature (RVA 0x0074A630, 1.0.68), verified
//     by RE, is __fastcall with ONLY 3 pointers in order rcx=buyer, rdx=item, r8=seller.
// La firma anterior tenía:
//   (a) un 4º param fantasma `int quantity` (r9 NUNCA se lee como entrante en la función), y
//   (b) seller e item CRUZADOS (el hook ponía seller en rdx e item en r8, al revés del binario).
// El cruce hacía que el trampoline reconstruyera la llamada con los punteros intercambiados;
// la función desreferenciaba el "item" como tienda (mov rax,[rdi]; call [rax+0x58]) -> vtable
// en offset basura -> CALL a puntero inválido -> Access Violation -> SEH -> auto-disable.
//
// EN: The previous signature had (a) a phantom 4th param `int quantity` (r9 is NEVER read as
//     an incoming value) and (b) seller and item SWAPPED. The swap made the trampoline rebuild
//     the call with the pointers exchanged; the function dereferenced the "item" as a shop
//     (mov rax,[rdi]; call [rax+0x58]) -> vtable at a garbage offset -> CALL to an invalid
//     pointer -> Access Violation -> SEH -> auto-disable.
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
// EN: RETURN FIX + UAF GUARD MERGE (2026-07-14): the return type is not `char` but `void*`
//     (pointer to the purchased item, or nullptr on failure). Confirmed by RE of the epilogue
//     (single `ret`, success path `mov rax,rsi` with a full 64-bit pointer, failure paths
//     `xor eax,eax`) and by the native caller game+0x9A18B8 (trade AI routine 0x9A14B0, xref
//     'Sell_Item') which dereferences the result as a vtable object (`mov r8,[rbx]` at
//     0x9A18DA -> `call [r8+0x290]`). Declaring `char` TRUNCATED the pointer to 1 byte when
//     returning from the detour -> deterministic crash at game+0x9A18DA. The UAF guard is also
//     MERGED here (previously Hook_BuyItemUafGuard in combat_hooks.cpp, which collided with
//     this hook on the SAME RVA — MinHook dedups by address and rejected the second
//     MH_CreateHook, silently leaving one of them uninstalled).
using BuyItemFn    = void*(__fastcall*)(void* buyer, void* item, void* seller);

// ES: Estado: trampolines, contadores de llamadas y bandera de carga (no atómica).
// EN: State: trampolines, call counters and loading flag (not atomic).
// ── State ──
static ItemPickupFn s_origItemPickup = nullptr;
static ItemDropFn   s_origItemDrop   = nullptr;
static BuyItemFn    s_origBuyItem    = nullptr;
static int s_pickupCount = 0;
static int s_dropCount = 0;
static int s_buyCount = 0;
static bool s_loading = false;

// ES: Salud de cada hook (desactiva el trampolín si crashea).
// ── HookHealth tracking (auto-disables trampoline on crash) ──
static HookHealth s_pickupHealth{"ItemPickup"};
static HookHealth s_dropHealth{"ItemDrop"};
static HookHealth s_buyHealth{"BuyItem"};

// ES: Envoltorios SEH con el patrón SafeCall (maneja bien los trampolines MovRaxRsp).
// ── SEH wrappers using SafeCall pattern (handles MovRaxRsp trampolines safely) ──

// ES: Llamada protegida al ItemPickup original.
// void fn(void*, void*, int) — ItemPickup
static bool SEH_ItemPickup(void* inventory, void* item, int quantity) {
    return SafeCall_Void_PtrPtrI(reinterpret_cast<void*>(s_origItemPickup),
                                  inventory, item, quantity, &s_pickupHealth);
}

// ES: Llamada protegida al ItemDrop original.
// void fn(void*, void*) — ItemDrop
static bool SEH_ItemDrop(void* inventory, void* item) {
    return SafeCall_Void_PtrPtr(reinterpret_cast<void*>(s_origItemDrop),
                                 inventory, item, &s_dropHealth);
}

// ── Guard UAF de BuyItem (fusionado desde combat_hooks.cpp, 2026-07-14) ──
// EN: BuyItem UAF guard (merged from combat_hooks.cpp, 2026-07-14). The original returns a
//     pointer to the purchased item (or nullptr). The native trade AI caller (game+0x9A18DA)
//     dereferences it as a vtable object. If BuyItem returns a slot RECYCLED by the
//     RootObjectFactory (non-null but dead), that vtable dereference blows up. The guard
//     validates the returned pointer BEFORE the caller uses it: it must be a plausible heap
//     pointer whose vtable lies in the GAME's code range ([.text, .rdata) of kenshi_x64.exe).
//     Otherwise it is treated as nullptr (the caller already handles null with test/jz).
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
// EN: GAME code range (kenshi_x64.exe): [start of .text, end of .rdata). A valid game object
//     vtable ALWAYS lies here; a recycled/garbage slot's does not. Computed once in
//     InitGameCodeRangeForBuyItem() (called from Install()).
static uintptr_t s_gameTextLo = 0, s_gameRdataHi = 0;

// Calcula [.text, .rdata) del EXE principal recorriendo sus secciones PE.
// EN: Computes [.text, .rdata) of the main EXE by walking its PE sections. The fallbacks use
//     hard-coded section offsets from Steam 1.0.68 (probably; not verified for other builds).
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
// EN: Does 'v' look like a plausible heap pointer? (user-mode range + 8-byte aligned)
static inline bool IsHeapPtrForBuyItem(uintptr_t v) {
    if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false; // fuera del rango user-mode
    if ((v & 0x7) != 0) return false;                          // no alineado a puntero
    return true;
}

// ── Helpers ──

// ES: Extrae (si puede) el id de plantilla del objeto leyendo item+ItemOffsets::templateId.
//     OJO: usa un ItemOffsets por defecto, no GetOffsets() como el resto del fichero.
// EN: Note: it uses a default-constructed ItemOffsets, not GetOffsets() like the rest of the file.
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

// ES: Detour de ItemPickup (Inventory::addItem). Llama al original; luego, si hay conexión
//     y no se carga, lee el dueño del inventario, obtiene su netId y envía C2S_ItemPickup
//     con plantilla y cantidad. Si el dueño no está registrado, no envía nada.
// EN: ItemPickup detour (Inventory::addItem). Calls the original; then, when connected and
//     not loading, reads the inventory owner, gets its netId and sends C2S_ItemPickup with
//     template and quantity. If the owner is not registered, nothing is sent.
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

    // ES: El registro mapea punteros de PERSONAJE, no de inventario.
    // Registry maps CHARACTER pointers, not inventory pointers.
    // Lee el dueño del inventario en inventory+0x88 (InventoryOffsets::owner).
    // CORRECCIÓN audit-02 (2026-06-18): antes el comentario decía +0x28 (offset INCORRECTO);
    // el owner real está en +0x88 según KenshiLib. El valor se toma de GetOffsets() (dinámico),
    // así que ya usa el +0x88 corregido en game_types.h.
    // EN: Reads the inventory owner at inventory+0x88 (InventoryOffsets::owner). audit-02 FIX
    //     (2026-06-18): the old comment said +0x28 (WRONG offset); the real owner is at +0x88
    //     per KenshiLib. The value comes from GetOffsets() (dynamic), so it already uses the
    //     corrected +0x88 from game_types.h.
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

// ES: Detour de ItemDrop (Inventory::removeItem). Mismo patrón: original, dueño -> netId y
//     envío de C2S_ItemDrop (posición a 0, no se conoce aquí).
// EN: ItemDrop detour (Inventory::removeItem). Same pattern: original, owner -> netId and a
//     C2S_ItemDrop send (position set to 0, unknown here).
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

    // ES: El registro mapea punteros de PERSONAJE, no de inventario: hay que leer el dueño.
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
// EN: Signature fixed to 3 pointers in the binary's real order (buyer=rcx, item=rdx,
//     seller=r8) returning void* (purchased item or nullptr). MinHook redirects the original
//     here, so the detour signature MUST match the game's EXACTLY or arguments/return value
//     get corrupted.
// Hook FUSIONADO (2026-07-14): combina en un solo detour sobre 0x74A630
//   (1) el guard UAF que valida el puntero devuelto (antes en combat_hooks.cpp), y
//   (2) la sincronización de red de la compra (C2S_TradeRequest) — lógica original de este fichero.
// Fusionado porque MinHook deduplica por dirección: dos hooks sobre la misma RVA hacían que el
// segundo MH_CreateHook se rechazara en silencio y uno de los dos quedara sin instalar.
// EN: MERGED hook (2026-07-14): a single detour on 0x74A630 combining (1) the UAF guard that
//     validates the returned pointer (formerly in combat_hooks.cpp) and (2) the network sync
//     of the purchase (C2S_TradeRequest). Merged because MinHook dedups by address.
static void* __fastcall Hook_BuyItem(void* buyer, void* item, void* seller) {
    s_buyCount++;

    // ── Capa 1: llamar al original bajo SEH con la firma EXACTA (buyer, item, seller) ──
    // SafeCall_Ptr_PtrPtrPtr envuelve el trampoline en __try/__except: si la función original
    // revienta DENTRO (incluida su desreferencia interna de la vtable del resultado en 0x74A6F5),
    // el except marca s_buyHealth y devuelve nullptr. Reutiliza el mecanismo SEH del proyecto
    // (kmp/safe_hook.h) en vez de duplicar un __try propio.
    // EN: Layer 1: call the original under SEH with the EXACT signature. SafeCall_Ptr_PtrPtrPtr
    //     wraps the trampoline in __try/__except: if the original crashes INSIDE (including its
    //     internal vtable dereference of the result at 0x74A6F5), s_buyHealth is flagged and
    //     nullptr is returned.
    void* res = SafeCall_Ptr_PtrPtrPtr(reinterpret_cast<void*>(s_origBuyItem),
                                       buyer, item, seller, &s_buyHealth);

    // Si el trampoline está marcado como fallido (crasheó ahora o en una llamada previa), el hook
    // queda auto-deshabilitado: no hay compra real que propagar ni sincronizar → devolvemos nullptr.
    // EN: If the trampoline is flagged as failed (now or earlier) the hook is auto-disabled:
    //     there is no real purchase to propagate or sync, so nullptr is returned.
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
    // EN: Layer 2: UAF guard on the returned pointer. Only when res != nullptr (nullptr is a
    //     legitimate "purchase failed"). If the pointer is garbage/recycled, return nullptr
    //     WITHOUT syncing. 2a: plausible heap pointer? 2b: read the vtable SEH-safe and require
    //     it to lie in the game's code range.
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
    // EN: From here on, the ORIGINAL sync logic of this file: when connected and the buyer has a
    //     netId, send a reliable C2S_TradeRequest.
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
    // EN: The game function takes no quantity (single-unit purchase from the shop UI by
    //     default), so 1 is sent; the real quantity would have to be read from the trade UI state.
    msg.quantity = 1;
    msg.price = 0;
    writer.WriteRaw(&msg, sizeof(msg));
    core.GetClient().SendReliable(writer.Data(), writer.Size());

    spdlog::info("inventory_hooks: BuyItem #{} sent (buyer={})",
                  s_buyCount, buyerNetId);

    // Propagamos el puntero de retorno REAL (ya validado) de la función original sin modificar,
    // para no alterar la semántica que el juego espera (el caller de la IA lo desreferencia).
    // EN: Propagate the REAL (already validated) return pointer unchanged, so the game's
    //     expected semantics are preserved (the AI caller dereferences it).
    return res;
}

// ── Install / Uninstall ──

// ES: Instala ItemPickup, ItemDrop y BuyItem en las direcciones del escáner. Antes de
//     BuyItem calcula el rango de código del juego que usa su guard UAF.
// EN: Installs ItemPickup, ItemDrop and BuyItem at the scanner addresses. Before BuyItem it
//     computes the game code range its UAF guard uses.
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
        // EN: Computes the game code range [.text,.rdata) used by the merged UAF guard to validate
        //     the vtable of the pointer returned by BuyItem (see Hook_BuyItem, layer 2b).
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

// ES: Quita los hooks instalados y olvida los trampolines.
// EN: Removes installed hooks and forgets the trampolines.
void Uninstall() {
    auto& hooks = HookManager::Get();
    if (s_origItemPickup) hooks.Remove("ItemPickup");
    if (s_origItemDrop)   hooks.Remove("ItemDrop");
    if (s_origBuyItem)    hooks.Remove("BuyItem");
    s_origItemPickup = nullptr;
    s_origItemDrop = nullptr;
    s_origBuyItem = nullptr;
}

// ES: Activa/desactiva la supresión de envíos durante la carga.
// EN: Enables/disables send suppression during loading.
void SetLoading(bool loading) {
    s_loading = loading;
}

} // namespace kmp::inventory_hooks
