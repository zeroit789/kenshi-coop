# Credits / Créditos

This project would not exist without the work of the Kenshi modding and reverse-engineering
community. **Kenshi Co-op is a continuation that stands on their shoulders.** Below is honest
attribution to every project we learned from, built on, or borrowed ideas from.

Este proyecto no existiría sin el trabajo de la comunidad de modding e ingeniería inversa de
Kenshi. **Kenshi Co-op es una continuación que se apoya en su trabajo.** Abajo va la atribución
honesta a cada proyecto del que aprendimos, sobre el que construimos, o del que tomamos ideas.

---

## Foundation / Base

### The404Studios — Kenshi-Online
- **Repo:** https://github.com/The404Studios/Kenshi-Online
- **License:** MIT
- **What we use / Qué usamos:** This is our **direct base**. The networking architecture, the
  Ogre-plugin injection, the runtime string-xref address resolution, the entity registry, the
  protocol, the dedicated server and the in-game UI all come from here. We fixed the build (missing
  source files in the CMake scripts) so the June 2026 code actually compiles and runs on Steam 1.0.68.
  / Es nuestra **base directa**: arquitectura de red, inyección como plugin de Ogre, resolución de
  direcciones por texto en runtime, registro de entidades, protocolo, servidor dedicado y UI. Nosotros
  arreglamos la compilación (faltaban archivos en los CMake) para que el código de junio 2026
  compilara y arrancara en Steam 1.0.68.

## Learned from / De los que aprendimos

### im-blatnoyua — kenshi-online-simplified
- **Repo:** https://github.com/im-blatnoyua/kenshi-online-simplified
- **License:** MIT
- **What:** A sibling snapshot of the same lineage. Reference for zone interest management (3x3
  grid), delta compression and interpolation. / Snapshot hermano del mismo linaje. Referencia para
  gestión de interés por zonas (grid 3x3), compresión delta e interpolación.

### ernivani — kenshi-mp
- **Repo:** https://github.com/ernivani/kenshi-mp
- **What:** Independent multiplayer attempt. Reference for the host→joiner save-transfer approach
  (zip + chunked ENet + SHA256). / Intento multijugador independiente. Referencia para la
  transferencia de partida host→joiner (zip + chunks ENet + SHA256).

### codiren — KServerMod
- **Repo:** https://github.com/codiren/KServerMod
- **What:** Early multiplayer experiment; reference for position/spawn sync ideas. / Experimento
  multijugador temprano; referencia de ideas de sincronización de posición/spawn.

## Reverse-engineering libraries / Librerías de ingeniería inversa

### BFrizzleFoShizzle — RE_Kenshi & KenshiLib
- **Repos:** https://github.com/BFrizzleFoShizzle/RE_Kenshi · https://github.com/BFrizzleFoShizzle/KenshiLib
- **License:** GPLv3
- **What:** The canonical Kenshi mod loader and structure-reconstruction library. Reference for the
  game's memory layout, struct offsets and the version-independent address resolution (MD5 version
  detection + per-version RVA tables). / El cargador de mods y la librería de reconstrucción de
  estructuras de referencia de Kenshi. Referencia para el mapa de memoria, offsets de struct y la
  resolución de direcciones independiente de versión.

### KenshiReclaimer / KServerMod structs, Cheat Engine community
- Verified character/struct offsets and pointer chains. / Offsets de personaje/struct y cadenas de
  punteros verificados.

## Third-party dependencies / Dependencias de terceros (bundled in `lib/`)

| Library | Author | License | Pinned commit |
|---------|--------|---------|---------------|
| [ENet](https://github.com/lsalzman/enet) | Lee Salzman | MIT | `8be2368` |
| [Dear ImGui](https://github.com/ocornut/imgui) | Omar Cornut | MIT | `dbb5eeaad` |
| [nlohmann/json](https://github.com/nlohmann/json) | Niels Lohmann | MIT | `9cca280a4` |
| [MinHook](https://github.com/TsudaKageyu/minhook) | Tsuda Kageyu | BSD-2-Clause | `1e9ad1e` |
| [spdlog](https://github.com/gabime/spdlog) | Gabime | MIT | `48bcf39a` |

---

## Our contributions / Nuestras aportaciones
- Fixed the build so the project compiles (missing `.cpp` files added to the `KenshiMP.Core` and
  `KenshiMP.Server` CMake scripts — without these, the client DLL never linked). / Arreglada la
  compilación (archivos `.cpp` que faltaban en los CMake del cliente y el servidor).
- Documentation, bilingual README, and ongoing work toward a stable co-op session for up to 16
  players on Steam 1.0.68. / Documentación, README bilingüe, y trabajo continuo hacia una sesión
  co-op estable de hasta 16 jugadores en Steam 1.0.68.

**Thank you to everyone above.** Open source is a relay race, not a solo sprint.
**Gracias a toda la gente de arriba.** El open source es una carrera de relevos, no una carrera en solitario.
