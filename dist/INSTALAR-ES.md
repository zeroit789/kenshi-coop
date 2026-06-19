# Kenshi Co-op — Guía de instalación (español)

¡Hola! Esta guía te explica paso a paso cómo instalar el mod de **multijugador para Kenshi** y conectarte a la partida. Está pensada para que cualquiera pueda hacerlo, aunque no sepas nada de informática. Síguela tranquilo, que en 5 minutos estás dentro.

---

## 1. Lo que necesitas antes de empezar

- **Tener Kenshi instalado en Steam.** El mod modifica tu Kenshi, así que es obligatorio tenerlo.
- **A poder ser, la misma versión que el host: la `1.0.68`.** Si los dos tenéis versiones distintas puede dar problemas raros. (Steam normalmente os tiene a todos en la última, así que casi seguro vais iguales.)
- **El paquete `KenshiOnline-Cliente.zip`** (el que te ha pasado Zero). Es el que contiene todo esto que estás leyendo.

> Consejo: cierra Kenshi del todo antes de instalar. Que no esté abierto mientras haces los pasos.

---

## 2. Instalar el mod

Tienes dos formas de hacerlo. La **Opción A es la recomendada** porque es la más fácil y casi no falla.

### Opción A — Con el Injector (RECOMENDADA)

1. **Extrae el ZIP** en cualquier carpeta (botón derecho sobre el `.zip` → "Extraer todo..."). No lo dejes dentro del zip, sácalo a una carpeta normal, por ejemplo en tu Escritorio.
2. Entra en la carpeta y haz **doble clic en `KenshiMP.Injector.exe`**.
3. Se abre una ventanita. Ahí pones:
   - **Tu nombre** (el que quieras que vean los demás en la partida).
   - En **servidor / server address**, escribe exactamente esto:
     ```
     85.57.86.232:27800
     ```
4. Pulsa el botón **PLAY**.

El Injector hace todo solo: copia el mod dentro de tu Kenshi, lo activa y te abre el juego. No tienes que tocar nada más.

### Opción B — Con install.bat (alternativa)

Solo si la Opción A te diera problemas:

1. Extrae el ZIP en una carpeta.
2. **Botón derecho sobre `install.bat` → "Ejecutar como administrador".** (Esto es importante: tiene que ser como administrador, si no, no puede instalar bien.)
3. El script detecta solo tu carpeta de Kenshi e instala todo.
4. Cuando termine, **abre Kenshi normalmente** desde Steam.

---

## 3. Unirte a la partida de Zero

Una vez dentro del juego (te abrirá Kenshi tras el paso anterior):

1. En el menú principal, pulsa **MULTIPLAYER**.
2. Pulsa **JOIN GAME** (unirse a partida).
3. Te pedirá la IP y el puerto del servidor. Pon:
   - **IP:** `85.57.86.232`
   - **Puerto:** `27800`
4. Pulsa **CONNECT** (conectar).
5. Pulsa **NEW GAME** (partida nueva). ⚠️ **MUY IMPORTANTE: siempre NEW GAME, nunca Load Game.**
6. Elige el gamestart que ponga **Multiplayer** y **crea tu personaje**.

Y ya está, en cuanto cargue el mundo te conectas con Zero automáticamente.

---

## 4. Controles dentro del juego

| Tecla | Para qué sirve |
|-------|----------------|
| **F1** | Abrir/cerrar el menú de multijugador |
| **Enter** | Abrir/cerrar el chat |
| **Tab** | Ver la lista de jugadores conectados |

---

## 5. Si algo no va — Soluciones rápidas

**Siempre NEW GAME.** El fallo más típico es darle a "Load Game" en vez de a "NEW GAME". Si haces eso, no conecta. Empieza SIEMPRE partida nueva.

**No conecta / no entra al servidor:**
- Avisa a Zero (el host) de que **tenga el servidor abierto** en su PC. Si su servidor no está encendido, nadie puede entrar.
- El puerto **27800 UDP** tiene que estar abierto en el router del host (con UPnP activado o abierto a mano). Eso es cosa de Zero, no tuya.
- Comprueba que escribiste bien la IP: `85.57.86.232` y el puerto `27800`.

**No veo al otro jugador / está invisible:**
- Espera unos segundos, a veces tarda en aparecer.
- Si sigue sin verse, **acércate a un pueblo o a un grupo de NPCs**. El mod suele "engancharlo" mejor cerca de personajes del juego.

**Kenshi peta al entrar:**
- Asegúrate de que los dos tenéis el mod instalado.
- Recuerda: **NEW GAME**, nunca Load Game.

---

## 6. Desinstalar (si quieres dejarlo)

Ejecuta `uninstall.bat` y te devuelve el Kenshi normal, sin el mod. Limpio.

---

¿Dudas? Pregúntale a Zero. ¡Que lo disfrutes! 🎮
