// ES: Interfaz del módulo de hooks de recursos de Ogre3D (MeshManager/TextureManager/
//     MaterialManager::load). Su objetivo era medir con precisión la carga de recursos
//     para el LoadingOrchestrator. Hoy está SIN IMPLEMENTAR: el descubrimiento de los
//     managers siempre falla y el orquestador usa la detección por ráfagas (burst timing).
// EN: Interface of the Ogre3D resource hooks module (MeshManager/TextureManager/
//     MaterialManager::load). Its goal was precise resource-load timing for the
//     LoadingOrchestrator. It is currently NOT IMPLEMENTED: manager discovery always
//     fails and the orchestrator falls back to burst-detection timing.
#pragma once

// ES: Espacio de nombres de los hooks de recursos de Ogre.
// EN: Namespace for the Ogre resource hooks.
namespace kmp::resource_hooks {

// ES: Intenta descubrir y hookear los resource managers de Ogre3D.
//     Devuelve true si se instaló al menos un hook; false si el descubrimiento falló
//     (degradación controlada: el LoadingOrchestrator funciona sin estos hooks usando
//     la temporización por ráfagas). Hoy siempre devuelve false.
// EN: Attempt to discover and hook Ogre3D resource managers.
// Returns true if at least one hook was installed.
// Returns false if discovery failed (graceful degradation — LoadingOrchestrator
// works without resource hooks, falling back to burst-detection timing).
bool Install();

// ES: Quita los hooks de vtable de Ogre si llegaron a instalarse.
// EN: Removes the Ogre vtable hooks if they were ever installed.
void Uninstall();

// ES: Indica si los hooks de recursos de Ogre están activos.
// EN: Whether Ogre resource hooks are active
bool IsActive();

} // namespace kmp::resource_hooks
