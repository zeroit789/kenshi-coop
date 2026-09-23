// ES: Interfaz de los hooks de edificios: colocación (RootObjectFactory::createBuilding,
//     RVA ~0x57CC70), destrucción, desmontaje, progreso de construcción y reparación. Las
//     direcciones las resuelve el escáner por patrón/cadena; cada hook se autodesactiva tras
//     10 crashes por si el escáner emparejó una función equivocada.
// EN: Interface of the building hooks: placement (RootObjectFactory::createBuilding,
//     RVA ~0x57CC70), destruction, dismantling, construction progress and repair. The scanner
//     resolves the addresses by pattern/string; each hook self-disables after 10 crashes in
//     case the scanner matched the wrong function.
#pragma once

// ES: Espacio de nombres de los hooks de edificios.
// EN: Namespace for the building hooks.
namespace kmp::building_hooks {

// ES: Instala los hooks resueltos; true si al menos uno quedó instalado.
// EN: Installs the resolved hooks; true if at least one was installed.
bool Install();
// ES: Quita los hooks instalados y olvida sus trampolines.
// EN: Removes installed hooks and forgets their trampolines.
void Uninstall();

// ES: Suprime los envíos de red durante la carga de partida.
// Suppress during save load
void SetLoading(bool loading);

} // namespace kmp::building_hooks
