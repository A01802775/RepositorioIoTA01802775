"""
main.py — Lado Linux de la App del Arduino UNO Q
Proyecto: Invernadero IoT — TC1004B

Qué hace:
  1. Recibe del MCU cada lectura por el Bridge ("lectura").
  2. La convierte a JSON y la publica por MQTT en:
        invernadero/<NODO_ID>/datos
     y el estado del nodo (online/offline) en:
        invernadero/<NODO_ID>/estado
  3. Cada 5 s le avisa al MCU si MQTT está conectado ("estado_mqtt")
     para que lo muestre en la LCD.

App Lab / arduino-app-cli corre este archivo dentro de un contenedor,
así que se reinicia solo junto con la App.
"""

import json
import os
import time

import paho.mqtt.client as mqtt
from arduino.app_utils import App, Bridge


# =============================================================================
#  BLOQUE 1: CONFIGURACIÓN
# =============================================================================

NODO_ID = "zona1"

# IP de TU COMPU (donde corre Docker Desktop con Mosquitto), vista desde
# la placa por el cable USB. Para saberla, en PowerShell:
#     ssh arduino@192.168.7.1 "echo $SSH_CLIENT"
# El primer número que sale es la IP (ej. 192.168.7.132).
MQTT_BROKER = os.getenv("MQTT_BROKER", "192.168.7.132")
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


cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"unoq-{NODO_ID}")
cliente.on_connect = al_conectar
cliente.on_disconnect = al_desconectar
# Si la placa se apaga, el broker publica "offline" por nosotros
cliente.will_set(TOPICO_ESTADO, "offline", qos=1, retain=True)
cliente.reconnect_delay_set(min_delay=1, max_delay=30)

# connect_async + loop_start: no bloquea y reconecta automáticamente
cliente.connect_async(MQTT_BROKER, MQTT_PUERTO, keepalive=30)
cliente.loop_start()


# =============================================================================
#  BLOQUE 3: RECEPCIÓN DESDE EL MCU (Bridge)
# =============================================================================

def recibir_lectura(temp, hum, luz, agua, alarma, dht_ok):
    """Lo llama el sketch con Bridge.notify("lectura", ...).
    ⚠ El orden de los argumentos debe coincidir con enviarLectura() del sketch."""
    datos = {
        "nodo": NODO_ID,
        "temp": temp if dht_ok else None,
        "hum": hum if dht_ok else None,
        "luz": luz,
        "agua": agua,
        "alarma": bool(alarma),
    }
    payload = json.dumps(datos)

    if cliente.is_connected():
        cliente.publish(TOPICO_DATOS, payload, qos=0)
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
