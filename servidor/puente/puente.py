"""
puente.py — Escucha MQTT y guarda cada lectura en SQL Server (contenedor mssql)
Proyecto: Invernadero IoT — TC1004B

Se suscribe a:
  invernadero/+/datos   -> lecturas de sensores (JSON)       -> tabla `lecturas`
  invernadero/+/estado  -> "online" / "offline" del Arduino  -> tabla `conexiones`
El "+" es el nodo (zona1, zona2...). El nodo se toma del TÓPICO, como pide el reto.

"offline" lo publica Mosquitto solo (Last Will) cuando la placa se desconecta
sin despedirse: se apagó, perdió el WiFi, etc.

Al arrancar crea (si no existen) la base de datos y las tablas.
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
TOPICO_ESTADO = "invernadero/+/estado"

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

# Columnas nuevas (mini interfaz de la LCD). Se agregan solas a la tabla
# existente, sin borrar las lecturas que ya había.
SQL_AGREGAR_COLUMNAS = """
IF COL_LENGTH(N'dbo.lecturas', N'mensaje') IS NULL
    ALTER TABLE dbo.lecturas ADD
        sensores  TINYINT       NULL,   -- bits: 1=DHT11 2=agua 4=luz 8=LCD (1 = detectado)
        codigo    TINYINT       NULL,   -- 0=todo OK, 2=sensor no detectado, 3=alarma
        mensaje   NVARCHAR(200) NULL;   -- el mismo texto que muestra la LCD
"""

SQL_CREAR_TABLA_CONEXIONES = """
IF OBJECT_ID(N'dbo.conexiones', N'U') IS NULL
BEGIN
    CREATE TABLE dbo.conexiones (
        id      BIGINT IDENTITY(1,1) PRIMARY KEY,
        ts      DATETIME2(3) NOT NULL DEFAULT SYSUTCDATETIME(),  -- hora UTC
        nodo    NVARCHAR(20) NOT NULL,                           -- del tópico MQTT
        estado  NVARCHAR(10) NOT NULL                            -- 'online' / 'offline'
    );
    CREATE INDEX idx_con_nodo_ts ON dbo.conexiones (nodo, ts);
END
"""

SQL_ULTIMO_ESTADO = """
    SELECT TOP 1 estado FROM dbo.conexiones WHERE nodo = %s ORDER BY ts DESC
"""

SQL_INSERT_ESTADO = """
    INSERT INTO dbo.conexiones (nodo, estado) VALUES (%s, %s)
"""

SQL_INSERT = """
    INSERT INTO dbo.lecturas (nodo, temperatura, humedad, luz, nivel_agua, alarma,
                              sensores, codigo, mensaje)
    VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s)
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
                    cur.execute(SQL_AGREGAR_COLUMNAS)
                    cur.execute(SQL_CREAR_TABLA_CONEXIONES)
            log(f"[SQL Server] Base '{BASE}' y tablas 'lecturas' y 'conexiones' listas")
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
        datos.get("sensores"),
        datos.get("codigo"),
        datos.get("mensaje"),
    )
    try:
        with conexion.cursor() as cur:
            cur.execute(SQL_INSERT, valores)
        log(f"[SQL Server] Guardado {nodo}: {valores[1:6]} | {valores[8]}")
    except pymssql.Error as e:
        log(f"[SQL Server] Error al guardar ({e}). Reconectando...")
        try:
            conexion.close()
        except Exception:
            pass
        conexion = conectar()


def guardar_estado(nodo, estado):
    """Guarda 'online'/'offline' solo si CAMBIÓ (el 'online' retenido llega
    otra vez cada vez que el puente se reconecta y no queremos repetirlo)."""
    global conexion
    try:
        with conexion.cursor() as cur:
            cur.execute(SQL_ULTIMO_ESTADO, (nodo,))
            fila = cur.fetchone()
            if fila and fila[0] == estado:
                return
            cur.execute(SQL_INSERT_ESTADO, (nodo, estado))
        log(f"[SQL Server] Conexión {nodo}: {estado.upper()}")
    except pymssql.Error as e:
        log(f"[SQL Server] Error al guardar estado ({e}). Reconectando...")
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
        log(f"[MQTT] Conectado. Suscrito a {TOPICO} y {TOPICO_ESTADO}")
        client.subscribe([(TOPICO, 0), (TOPICO_ESTADO, 1)])
    else:
        log(f"[MQTT] Rechazado: {reason_code}")


def al_recibir(client, userdata, msg):
    # invernadero/zona1/datos -> nodo "zona1", tipo "datos"
    partes = msg.topic.split("/")
    nodo = partes[1] if len(partes) >= 3 else "desconocido"
    tipo = partes[2] if len(partes) >= 3 else ""

    if tipo == "estado":
        estado = msg.payload.decode("utf-8", errors="ignore").strip().lower()
        if estado in ("online", "offline"):
            guardar_estado(nodo, estado)
        else:
            log(f"[MQTT] Estado desconocido en {msg.topic}: {msg.payload!r}")
        return

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
