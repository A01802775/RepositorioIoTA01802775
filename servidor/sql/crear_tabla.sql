-- =============================================================================
--  Base de datos del invernadero en SQL Server (contenedor mssql)
--
--  NO hace falta correr esto a mano: puente.py lo hace solo al arrancar.
--  Sirve como documentación para el reporte, o para crearla manualmente desde
--  la extensión "SQL Server (mssql)" de VS Code o con sqlcmd.
--  Las horas (ts) se guardan en UTC; Grafana las muestra en tu hora local.
-- =============================================================================

IF DB_ID(N'invernadero') IS NULL
    CREATE DATABASE invernadero;
GO

USE invernadero;
GO

IF OBJECT_ID(N'dbo.lecturas', N'U') IS NULL
BEGIN
    CREATE TABLE dbo.lecturas (
        id           BIGINT IDENTITY(1,1) PRIMARY KEY,
        ts           DATETIME2(3) NOT NULL DEFAULT SYSUTCDATETIME(),
        nodo         NVARCHAR(20) NOT NULL,      -- sale del tópico MQTT (zona1...)
        temperatura  FLOAT NULL,                 -- °C (NULL si falló el DHT11)
        humedad      FLOAT NULL,                 -- %
        luz          TINYINT NULL,               -- % (fotorresistor)
        nivel_agua   TINYINT NULL,               -- %
        alarma       BIT NOT NULL DEFAULT 0
    );
    CREATE INDEX idx_nodo_ts ON dbo.lecturas (nodo, ts);
END
GO

-- Columnas de la mini interfaz de la LCD (puente.py las agrega solo)
IF COL_LENGTH(N'dbo.lecturas', N'mensaje') IS NULL
    ALTER TABLE dbo.lecturas ADD
        sensores  TINYINT       NULL,   -- bits: 1=DHT11 2=agua 4=luz 8=LCD
        codigo    TINYINT       NULL,   -- 0=OK, 2=sensor no detectado, 3=alarma
        mensaje   NVARCHAR(200) NULL;   -- mismo texto que la LCD
GO

-- Historial de conexión del Arduino (lo llena puente.py desde invernadero/<nodo>/estado)
-- 'online'  = la placa se conectó a Mosquitto
-- 'offline' = Mosquitto la dio por perdida (Last Will: se apagó o perdió el WiFi)
IF OBJECT_ID(N'dbo.conexiones', N'U') IS NULL
BEGIN
    CREATE TABLE dbo.conexiones (
        id      BIGINT IDENTITY(1,1) PRIMARY KEY,
        ts      DATETIME2(3) NOT NULL DEFAULT SYSUTCDATETIME(),
        nodo    NVARCHAR(20) NOT NULL,
        estado  NVARCHAR(10) NOT NULL
    );
    CREATE INDEX idx_con_nodo_ts ON dbo.conexiones (nodo, ts);
END
GO

-- Consultas útiles
-- SELECT TOP 10 * FROM dbo.conexiones ORDER BY id DESC;
-- SELECT TOP 10 * FROM dbo.lecturas ORDER BY id DESC;
-- SELECT nodo, AVG(temperatura) AS temp_prom, MAX(temperatura) AS temp_max
--   FROM dbo.lecturas GROUP BY nodo;
