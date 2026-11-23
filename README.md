# Invernadero IoT — TC1004B

Monitoreo de un invernadero con **Arduino UNO Q**: temperatura y humedad (DHT11),
nivel de agua, luz (fotorresistor), buzzer de alarma y LCD 16x2.
Los datos viajan por **MQTT**, se guardan en **SQL Server** (el contenedor `mssql` de la clase)
y se ven en **Grafana**. Todo el servidor corre en contenedores de **Docker Desktop**.

```
 Sensores ──► MCU STM32 del UNO Q (invernadero/sketch/sketch.ino)
                 │  Bridge.notify("lectura", ...)
                 ▼
          Linux del UNO Q: invernadero/python/main.py (contenedor de la App)
                 │  MQTT  invernadero/zona1/datos
                 ▼
          Tu compu — Docker Desktop
                ├─ mosquitto  (broker MQTT, puerto 1883)      ┐
                ├─ puente     (MQTT -> SQL Server)            ├ servidor/docker-compose.yml
                ├─ grafana    (dashboard, puerto 3000)        ┘
                └─ mssql      (SQL Server 2022, puerto 1433)  ← contenedor de la clase
```

## Estructura

| Carpeta | Qué es | Dónde corre |
|---|---|---|
| `invernadero/sketch/` | Código C++ del microcontrolador (sensores, buzzer, LCD) | STM32 del UNO Q |
| `invernadero/python/` | Recibe las lecturas por el Bridge y las publica por MQTT | Linux del UNO Q |
| `servidor/` | Mosquitto + puente + Grafana (la base es el contenedor `mssql`) | Docker Desktop |
| `servidor/sql/` | Script de la tabla `lecturas` en T-SQL (el puente la crea solo) | — |

## Primera vez

1. **VS Code:** crea la carpeta `.vscode` en la raíz del repo y mueve ahí los archivos de
   `PARA_.vscode/` (`tasks.json`, `c_cpp_properties.json`, `settings.json`). Borra `PARA_.vscode`.
2. **Contraseñas:** en `servidor/config.env` pon la contraseña de `sa` del contenedor `mssql`
   (`MSSQL_PASSWORD`) y la de Grafana. `config.env` está en `.gitignore`, así que no se sube a GitHub.
   Pruébala con la tarea **SQL Server: probar contraseña de sa**.
3. **Docker Desktop:** ábrelo y espera a que diga *Engine running*.
4. **Firewall de Windows** (una vez, en PowerShell **como administrador**), para que la placa
   pueda llegar a Mosquitto en tu compu:
   ```powershell
   New-NetFirewallRule -DisplayName "Mosquitto MQTT 1883" -Direction Inbound -Protocol TCP -LocalPort 1883 -Action Allow
   ```
5. **IP de tu compu:** tarea **W3) Ver la IP de mi compu**. Pon el primer número que salga en
   `MQTT_BROKER` dentro de `invernadero/python/main.py`.
6. **Servidor:** tarea **3) Servidor: levantar en Docker Desktop**. Prende `mssql` y levanta
   Mosquitto, puente y Grafana. El puente crea solo la base `invernadero` y la tabla `lecturas`.
7. **STM32 (una vez):** tareas **S0) grabar loader zephyr 1.1.0** y **0b) dejar la App solo con Python**.
   Así el sketch se graba siempre con el core 1.1.0 de la clase y no con el de la placa (0.55.2).
8. **Sketch:** tarea **S1) Sketch: compilar y grabar en el STM32** (también `Ctrl+Shift+B`).
   LED3 parpadea si el programa corre.
9. **Python en la placa:** tarea **1) App (Python): subir e iniciar**.

## Verificar

- **2) App: ver logs del Python** → debe decir `[MQTT] Conectado` y `[MQTT] -> invernadero/zona1/datos`.
- **2b) App: ver monitor del sketch** → mensajes del microcontrolador.
- **4) MQTT: escuchar todo** → los JSON llegando al broker.
- **Servidor: logs del puente** → `[SQL Server] Guardado zona1: ...`
- **5) SQL Server: últimas 10 lecturas** → filas en la tabla `lecturas`.

## Grafana

Abre `http://localhost:3000` (usuario/contraseña de `config.env`). La fuente
**SQLServer-Invernadero** ya viene configurada. Ejemplo de panel *Time series* (modo *Code*):

```sql
SELECT ts AS time, temperatura AS "Temperatura °C"
FROM lecturas
WHERE nodo = 'zona1' AND $__timeFilter(ts)
ORDER BY ts;
```
Repite con `humedad`, `luz` y `nivel_agua`. Panel *Stat* para la alarma:
```sql
SELECT TOP 1 CAST(alarma AS INT) AS alarma FROM lecturas WHERE nodo='zona1' ORDER BY ts DESC;
```

## Lógica de alarma (función booleana)

Cada variable vale 1 si su sensor está fuera de rango (los rangos se editan en el Bloque 1 de `sketch.ino`):

- A = temperatura fuera de 15–32 °C
- B = humedad fuera de 40–85 %
- C = agua < 30 %
- D = luz fuera de 10–95 %

**ALARMA = A + B + C + D**  ⇔  **ALARMA' = A'·B'·C'·D'**

## Conexiones (UNO Q, pines de 3.3 V)

| Componente | Pines |
|---|---|
| DHT11 | VCC 3.3V · GND · DATA → D2 |
| Sensor de agua | + 3.3V · − GND · S → A0 |
| Fotorresistor | 3.3V → LDR → A1 → 10k → GND |
| Buzzer (activo) | + → D8 · − → GND |
| LCD I2C | VCC 5V · GND · SDA → SDA · SCL → SCL |

⚠ A0 y A1 **no** aguantan 5 V. Desconecta el USB antes de cablear.

## Problemas comunes

| Síntoma | Causa probable |
|---|---|
| Logs: `[MQTT] Sin conexión` | IP mal en `main.py`, Docker Desktop apagado o falta la regla del firewall |
| Puente: `[SQL Server] No disponible` | Prende `mssql` en Docker Desktop o revisa `MSSQL_PASSWORD` en `config.env` |
| STM32 no corre el sketch (LED3 quieto) | Loader distinto al core: corre **S0** y luego **S1** |
| LCD prende sin texto | Cambia `0x27` por `0x3F` o gira el potenciómetro azul |
