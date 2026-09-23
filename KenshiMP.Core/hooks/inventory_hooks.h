// ES: Interfaz de los hooks de inventario y comercio: ItemPickup (Inventory::addItem, RVA
//     0x74C8B0), ItemDrop (Inventory::removeItem, RVA 0x745DE0) y BuyItem (compra en tienda,
//     RVA 0x74A630). Envían C2S_ItemPickup / C2S_ItemDrop / C2S_TradeRequest cuando cambia el
//     inventario de un personaje registrado; BuyItem además protege frente a punteros reciclados.
// EN: Interface of the inventory and trade hooks: ItemPickup (Inventory::addItem, RVA
//     0x74C8B0), ItemDrop (Inventory::removeItem, RVA 0x745DE0) and BuyItem (shop purchase,
//     RVA 0x74A630). They send C2S_ItemPickup / C2S_ItemDrop / C2S_TradeRequest when a
//     registered character's inventory changes; BuyItem also guards against recycled pointers.
#pragma once

// ES: Espacio de nombres de los hooks de inventario.
// EN: Namespace for the inventory hooks.
namespace kmp::inventory_hooks {

// ES: Instala los tres hooks que se hayan resuelto; true si al menos uno quedó instalado.
// EN: Installs whichever of the three hooks were resolved; true if at least one was installed.
bool Install();
// ES: Quita los hooks instalados y olvida sus trampolines.
// EN: Removes the installed hooks and forgets their trampolines.
void Uninstall();

// ES: Suprime los envíos de red durante la carga (los objetos que se añaden al cargar partida).
// Suppress network sends during loading (items added during save load)
void SetLoading(bool loading);

} // namespace kmp::inventory_hooks
