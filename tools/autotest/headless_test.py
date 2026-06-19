#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
headless_test.py
================
Prueba HEADLESS (sin GUI) de la capa de red/protocolo del mod, usando los
ejecutables de test que YA trae el proyecto:

  - KenshiMP.Server.exe          -> el server MP
  - KenshiMP.IntegrationTest.exe -> 15 tests de protocolo automaticos
  - KenshiMP.TestClient.exe      -> bot "jugador falso" que conecta y se mueve

POR QUE EXISTE:
  La automatizacion de GUI (autotest_kenshi.py) es fragil y prueba el flujo
  COMPLETO de usuario. Pero para verificar SOLO que el server, el handshake,
  el spawn, la sincronizacion de posicion y el combate funcionan a nivel de
  RED, esto es MUCHO mas fiable: no depende de clics ni de Kenshi cargando.

  Onyx deberia usar ESTE script primero para validar la capa de red, y el
  autotest de GUI solo cuando quiera probar el flujo real dentro del juego.

QUE HACE:
  --integration : lanza la suite de 15 tests de protocolo (la mas util).
  --client      : lanza el server + un bot TestClient que conecta y patrulla.
                  Util para ver, junto a un Kenshi real, si el bot aparece.

USO:
  python headless_test.py --integration
  python headless_test.py --client
  python headless_test.py --client --name "OnyxBot"
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
GAME_DIR = Path(r"E:\SteamLibrary\steamapps\common\Kenshi")

SERVER = GAME_DIR / "KenshiMP.Server.exe"
INTEGRATION = GAME_DIR / "KenshiMP.IntegrationTest.exe"
TESTCLIENT = GAME_DIR / "KenshiMP.TestClient.exe"


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def matar_procesos():
    """Arranque limpio."""
    for exe in ("KenshiMP.Server.exe", "KenshiMP.IntegrationTest.exe", "KenshiMP.TestClient.exe"):
        subprocess.run(["taskkill", "/F", "/IM", exe],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_integration():
    """
    Lanza KenshiMP.IntegrationTest.exe. Este exe descubre/arranca el server
    el solo y corre los 15 tests, imprimiendo PASS/FAIL. Capturamos su salida.
    """
    if not INTEGRATION.exists():
        log(f"[FATAL] No existe {INTEGRATION}.")
        log("        Compila el proyecto (build) para generar los exes de test,")
        log("        o usa el autotest de GUI (autotest_kenshi.py).")
        sys.exit(2)

    log("Lanzando suite de integracion (15 tests de protocolo). Esto arranca el server solo.")
    matar_procesos()
    time.sleep(1)
    # cwd = GAME_DIR para que encuentre server.json y el server exe.
    proc = subprocess.run([str(INTEGRATION)], cwd=str(GAME_DIR),
                          capture_output=True, text=True, timeout=300)
    print("\n----- SALIDA IntegrationTest -----")
    print(proc.stdout)
    if proc.stderr:
        print("----- stderr -----")
        print(proc.stderr)
    print("----------------------------------")
    log(f"IntegrationTest termino con codigo {proc.returncode} "
        f"(0 = todos los tests OK).")
    matar_procesos()


def run_client(nombre):
    """
    Lanza el server + un bot TestClient que conecta a 127.0.0.1:27800,
    se posiciona cerca del host y patrulla. Util para verificar visualmente,
    junto a un Kenshi real conectado, que el bot remoto aparece y se mueve.
    """
    if not SERVER.exists():
        log(f"[FATAL] No existe {SERVER}.")
        sys.exit(2)
    if not TESTCLIENT.exists():
        log(f"[FATAL] No existe {TESTCLIENT}. Compila el proyecto para generarlo.")
        sys.exit(2)

    matar_procesos()
    time.sleep(1)

    log("Lanzando server MP (ventana propia)...")
    subprocess.Popen([str(SERVER)], cwd=str(GAME_DIR),
                     creationflags=subprocess.CREATE_NEW_CONSOLE)
    time.sleep(3)

    log(f"Lanzando TestClient bot '{nombre}' -> 127.0.0.1:27800 (ventana propia).")
    log("El bot conecta, espera la posicion del host y patrulla en linea de 100 unidades.")
    subprocess.Popen([str(TESTCLIENT), "127.0.0.1", "27800", nombre],
                     cwd=str(GAME_DIR),
                     creationflags=subprocess.CREATE_NEW_CONSOLE)

    log("Server + bot lanzados en ventanas separadas.")
    log("Conecta un Kenshi real (autotest_kenshi.py o a mano) para ver al bot.")
    log("Cierra las ventanas manualmente cuando termines, o ejecuta taskkill.")


def main():
    parser = argparse.ArgumentParser(description="Pruebas headless de red del mod Kenshi co-op.")
    g = parser.add_mutually_exclusive_group(required=True)
    g.add_argument("--integration", action="store_true",
                   help="Corre los 15 tests de protocolo (recomendado).")
    g.add_argument("--client", action="store_true",
                   help="Lanza server + bot TestClient que patrulla.")
    parser.add_argument("--name", default="OnyxBot",
                        help="Nombre del bot TestClient (solo con --client).")
    args = parser.parse_args()

    if args.integration:
        run_integration()
    elif args.client:
        run_client(args.name)


if __name__ == "__main__":
    main()
