# Kenshi Co-op

**Open-source cooperative multiplayer for Kenshi (Steam 1.0.68).**
A community continuation built on the work of several earlier multiplayer projects — see [CREDITS](CREDITS.md).

[![Status](https://img.shields.io/badge/status-archived%20%7C%20unstable-red)]()
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-lightgrey)]()
[![Kenshi](https://img.shields.io/badge/Kenshi-1.0.68%20Steam-blue)]()
[![License](https://img.shields.io/badge/license-MIT-green)]()

> 🛑 **Archived / not stable — please use [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) instead.**
>
> This project (a continuation of `The404Studios/Kenshi-Online`) never got past the point of
> making the host reliably play with the mod loaded — combat sync stayed at the diagnostic-hook
> stage, remote-player spawning was unreliable on the Steam build, and client-side reconciliation
> was never implemented upstream. After investigation, a different open-source project —
> [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) — turned out to solve exactly these
> problems (owner-authoritative characters with suppressed-AI puppets for remote players), and it
> actually works in live 2-player sessions today. We've moved our effort there instead of
> continuing this codebase. Our own reverse-engineering notes here (`docs/`, `RE-tools/`) remain
> as reference and fed several fixes into that project, but this repository itself is not being
> developed further.
>
> **If you want a working co-op mod for Kenshi right now, go to
> [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)** (or our fork with a combined,
> tested build of several additional fixes: [zeroit789/KenshiCoop, `integration/session-2026-07-20-all-fixes`](https://github.com/zeroit789/KenshiCoop/tree/integration/session-2026-07-20-all-fixes),
> [download the release here](https://github.com/zeroit789/KenshiCoop/releases/tag/session-2026-07-20)).

---

## 🇬🇧 English

### What is this?
Kenshi is a brilliant single-player sandbox RPG. It has **no official multiplayer**, and the
developers (Lo-Fi Games) have stated it never will. This project is a community effort to add
**cooperative multiplayer** anyway — several players sharing the same world, each controlling
their own squad.

It is a **continuation and repair** of the existing open-source attempts (mainly
[The404Studios/Kenshi-Online](https://github.com/The404Studios/Kenshi-Online)), with fixes that
make the current code actually **compile and run on the Steam build of Kenshi 1.0.68**.

### Current status
- ✅ **Builds from source** with Visual Studio 2022 (the upstream code did *not* compile — several
  source files were missing from the build scripts; fixed here).
- ✅ Client plugin (`KenshiMP.Core.dll`), dedicated server, master server and tests all build.
- ✅ Unit tests pass (95/95).
- 🔄 In-game co-op (character detection, spawning, sync) is being validated and fixed.
- 🎯 Target: up to 16 players, dedicated server, zone-based interest management.

### How it works
- The client is an **Ogre3D plugin** loaded by Kenshi (no external DLL injection).
- Game memory addresses are resolved at runtime by **string cross-references** (robust across
  Steam/GOG versions), not hardcoded offsets.
- Networking uses **ENet (reliable UDP)** with a dedicated, server-authoritative model.

### Build from source
Requirements: **Visual Studio 2022** (Desktop C++ workload) and **CMake 3.20+**.

```bash
git clone --recursive https://github.com/zeroit789/kenshi-coop.git
cd kenshi-coop
cmake -G "Visual Studio 17 2022" -A x64 -S . -B build
cmake --build build --config Release
```
> If you cloned without `--recursive`, fetch the dependencies in `lib/` (ENet, MinHook, spdlog,
> nlohmann/json, ImGui) at the pinned commits listed in [CREDITS](CREDITS.md).

Output binaries land in `build/bin/Release/`. The key file is `KenshiMP.Core.dll`.

### How to play (quick)
1. Copy `KenshiMP.Core.dll` next to `kenshi_x64.exe` and add `Plugin=KenshiMP.Core` to `Plugins_x64.cfg`.
2. Run `KenshiMP.Server.exe` (the dedicated server, UDP port `27800`).
3. Launch Kenshi → **Multiplayer** start → in-world press **F1** → **Join Game** → host IP → **Connect**.

(Detailed, up-to-date instructions live in `docs/`.)

### License
MIT — see [LICENSE](LICENSE). This project stands on the shoulders of others; full attribution in
[CREDITS](CREDITS.md). If you contribute or fork, please keep the credits intact.

---

## 🇪🇸 Español

### ¿Qué es esto?
Kenshi es un RPG sandbox de un solo jugador excelente. **No tiene multijugador oficial** y sus
creadores (Lo-Fi Games) han dicho que nunca lo tendrá. Este proyecto es un esfuerzo de la
comunidad para añadirle **cooperativo** de todas formas: varios jugadores compartiendo el mismo
mundo, cada uno controlando su propio escuadrón.

Es una **continuación y reparación** de los intentos open-source que ya existían (sobre todo
[The404Studios/Kenshi-Online](https://github.com/The404Studios/Kenshi-Online)), con arreglos que
hacen que el código actual de verdad **compile y arranque en la versión de Steam de Kenshi 1.0.68**.

### Estado actual
- ✅ **Compila desde el código fuente** con Visual Studio 2022 (el código original *no* compilaba:
  faltaban varios archivos en los scripts de compilación; corregido aquí).
- ✅ El plugin del cliente (`KenshiMP.Core.dll`), el servidor dedicado, el master server y los tests
  compilan todos.
- ✅ Los tests unitarios pasan (95/95).
- 🔄 El co-op dentro del juego (detección de personajes, spawn, sincronización) se está validando y
  arreglando.
- 🎯 Objetivo: hasta 16 jugadores, servidor dedicado, gestión de interés por zonas.

### Cómo funciona
- El cliente es un **plugin de Ogre3D** que carga Kenshi (sin inyección externa de DLL).
- Las direcciones de memoria del juego se resuelven en tiempo de ejecución por **referencias de
  texto** (robusto entre versiones Steam/GOG), no con offsets fijos.
- La red usa **ENet (UDP fiable)** con un modelo de servidor dedicado autoritativo.

### Compilar desde el código
Requisitos: **Visual Studio 2022** (workload de C++ de escritorio) y **CMake 3.20+**.

```bash
git clone --recursive https://github.com/zeroit789/kenshi-coop.git
cd kenshi-coop
cmake -G "Visual Studio 17 2022" -A x64 -S . -B build
cmake --build build --config Release
```
> Si clonaste sin `--recursive`, consigue las dependencias de `lib/` (ENet, MinHook, spdlog,
> nlohmann/json, ImGui) en los commits fijados que se listan en [CREDITS](CREDITS.md).

Los binarios salen en `build/bin/Release/`. El archivo clave es `KenshiMP.Core.dll`.

### Cómo jugar (rápido)
1. Copia `KenshiMP.Core.dll` junto a `kenshi_x64.exe` y añade `Plugin=KenshiMP.Core` a `Plugins_x64.cfg`.
2. Ejecuta `KenshiMP.Server.exe` (el servidor dedicado, puerto UDP `27800`).
3. Lanza Kenshi → inicio **Multiplayer** → dentro del mundo pulsa **F1** → **Join Game** → IP del host → **Connect**.

(Las instrucciones detalladas y actualizadas están en `docs/`.)

### Licencia
MIT — ver [LICENSE](LICENSE). Este proyecto se apoya en el trabajo de otros; atribución completa en
[CREDITS](CREDITS.md). Si contribuyes o haces un fork, por favor mantén los créditos intactos.
