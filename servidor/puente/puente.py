"""
puente.py — Escucha MQTT y guarda cada lectura en SQL Server (contenedor mssql)
Proyecto: Invernadero IoT — TC1004B

Se suscribe a  invernadero/+/datos  -> el "+" es el nodo (zona1, zona2...)
El nodo se toma del TÓPICO, como pide el reto.

Al arrancar crea (si no existen) la base de datos y la tabla `lecturas`.
"""

import json
import os
import time

import paho.mqtt.client as mqtt
import pymssql


# =============================================================================
#  BLOQUE 1: CONFIGURACIÓN (viene de docker-compose.yml / config.env)
# =============================================================================

MQTT_BROKER = os.getenv("MQTT_BROKER", "mosquitto")
MQTT_PUERTO = int(os.getenv("MQTT_PUERTO", "1883"))
TOPICO = "invernadero/+/datos"

# El SQL Server de la clase es el contenedor "mssql" (red de Docker "tc1004b").
# El puente se une a esa red, así que lo encuentra por su nombre.
MSSQL = {
    "server": os.getenv("MSSQL_HOST", "mssql"),
    "port": int(os.getenv("MSSQL_PORT", "1433")),
    "user": os.getenv("MSSQL_USER", "sa"),
    "password": os.getenv("MSSQL_PASSWORD", ""),
}
BASE = os.getenv("MSSQL_DATABASE", "invernadero")

SQL_CREAR_BASE = f"IF DB_ID(N'{BASE}') IS NULL CREATE DATABASE [{BASE}];"

SQL_CREAR_TABLA = """
IF OBJECT_ID(N'dbo.lecturas', N'U') IS NULL
BEGIN
    CREATE TABLE dbo.lecturas (
        id           BIGINT IDENTITY(1,1) PRIMARY KEY,
        ts           DATETIME2(3) NOT NULL DEFAULT SYSUTCDATETIME(),  -- hora UTC
        nodo         NVARCHAR(20) NOT NULL,                           -- del tópico MQTT
        temperatura  FLOAT NULL,                                      -- °C (NULL si falló el DHT11)
        humedad      FLOAT NULL,                                      -- %
        luz          TINYINT NULL,                                    -- % (fotorresistor)
        nivel_agua   TINYINT NULL,                                    -- %
        alarma       BIT NOT NULL DEFAULT 0
    );
    CREATE INDEX idx_nodo_ts ON dbo.lecturas (nodo, ts);
END
"""

SQL_INSERT = """
    INSERT INTO dbo.lecturas (nodo, temperatura, humedad, luz, nivel_agua, alarma)
    VALUES (%s, %s, %s, %s, %s, %s)
"""


def log(msg):
    print(msg, flush=True)


# =============================================================================
#  BLOQUE 2: BASE DE DATOS
# =============================================================================

def preparar_base():
    """Crea la base y la tabla si no existen. Reintenta hasta que SQL Server responda."""
    while True:
        try:
            with pymssql.connect(**MSSQL, database="master", autocommit=True) as con:
                with con.cursor() as cur:
                    cur.execute(SQL_CREAR_BASE)
            with pymssql.connect(**MSSQL, database=BASE, autocommit=True) as con:
                with con.cursor() as cur:
                    cur.execute(SQL_CREAR_TABLA)
            log(f"[SQL Server] Base '{BASE}' y tabla 'lecturas' listas")
            return
        except pymssql.Error as e:
            log(f"[SQL Server] No disponible ({e}). ¿Está prendido el contenedor mssql "
                f"y la contraseña de sa es correcta? Reintento en 5 s...")
            time.sleep(5)


def conectar():
    while True:
        try:
            con = pymssql.connect(**MSSQL, database=BASE, autocommit=True)
            log("[SQL Server] Conectado")
            return con
        except pymssql.Error as e:
            log(f"[SQL Server] Error al conectar ({e}). Reintento en 5 s...")
            time.sleep(5)


preparar_base()
conexion = conectar()


def guardar(nodo, datos):
    global conexion
    valores = (
        nodo,
        datos.get("temp"),
        datos.get("hum"),
        datos.get("luz"),
        datos.get("agua"),
        bool(datos.get("alarma", False)),
    )
    try:
        with conexion.cursor() as cur:
            cur.execute(SQL_INSERT, valores)
        log(f"[SQL Server] Guardado {nodo}: {valores[1:]}")
    except pymssql.Error as e:
        log(f"[SQL Server] Error al guardar ({e}). Reconectando...")
        try:
            conexion.close()
        except Exception:
            pass
        conexion = conectar()


# =============================================================================
#  BLOQUE 3: MQTT
# =============================================================================

def al_conectar(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        log(f"[MQTT] Conectado. Suscrito a {TOPICO}")
        client.subscribe(TOPICO)
    else:
        log(f"[MQTT] Rechazado: {reason_code}")


def al_recibir(client, userdata, msg):
    # invernadero/zona1/datos -> "zona1"
    partes = msg.topic.split("/")
    nodo = partes[1] if len(partes) >= 3 else "desconocido"
    try:
        datos = json.loads(msg.payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        log(f"[MQTT] Mensaje inválido en {msg.topic}: {msg.payload!r}")
        return
    guardar(nodo, datos)


# =============================================================================
#  BLOQUE 4: MAIN
# =============================================================================

if __name__ == "__main__":
    cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="puente-sqlserver")
    cliente.on_connect = al_conectar
    cliente.on_message = al_recibir
    cliente.reconnect_delay_set(min_delay=1, max_delay=30)

    while True:
        try:
            cliente.connect(MQTT_BROKER, MQTT_PUERTO, keepalive=30)
            break
        except OSError as e:
            log(f"[MQTT] Broker no disponible ({e}). Reintento en 5 s...")
            time.sleep(5)

    log("=== Puente MQTT -> SQL Server corriendo ===")
    cliente.loop_forever(retry_first_connection=True)
