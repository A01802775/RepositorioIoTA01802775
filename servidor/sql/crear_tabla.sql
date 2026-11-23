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

-- Consultas útiles
-- SELECT TOP 10 * FROM dbo.lecturas ORDER BY id DESC;
-- SELECT nodo, AVG(temperatura) AS temp_prom, MAX(temperatura) AS temp_max
--   FROM dbo.lecturas GROUP BY nodo;
