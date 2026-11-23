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
