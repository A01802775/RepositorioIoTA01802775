"""
main.py — Lado Linux de la App del Arduino UNO Q
Proyecto: Invernadero IoT — TC1004B

Qué hace:
  1. Recibe del MCU cada lectura por el Bridge ("lectura").
  2. Arma el MISMO mensaje que muestra la LCD (alarma / sensor no detectado /
     todo normal) para que Grafana lo "retransmita".
  3. La convierte a JSON y la publica por MQTT en:
        invernadero/<NODO_ID>/datos
     y el estado del nodo (online/offline) en:
        invernadero/<NODO_ID>/estado
  4. Cada 5 s le avisa al MCU si MQTT está conectado ("estado_mqtt")
     para que lo muestre en la LCD.

App Lab / arduino-app-cli corre este archivo dentro de un contenedor,
así que se reinicia solo junto con la App.
"""

import json
import os
import socket
import struct
import time

import paho.mqtt.client as mqtt
from arduino.app_utils import App, Bridge


# =============================================================================
#  BLOQUE 1: CONFIGURACIÓN
# =============================================================================

NODO_ID = "zona1"

def puerta_de_enlace():
    """IP del Linux de la placa vista desde este contenedor (su gateway)."""
    try:
        with open("/proc/net/route") as f:
            for linea in f.readlines()[1:]:
                campos = linea.split()
                if campos[1] == "00000000":  # ruta por defecto
                    return socket.inet_ntoa(struct.pack("<L", int(campos[2], 16)))
    except Exception:
        pass
    return "172.17.0.1"


# El contenedor NO puede mandar directo a la compu por el cable USB
# (NetworkManager lo bloquea). Por eso se conecta al repetidor MQTT
# (servicio mqtt-relay, socat) que corre en el Linux de la placa, y ese
# lo reenvía al Mosquitto de Docker Desktop en tu compu.
# Se puede forzar otra dirección con la variable de entorno MQTT_BROKER.
MQTT_BROKER = os.getenv("MQTT_BROKER") or puerta_de_enlace()
MQTT_PUERTO = int(os.getenv("MQTT_PUERTO", "1883"))

TOPICO_DATOS = f"invernadero/{NODO_ID}/datos"
TOPICO_ESTADO = f"invernadero/{NODO_ID}/estado"


def log(msg):
    print(msg, flush=True)


# =============================================================================
#  BLOQUE 2: CLIENTE MQTT
# =============================================================================

def al_conectar(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        log(f"[MQTT] Conectado a {MQTT_BROKER}:{MQTT_PUERTO}")
        client.publish(TOPICO_ESTADO, "online", qos=1, retain=True)
    else:
        log(f"[MQTT] Rechazado: {reason_code}")


def al_desconectar(client, userdata, flags, reason_code, properties):
    log(f"[MQTT] Desconectado ({reason_code}). Reintentando solo...")


def al_fallar_conexion(client, userdata):
    log(f"[MQTT] No se pudo conectar a {MQTT_BROKER}:{MQTT_PUERTO}. "
        f"¿Está activo el servicio mqtt-relay en la placa y Mosquitto en Docker Desktop?")


cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"unoq-{NODO_ID}")
cliente.on_connect = al_conectar
cliente.on_disconnect = al_desconectar
cliente.on_connect_fail = al_fallar_conexion
# Si la placa se apaga, el broker publica "offline" por nosotros
cliente.will_set(TOPICO_ESTADO, "offline", qos=1, retain=True)
cliente.reconnect_delay_set(min_delay=1, max_delay=30)

# connect_async + loop_start: no bloquea y reconecta automáticamente
cliente.connect_async(MQTT_BROKER, MQTT_PUERTO, keepalive=30)
cliente.loop_start()


# =============================================================================
#  BLOQUE 3: RECEPCIÓN DESDE EL MCU (Bridge)
# =============================================================================

# Códigos de estado (iguales a los del dashboard de Grafana)
CODIGO_OK = 0            # Todo detectado con normalidad
CODIGO_NO_CONECTADO = 1  # (lo pone Grafana cuando dejan de llegar datos)
CODIGO_SENSORES = 2      # Uno o más sensores no se detectan
CODIGO_ALARMA = 3        # Algún sensor fuera de rango

NOMBRES_ESTADO = {-1: "BAJA", 1: "ALTA"}


def decodificar_alertas(alertas):
    """Del sketch (bitsAlertas): 2 bits por sensor, 0=BAJO 1=OK 2=ALTO.
    Devuelve {"temp": -1/0/1, "hum": ..., "agua": ..., "luz": ...}"""
    return {nombre: ((alertas >> (2 * i)) & 3) - 1
            for i, nombre in enumerate(("temp", "hum", "agua", "luz"))}


def armar_mensaje(temp, hum, luz, agua, alarma, detectados, estados):
    """Mismo texto que la mini interfaz de la LCD, en una sola línea."""
    faltan = [n for n, ok in (("DHT11", detectados["dht11"]), ("Agua", detectados["agua"]),
                              ("Luz", detectados["luz"]), ("LCD", detectados["lcd"])) if not ok]
    partes = []
    if alarma:
        detalle = []
        if estados["temp"]: detalle.append(f"Temp {NOMBRES_ESTADO[estados['temp']]} ({temp} °C)")
        if estados["hum"]:  detalle.append(f"Humedad {NOMBRES_ESTADO[estados['hum']]} ({hum} %)")
        if estados["agua"]: detalle.append(f"Agua {NOMBRES_ESTADO[estados['agua']]} ({agua} %)")
        if estados["luz"]:  detalle.append(f"Luz {NOMBRES_ESTADO[estados['luz']]} ({luz} %)")
        partes.append("🚨 ALARMA: " + (", ".join(detalle) or "revisa sensores"))
    if faltan:
        partes.append("⚠️ Uno o más sensores no se detectan: " + ", ".join(faltan))
    if not partes:
        return CODIGO_OK, "✅ Todo detectado con normalidad"
    codigo = CODIGO_ALARMA if alarma else CODIGO_SENSORES
    return codigo, " · ".join(partes)


def recibir_lectura(temp, hum, luz, agua, alarma, dht_ok, sensores=None, alertas=0):
    """Lo llama el sketch con Bridge.notify("lectura", ...).
    ⚠ El orden de los argumentos debe coincidir con enviarLectura() del sketch.
    sensores (bits): 1=DHT11 2=agua 4=luz 8=LCD   (1 = detectado)
    alertas  (bits): ver decodificar_alertas()
    Si llega un sketch viejo (sin sensores/alertas) se asume todo detectado."""
    if sensores is None:
        sensores = 0b1110 | (1 if dht_ok else 0)
        alertas = 0b01010101  # todo OK
    detectados = {
        "dht11": bool(sensores & 1),
        "agua": bool(sensores & 2),
        "luz": bool(sensores & 4),
        "lcd": bool(sensores & 8),
    }
    codigo, mensaje = armar_mensaje(temp, hum, luz, agua, alarma,
                                    detectados, decodificar_alertas(alertas))
    datos = {
        "nodo": NODO_ID,
        # Sensor que falló o no está conectado -> null (no se guarda ruido)
        "temp": temp if dht_ok else None,
        "hum": hum if dht_ok else None,
        "luz": luz if detectados["luz"] else None,
        "agua": agua if detectados["agua"] else None,
        "alarma": bool(alarma),
        "sensores": int(sensores),
        "codigo": codigo,
        "mensaje": mensaje,
    }
    payload = json.dumps(datos, ensure_ascii=False)

    if cliente.is_connected():
        cliente.publish(TOPICO_DATOS, payload.encode("utf-8"), qos=0)
        log(f"[MQTT] -> {TOPICO_DATOS} : {payload}")
    else:
        log(f"[MQTT] Sin conexión, lectura descartada: {payload}")


Bridge.provide("lectura", recibir_lectura)


# =============================================================================
#  BLOQUE 4: "MAIN" — ciclo que mantiene viva la App
# =============================================================================

def ciclo():
    # Avisar al MCU el estado de MQTT (lo muestra en la LCD)
    Bridge.notify("estado_mqtt", cliente.is_connected())
    time.sleep(5)


log(f"=== Nodo {NODO_ID} iniciado. Broker: {MQTT_BROKER}:{MQTT_PUERTO} ===")
App.run(user_loop=ciclo)
