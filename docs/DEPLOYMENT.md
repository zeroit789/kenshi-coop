# Kenshi-Online Deployment Guide
### Complete Installation & Server Setup for End Users

---

## Table of Contents

1. [System Requirements](#system-requirements)
2. [Client Installation](#client-installation)
3. [Server Setup](#server-setup)
4. [Configuration](#configuration)
5. [Port Forwarding & Networking](#port-forwarding--networking)
6. [Verification](#verification)
7. [Common Issues](#common-issues)
8. [Advanced Deployment](#advanced-deployment)

---

## System Requirements

### Client (Player)
- **OS**: Windows 7/8/10/11 (64-bit)
- **Game**: Kenshi (Steam or GOG) - fully updated
- **Network**: UDP port 27800 (outbound - usually open by default)
- **Disk**: ~10 MB for mod files

### Server (Host)
- **OS**: Windows (native) or Linux (via Wine)
- **RAM**: 512 MB minimum, 1 GB recommended
- **Network**: UDP port 27800 (inbound - requires forwarding)
- **Disk**: ~10 MB + save file storage (~1-5 MB per save)

---

## Client Installation

### Method A: Automatic Installation (Recommended)

**Using the Injector GUI:**

1. Download `KenshiOnline.zip` and extract to any folder
2. Run `KenshiMP.Injector.exe`
3. The injector will:
   - Auto-detect your Kenshi installation (Steam/GOG)
   - Let you set your player name
   - Let you enter server IP/port
   - Show a "PLAY" button to launch
4. Click **PLAY**

**What the Injector does:**
- Copies `KenshiMP.Core.dll` to your Kenshi folder
- Adds `Plugin=KenshiMP.Core` to `Plugins_x64.cfg`
- Installs `kenshi-online.mod` to `data/` and `mods/kenshi-online/`
- Adds `kenshi-online` to `data/__mods.list`
- Writes connection config to `%APPDATA%\KenshiMP\client.json`
- Launches `kenshi_x64.exe`

**Using install.bat:**

1. Extract `KenshiOnline.zip` anywhere
2. Right-click `install.bat` → **Run as Administrator**
3. The script auto-detects your Kenshi folder (or asks if not found)
4. Creates backups in `Kenshi\KenshiMP_backup\`
5. Installs all files and patches `Plugins_x64.cfg` and the main menu layout

Launch Kenshi normally after installation. You'll see a **MULTIPLAYER** button on the main menu.

---

### Method B: Manual Installation

**For advanced users or troubleshooting:**

1. **Locate your Kenshi folder:**
   - Steam: `C:\Program Files (x86)\Steam\steamapps\common\Kenshi`
   - GOG: `C:\GOG Games\Kenshi`

2. **Copy the plugin DLL:**
   ```
   KenshiMP.Core.dll → <Kenshi>\KenshiMP.Core.dll
   ```

3. **Edit `Plugins_x64.cfg`:**
   Open `<Kenshi>\Plugins_x64.cfg` in Notepad and add:
   ```
   Plugin=KenshiMP.Core
   ```

4. **Install the mod:**
   ```
   kenshi-online.mod → <Kenshi>\data\kenshi-online.mod
   kenshi-online.mod → <Kenshi>\mods\kenshi-online\kenshi-online.mod
   ```
   Open `<Kenshi>\data\__mods.list` and add:
   ```
   kenshi-online
   ```

5. **Install UI layouts:**
   ```
   Kenshi_MultiplayerPanel.layout → <Kenshi>\data\gui\layout\
   Kenshi_MultiplayerHUD.layout → <Kenshi>\data\gui\layout\
   ```

6. **Patch main menu layout (optional):**
   Edit `<Kenshi>\data\gui\layout\Kenshi_MainMenu.layout`:
   
   Find the line with `name="OptionsButton"` and add this BEFORE it:
   ```xml
   <Widget type="Button" skin="Kenshi_Button1" position_real="0.260417 0.582407 0.15625 0.0638889" name="MultiplayerButton">
       <Property key="Caption" value="MULTIPLAYER"/>
       <Property key="FontName" value="Kenshi_PaintedTextFont_Large"/>
   </Widget>
   ```

7. **Launch Kenshi normally**

---

## Server Setup

### Windows (Native)

#### Option 1: Quick Start

1. Extract `KenshiOnline.zip`
2. Double-click `KenshiMP.Server.exe`
3. The server starts with defaults:
   - Name: "KenshiMP Server"
   - Port: 27800
   - Max Players: 16
   - PvP: Enabled

#### Option 2: Custom Configuration

1. Create `server.json` next to `KenshiMP.Server.exe`:
   ```json
   {
     "serverName": "My Kenshi Server",
     "port": 27800,
     "maxPlayers": 16,
     "password": "",
     "pvpEnabled": true,
     "gameSpeed": 1.0,
     "savePath": "world.kmpsave",
     "tickRate": 20,
     "masterServer": "162.248.94.149",
     "masterPort": 27801
   }
   ```

2. Run `KenshiMP.Server.exe` (it auto-loads `server.json`)

#### Option 3: Command Line

```cmd
KenshiMP.Server.exe --port 27800 --name "Custom Server" --max-players 16
```

The server console shows:
```
[*] KenshiMP.Server v1.0
[*] Loading config from server.json...
[*] Starting server on port 27800...
[*] Server "My Kenshi Server" listening (max 16 players)
[*] UPnP: Attempting to map port 27800...
[*] UPnP: Successfully mapped port 27800 (external IP: 203.0.113.45)
```

---

### Linux (via Wine)

**Requirements:**
- Wine 6.0+ (tested on Wine 8.0)
- `winetricks` (for dependencies)

**Setup:**

1. Install Wine and dependencies:
   ```bash
   sudo apt-get install wine64 winetricks
   winetricks vcrun2019  # Install Visual C++ 2019 runtime
   ```

2. Copy Windows binaries to Linux:
   ```bash
   mkdir -p ~/kenshi-server
   cp KenshiMP.Server.exe ~/kenshi-server/
   cp server.json ~/kenshi-server/
   cd ~/kenshi-server
   ```

3. Run the server:
   ```bash
   wine64 KenshiMP.Server.exe
   ```

4. **(Optional)** Run as systemd service:
   
   Create `/etc/systemd/system/kenshi-server.service`:
   ```ini
   [Unit]
   Description=Kenshi-Online Dedicated Server
   After=network.target

   [Service]
   Type=simple
   User=kenshi
   WorkingDirectory=/home/kenshi/kenshi-server
   ExecStart=/usr/bin/wine64 /home/kenshi/kenshi-server/KenshiMP.Server.exe
   Restart=on-failure
   RestartSec=10

   [Install]
   WantedBy=multi-user.target
   ```

   Enable and start:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable kenshi-server
   sudo systemctl start kenshi-server
   sudo systemctl status kenshi-server
   ```

---

### Docker Deployment

**Dockerfile:**

```dockerfile
FROM tobix/pipelight:latest

WORKDIR /app

# Copy server files
COPY KenshiMP.Server.exe server.json ./

# Install .NET dependencies
RUN winetricks -q vcrun2019

# Expose UDP port
EXPOSE 27800/udp

# Run server
CMD ["wine64", "KenshiMP.Server.exe"]
```

**Build and run:**

```bash
docker build -t kenshi-server .
docker run -d --name kenshi-online -p 27800:27800/udp kenshi-server
```

---

## Configuration

### Client Configuration

**Location:** `%APPDATA%\KenshiMP\client.json`

```json
{
  "playerName": "YourName",
  "lastServer": "162.248.94.149",
  "lastPort": 27800,
  "autoConnect": true,
  "overlayScale": 1.0,
  "masterServer": "162.248.94.149",
  "masterPort": 27801,
  "favoriteServers": [
    "162.248.94.149:27800",
    "192.168.1.100:27800"
  ],
  "useSyncOrchestrator": false
}
```

**Fields:**
- `playerName` - Your in-game name (displayed to other players)
- `lastServer` - Last connected server IP
- `lastPort` - Last connected server port (default: 27800)
- `autoConnect` - Auto-connect on world load (true/false)
- `overlayScale` - UI scale multiplier (0.5-2.0)
- `masterServer` - Master server for server browser
- `masterPort` - Master server port (default: 27801)
- `favoriteServers` - List of saved servers for quick connect
- `useSyncOrchestrator` - Enable experimental sync pipeline (default: false)

**In-game configuration:**
- Press **F1** to open multiplayer menu
- Click **Settings** to edit player name, auto-connect, etc.

---

### Server Configuration

**Location:** `server.json` (same folder as `KenshiMP.Server.exe`)

```json
{
  "serverName": "KenshiMP Server",
  "port": 27800,
  "maxPlayers": 16,
  "password": "",
  "pvpEnabled": true,
  "gameSpeed": 1.0,
  "savePath": "world.kmpsave",
  "tickRate": 20,
  "masterServer": "162.248.94.149",
  "masterPort": 27801
}
```

**Fields:**
- `serverName` - Server name (shown in browser, max 64 chars)
- `port` - UDP listen port (default: 27800, range: 1024-65535)
- `maxPlayers` - Max concurrent players (1-16)
- `password` - Server password (empty = no password, max 32 chars)
- `pvpEnabled` - Allow player-vs-player combat (true/false)
- `gameSpeed` - Game speed multiplier (0.5-3.0, 1.0 = normal)
- `savePath` - World save file path (relative or absolute)
- `tickRate` - State sync frequency in Hz (10-60, default: 20)
- `masterServer` - Master server IP for server listing
- `masterPort` - Master server port (default: 27801)

**Notes:**
- Config is loaded on startup only (restart server to apply changes)
- `tickRate=20` means 50ms update interval (20 Hz)
- Higher `tickRate` = smoother sync but more bandwidth (~2-5 KB/s per player)

---

## Port Forwarding & Networking

### Understanding Port 27800 UDP

Kenshi-Online uses **UDP port 27800** for all game traffic.

- **Client → Server:** Outbound UDP (usually works without config)
- **Server → Client:** Inbound UDP (requires port forwarding if hosting)

---

### Automatic Setup (UPnP)

**Windows Server:**

The server automatically attempts UPnP port mapping on startup:

```
[*] UPnP: Attempting to map port 27800 (UDP)...
[*] UPnP: Local IP is 192.168.1.100
[*] UPnP: External IP is 203.0.113.45
[*] UPnP: Successfully mapped port 27800 -> 192.168.1.100:27800 (UDP)
```

**If UPnP fails:**

```
[!] UPnP: Failed to create UPnPNAT instance (hr=0x800706D9)
[!] UPnP: Router may not support UPnP. Players will need to forward port 27800 manually.
```

**Enable UPnP on your router:**
1. Open router web interface (usually http://192.168.1.1)
2. Find **UPnP** settings (often under Advanced → UPnP)
3. Enable UPnP
4. Restart the server

---

### Manual Port Forwarding

**If UPnP doesn't work or you prefer manual control:**

1. **Find your local IP:**
   - Windows: `ipconfig` → look for "IPv4 Address" (e.g., 192.168.1.100)
   - Linux: `ip addr` or `hostname -I`

2. **Access your router settings:**
   - Common IPs: 192.168.1.1, 192.168.0.1, 10.0.0.1
   - Login with admin credentials

3. **Add port forwarding rule:**
   - **Service Name:** Kenshi-Online
   - **External Port:** 27800
   - **Internal Port:** 27800
   - **Internal IP:** (your PC's local IP from step 1)
   - **Protocol:** UDP
   - **Enabled:** Yes

4. **Save and reboot router** (if required)

---

### Firewall Configuration

**Windows Firewall:**

The installer does NOT auto-add firewall rules. Add manually:

```powershell
# Allow inbound (server hosting)
netsh advfirewall firewall add rule name="Kenshi-Online Server" dir=in action=allow protocol=UDP localport=27800

# Allow outbound (client connecting)
netsh advfirewall firewall add rule name="Kenshi-Online Client" dir=out action=allow protocol=UDP remoteport=27800
```

Or use GUI:
1. Windows Defender Firewall → Advanced Settings → Inbound Rules → New Rule
2. Rule Type: **Port**
3. Protocol: **UDP**, Port: **27800**
4. Action: **Allow the connection**
5. Profile: All (Domain, Private, Public)
6. Name: **Kenshi-Online Server**

**Linux Firewall (UFW):**

```bash
sudo ufw allow 27800/udp
sudo ufw reload
```

**Linux Firewall (iptables):**

```bash
sudo iptables -A INPUT -p udp --dport 27800 -j ACCEPT
sudo iptables-save > /etc/iptables/rules.v4
```

---

### VPS / Cloud Hosting

**AWS EC2:**
1. Security Group → Inbound Rules → Add Rule
2. Type: Custom UDP, Port: 27800, Source: 0.0.0.0/0
3. Apply to your EC2 instance

**Google Cloud:**
```bash
gcloud compute firewall-rules create kenshi-online \
  --allow udp:27800 \
  --source-ranges 0.0.0.0/0 \
  --description "Kenshi-Online server port"
```

**Azure:**
```bash
az network nsg rule create --resource-group myResourceGroup \
  --nsg-name myNSG --name KenshiOnline \
  --protocol Udp --direction Inbound --priority 1000 \
  --source-address-prefixes '*' --source-port-ranges '*' \
  --destination-address-prefixes '*' --destination-port-ranges 27800 \
  --access Allow
```

**OVH / Hetzner / DigitalOcean:**
- Check provider firewall/security group settings
- Allow UDP 27800 inbound from 0.0.0.0/0

---

### Testing Port Forwarding

**From external network (use your phone's 4G/5G):**

```bash
# Linux / Mac / Git Bash
nc -u -v YOUR_PUBLIC_IP 27800

# Windows PowerShell (requires nmap)
Test-NetConnection -ComputerName YOUR_PUBLIC_IP -Port 27800 -InformationLevel Detailed
```

**Online port checker:**
- https://www.yougetsignal.com/tools/open-ports/
- Enter your public IP and port 27800 (must run server first)

**Note:** Most online checkers test TCP, not UDP. For UDP, use `nmap`:
```bash
nmap -sU -p 27800 YOUR_PUBLIC_IP
```

---

## Verification

### Client Verification

**After installation:**

1. **Check files exist:**
   ```
   <Kenshi>\KenshiMP.Core.dll
   <Kenshi>\data\kenshi-online.mod
   <Kenshi>\data\gui\layout\Kenshi_MultiplayerPanel.layout
   ```

2. **Check Plugins_x64.cfg:**
   ```
   <Kenshi>\Plugins_x64.cfg
   ```
   Should contain:
   ```
   Plugin=KenshiMP.Core
   ```

3. **Check mod load list:**
   ```
   <Kenshi>\data\__mods.list
   ```
   Should contain:
   ```
   kenshi-online
   ```

4. **Launch Kenshi:**
   - Main menu should have **MULTIPLAYER** button
   - If missing, run `install.bat` again

5. **Open multiplayer menu:**
   - Click **MULTIPLAYER**
   - Should show: "JOIN GAME", "HOST GAME", "Server Browser", "Settings"

**In-game verification:**

1. Press **F1** → should open multiplayer overlay
2. Press **Tab** → should show player list (empty if not connected)
3. Press **Insert** → should show debug/loading log
4. Press **`** (backtick) → should show debug overlay (FPS, net stats)

---

### Server Verification

**After starting server:**

1. **Check console output:**
   ```
   [*] KenshiMP.Server v1.0
   [*] Loading config from server.json...
   [*] Starting server on port 27800...
   [*] Server "My Kenshi Server" listening (max 16 players)
   ```

2. **Test local connection:**
   - Launch Kenshi with multiplayer
   - Click **MULTIPLAYER** → **JOIN GAME**
   - Enter IP: `127.0.0.1`, Port: `27800`
   - Click **CONNECT** → should say "Connected to server"

3. **Check server console:**
   ```
   [*] Player 'YourName' connected from 127.0.0.1:54321
   [*] Player 'YourName' (ID=1) authenticated
   ```

4. **Test remote connection (LAN):**
   - Find server PC's local IP: `ipconfig`
   - From another PC on same network, connect to that IP

5. **Test internet connection:**
   - Find your public IP: https://whatismyip.com
   - Have a friend connect to `YOUR_PUBLIC_IP:27800`

**Server console commands:**

| Command | Description |
|---------|-------------|
| `status` | Show server status (players, uptime, tick rate) |
| `players` | List connected players with IDs |
| `kick <id>` | Kick player by ID |
| `say <message>` | Broadcast system message to all players |
| `save` | Manual world save |
| `stop` | Graceful shutdown (saves world, closes connections) |

---

## Common Issues

### Installation Issues

#### "DLL not found" or "Failed to load plugin"

**Cause:** `KenshiMP.Core.dll` not in Kenshi folder or Plugins_x64.cfg not updated

**Fix:**
1. Verify `KenshiMP.Core.dll` exists in `<Kenshi>\` folder
2. Check `Plugins_x64.cfg` contains `Plugin=KenshiMP.Core`
3. Run `install.bat` again as Administrator
4. Restart Kenshi

---

#### "Multiplayer button missing on main menu"

**Cause:** Main menu layout not patched

**Fix:**
1. Copy `Kenshi_MainMenu.layout` from dist to `<Kenshi>\data\gui\layout\`
2. Or run `install.bat` again (creates backup first)
3. Restart Kenshi

---

#### "Kenshi crashes on startup"

**Cause:** DLL mismatch (debug vs release) or missing dependencies

**Fix:**
1. Install Visual C++ 2019 Redistributable (x64): https://aka.ms/vs/17/release/vc_redist.x64.exe
2. Verify you have Steam/GOG version, not pirated (not supported)
3. Uninstall (`uninstall.bat`), restart, reinstall
4. Check Kenshi is fully updated (Steam: verify game files)

---

### Connection Issues

#### "Connection failed" or "Timeout"

**Possible causes:**
1. Server not running
2. Wrong IP/port
3. Firewall blocking UDP 27800
4. Port forwarding not configured
5. Server crashed during UPnP discovery

**Fix:**
1. **Verify server is running:**
   - Server console window should be open
   - Look for "Server listening" message

2. **Test local connection first:**
   - Connect to `127.0.0.1:27800` from same PC
   - If this fails, server config is wrong

3. **Check firewall:**
   ```powershell
   # Windows: list firewall rules
   netsh advfirewall firewall show rule name=all | findstr "27800"
   ```

4. **Test port forwarding:**
   ```bash
   nmap -sU -p 27800 YOUR_PUBLIC_IP
   ```
   Should show: `27800/udp open|open|filtered`

5. **Check UPnP logs:**
   Server console shows UPnP status. If failed, forward manually.

6. **Verify external IP:**
   - https://whatismyip.com
   - Server logs show detected external IP if UPnP succeeded

---

#### "Connected but can't see other players"

**Cause:** Not in-game yet, or spawn failed

**Fix:**
1. **Both players must click NEW GAME:**
   - Connection happens AFTER world loads
   - Don't use Load Game (not yet supported for joining)

2. **Wait for spawn:**
   - Remote players spawn in 5-20 seconds
   - Check debug log (Insert key) for spawn messages

3. **Walk near NPCs/towns:**
   - NPC hijack system needs nearby NPCs
   - If in empty desert, walk toward a settlement

4. **Check mod installed:**
   - Verify `kenshi-online.mod` in `data/` and `mods/kenshi-online/`
   - Check `data/__mods.list` contains `kenshi-online`

---

#### "Invisible player / no model"

**Cause:** Mod templates not loaded or spawn method failed

**Fix:**
1. **Verify mod installation:**
   ```
   <Kenshi>\data\kenshi-online.mod
   <Kenshi>\mods\kenshi-online\kenshi-online.mod
   ```

2. **Check mod active in-game:**
   - Main menu → Mods → should see "kenshi-online" with checkmark

3. **Check debug log (Insert):**
   - Look for "MOD TEMPLATE spawn succeeded"
   - Or "NPC HIJACK succeeded"
   - If neither, spawn system failed

4. **Fallback spawn:**
   - After 20s, fallback uses `createRandomChar` (wrong appearance but functional)
   - Player is visible but looks like random NPC

---

### Server Issues

#### "Server won't start" or "Port already in use"

**Cause:** Another process using port 27800, or server already running

**Fix:**
1. **Check what's using the port:**
   ```powershell
   # Windows
   netstat -ano | findstr "27800"
   
   # Shows: UDP 0.0.0.0:27800 *:* 12345
   # 12345 is the process ID (PID)
   
   tasklist /FI "PID eq 12345"
   ```

2. **Kill the process:**
   ```powershell
   taskkill /F /PID 12345
   ```

3. **Change server port:**
   Edit `server.json`:
   ```json
   "port": 27801
   ```
   Don't forget to update port forwarding!

---

#### "UPnP failed" or "Router doesn't support UPnP"

**Cause:** UPnP disabled on router, or router doesn't support it

**Fix:**
1. **Enable UPnP on router:**
   - Router settings → Advanced → UPnP → Enable
   - Reboot router
   - Restart server

2. **Or forward port manually** (see [Manual Port Forwarding](#manual-port-forwarding))

3. **Server still works without UPnP:**
   - You just need to forward port manually
   - Players can still connect if port is open

---

#### "Server crashes after 'Loading config...'"

**Cause:** Malformed `server.json`

**Fix:**
1. **Validate JSON:**
   - Copy `server.json` content to https://jsonlint.com
   - Fix syntax errors (missing commas, quotes, brackets)

2. **Use default config:**
   Delete `server.json` and run server (uses defaults)

3. **Check server console for errors:**
   ```
   [!] Failed to parse server.json: ...
   ```

---

#### "Players connect but server crashes on spawn"

**Cause:** Memory corruption, debug/release mismatch, or Steam offset mismatch

**Fix:**
1. **Verify all players have same version:**
   - Check KenshiOnline.zip version/date
   - Everyone must use same release

2. **Check Steam vs GOG:**
   - Server detects game version from patterns
   - If detection fails, offsets are wrong → crash

3. **Report crash:**
   - Include server console log
   - Include client debug log (Insert → copy)

---

### Performance Issues

#### "High ping / lag"

**Possible causes:**
1. Tick rate too high for connection
2. Too many entities synced (large squad)
3. Server CPU overloaded
4. Network congestion

**Fix:**
1. **Lower tick rate:**
   `server.json`:
   ```json
   "tickRate": 15
   ```
   (15 Hz = 66ms interval, less bandwidth)

2. **Check server CPU:**
   - Task Manager → Details → `KenshiMP.Server.exe`
   - Should be <10% CPU on modern hardware
   - If >50%, report as bug

3. **Check network:**
   ```bash
   ping SERVER_IP
   ```
   Should be <100ms. If >200ms, connection is too slow.

4. **Reduce entity count:**
   - Don't sync entire squads (only player character syncs)
   - Avoid large bases near spawn (not yet optimized)

---

#### "Choppy movement / teleporting"

**Cause:** Packet loss, interpolation disabled, or tick rate too low

**Fix:**
1. **Check packet loss:**
   - In-game: Press ``` (backtick) → shows net stats
   - Packet loss should be <1%

2. **Increase tick rate:**
   `server.json`:
   ```json
   "tickRate": 30
   ```
   (30 Hz = 33ms interval, smoother but more bandwidth)

3. **Check firewall/router:**
   - Some routers drop UDP packets aggressively
   - Try connecting via VPN (Hamachi, ZeroTier) as test

---

## Advanced Deployment

### Running Multiple Servers on One Machine

**Different ports:**

1. Create separate folders:
   ```
   C:\KenshiServers\
   ├── server1\
   │   ├── KenshiMP.Server.exe
   │   └── server.json (port 27800)
   └── server2\
       ├── KenshiMP.Server.exe
       └── server.json (port 27801)
   ```

2. Edit each `server.json`:
   ```json
   // server1
   "port": 27800,
   "savePath": "world1.kmpsave"
   
   // server2
   "port": 27801,
   "savePath": "world2.kmpsave"
   ```

3. Forward both ports (27800, 27801)

4. Run both `.exe` files

---

### Master Server / Server Browser

**Default master server:** `162.248.94.149:27801`

This server maintains a public server list for the in-game browser.

**To list your server:**
- Config is auto-sent if `masterServer` is set in `server.json`
- Players click **Server Browser** → see your server
- Click **Join** to auto-connect

**To run your own master server:**
1. Run `KenshiMP.MasterServer.exe --port 27801`
2. Edit all servers' `server.json`:
   ```json
   "masterServer": "YOUR_MASTER_IP",
   "masterPort": 27801
   ```
3. Edit clients' `client.json`:
   ```json
   "masterServer": "YOUR_MASTER_IP",
   "masterPort": 27801
   ```

---

### Backup & Restore

**Server saves world automatically every 60 seconds.**

**Save file location:** `world.kmpsave` (or custom `savePath` from config)

**Manual backup:**

```bash
# Windows
copy world.kmpsave world.kmpsave.backup

# Linux
cp world.kmpsave world.kmpsave.backup
```

**Restore backup:**

1. Stop server (console: `stop`)
2. Replace save file:
   ```bash
   copy world.kmpsave.backup world.kmpsave
   ```
3. Restart server

**Save file format:**
- JSON-based, human-readable
- Contains: player positions, inventories, entity states, world time

---

### Automated Server Restart (Windows)

**Using Task Scheduler:**

1. Create `restart_server.bat`:
   ```bat
   @echo off
   cd "C:\KenshiServers\server1"
   taskkill /F /IM KenshiMP.Server.exe 2>nul
   timeout /t 5
   start "" "KenshiMP.Server.exe"
   ```

2. Task Scheduler → Create Task:
   - Name: "Kenshi Server Restart"
   - Trigger: Daily at 4:00 AM
   - Action: Run `restart_server.bat`

---

### Monitoring & Logging

**Server logs to console only (no file by default).**

**To save logs:**

```bat
KenshiMP.Server.exe > server.log 2>&1
```

**Log rotation (PowerShell):**

```powershell
# rotate_logs.ps1
$date = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
Move-Item "server.log" "logs\server_$date.log"
Start-Process "KenshiMP.Server.exe" -RedirectStandardOutput "server.log"
```

**Linux systemd logging:**

Logs are auto-captured by journald:
```bash
journalctl -u kenshi-server -f
```

---

### Bandwidth & Resource Usage

**Per player:**
- Tick rate 20 Hz: ~2-5 KB/s upload, ~1-3 KB/s download
- Tick rate 30 Hz: ~4-8 KB/s upload, ~2-5 KB/s download

**Server requirements:**
- 16 players @ 20 Hz: ~30-80 KB/s upload, ~5-10 MB RAM
- CPU: <5% on modern hardware (Intel i5 / Ryzen 5+)

**Client requirements:**
- ~2-5 KB/s per player in view
- Minimal CPU/RAM impact (<50 MB)

---

### Uninstallation

**Using uninstall.bat:**

1. Close Kenshi
2. Run `uninstall.bat`
3. Restores backups from `KenshiMP_backup\`
4. Removes all mod files

**Manual uninstall:**

1. Delete `<Kenshi>\KenshiMP.Core.dll`
2. Remove `Plugin=KenshiMP.Core` from `Plugins_x64.cfg`
3. Delete `<Kenshi>\data\kenshi-online.mod`
4. Delete `<Kenshi>\mods\kenshi-online\`
5. Remove `kenshi-online` from `data/__mods.list`
6. Delete `%APPDATA%\KenshiMP\` folder

**Server uninstall:**

Just delete `KenshiMP.Server.exe` and `server.json`. No registry/system changes made.

---

## Support

**Issues / Bug Reports:**
- GitHub: https://github.com/yourusername/KenshiMP (if public)
- Discord: [Your Discord link]
- Email: the404studios@gmail.com

**Before reporting:**
1. Check [Common Issues](#common-issues)
2. Include server console log
3. Include client debug log (Insert → copy text)
4. Specify Kenshi version (Steam/GOG), Windows version, mod list

---

## Version History

**v1.0 (2026-06-03):**
- Initial public release
- 16-player co-op
- Position, combat, chat sync
- UPnP auto port forwarding
- Server browser
- Windows + Linux (Wine) support

---

## Technical Details

**Protocol:** ENet over UDP (reliable + unreliable channels)  
**Default Port:** 27800 UDP  
**Max Players:** 16  
**Tick Rate:** 20 Hz (50ms intervals)  
**Channels:**
- Channel 0: Reliable ordered (connection, spawns, chat)
- Channel 1: Reliable unordered (combat, items)
- Channel 2: Unreliable sequenced (movement, positions)

**Sync Features:**
- Player positions (interpolated)
- Combat (server-authoritative hit resolution)
- Chat (unlimited distance)
- Entity spawns (NPCs, items)
- Squad/faction data
- World time
- Health/stats
- Animations (attack, block, run)

**Not Yet Synced:**
- Building construction (client-side only)
- NPC AI decisions (each client runs own AI)
- Item pickup/drop (partial sync)
- Dialog/quest state

---

**Made with love by fourzerofour**  
**2026-06-03**
