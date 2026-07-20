# -*- coding: utf-8 -*-
"""
price_check.py — Verifica si el precio actual está dentro del rango de entrada.

Regla de Zero: si el precio actual está más de ENTRY_TOLERANCE_PCT (2%) fuera
del rango de entrada de la alerta en la dirección del trade → señal pasada → skip.

Usa la API PÚBLICA de Bitunix (sin autenticación) para obtener el mark price.
"""

import logging
import requests

logger = logging.getLogger("price_check")

# Endpoint público de Bitunix para obtener datos de mercado
# No requiere autenticación
BITUNIX_MARKET_URL = "https://fapi.bitunix.com/api/v1/futures/market/detail"


def obtener_mark_price(symbol: str) -> float | None:
    """Obtiene el mark price actual del par en Bitunix.
    Devuelve None si falla (red, par no existe, etc.)."""
    par = f"{symbol.upper()}USDT"
    try:
        resp = requests.get(BITUNIX_MARKET_URL, params={"symbol": par}, timeout=10)
        data = resp.json()
        # Intentar varios campos posibles según la respuesta real de la API
        # ⚠️ Ajustar la key exacta tras ver la primera respuesta real
        mark = (
            data.get("data", {}).get("markPrice")
            or data.get("data", {}).get("mark_price")
            or data.get("data", {}).get("lastPrice")
            or data.get("data", {}).get("last")
        )
        if mark is None:
            logger.warning(
                "No se encontró mark price en respuesta Bitunix para %s: %s",
                par, str(data)[:200]
            )
            return None
        precio = float(mark)
        logger.info("Mark price de %s: %.6f", par, precio)
        return precio
    except Exception as e:
        logger.warning("Error obteniendo mark price de %s: %s", par, e)
        return None


def precio_en_rango_entrada(alerta: dict, tolerancia_pct: float = 2.0) -> tuple:
    """Comprueba si el precio actual está dentro del rango de entrada ±tolerancia_pct%.

    Lógica:
      - SHORT: si precio_actual ya cayó más de tol% por debajo del entry_min → señal pasada → skip
      - LONG:  si precio_actual ya subió más de tol% por encima del entry_max → señal pasada → skip
      - Precio dentro del rango o aún no llegó → OK, entrar con orden LIMIT

    Returns:
        (True, motivo)  si se puede entrar
        (False, motivo) si hay que saltar la señal
    """
    symbol   = alerta.get("symbol", "")
    tipo     = alerta.get("type", "").lower()
    entry_min = alerta.get("entry_min")
    entry_max = alerta.get("entry_max")

    if entry_min is None or entry_max is None:
        return False, "No hay datos de entry en la alerta"

    precio = obtener_mark_price(symbol)
    if precio is None:
        # Sin precio no bloqueamos — mejor entrar con incertidumbre que perder la señal
        logger.warning(
            "No se pudo verificar el precio de %s — entrando sin filtro de precio", symbol
        )
        return True, "Sin datos de precio — entrando sin filtro"

    tol = tolerancia_pct / 100.0
    limite_bajo = entry_min * (1.0 - tol)   # 2% por debajo del mínimo del rango
    limite_alto = entry_max * (1.0 + tol)   # 2% por encima del máximo del rango

    if tipo == "short" and precio < limite_bajo:
        # Para un short: si ya cayó demasiado lejos del entry → la entrada ya se pasó
        pct_fuera = ((entry_min - precio) / entry_min) * 100
        return False, (
            f"SHORT {symbol}: precio {precio:.4f} está {pct_fuera:.1f}% "
            f"por debajo del entry ({entry_min}) — señal pasada"
        )

    if tipo == "long" and precio > limite_alto:
        # Para un long: si ya subió demasiado lejos del entry → la entrada ya se pasó
        pct_fuera = ((precio - entry_max) / entry_max) * 100
        return False, (
            f"LONG {symbol}: precio {precio:.4f} está {pct_fuera:.1f}% "
            f"por encima del entry ({entry_max}) — señal pasada"
        )

    # Precio dentro del rango → entrar directamente
    if entry_min <= precio <= entry_max:
        return True, f"Precio {precio:.4f} DENTRO del rango [{entry_min}, {entry_max}]"

    # Precio aún no llegó al rango → orden LIMIT esperará
    if tipo == "short" and precio > entry_max:
        return True, f"Precio {precio:.4f} por encima del rango (SHORT) — LIMIT esperará bajada"
    if tipo == "long" and precio < entry_min:
        return True, f"Precio {precio:.4f} por debajo del rango (LONG) — LIMIT esperará subida"

    return True, f"Precio {precio:.4f} cerca del rango — OK"
