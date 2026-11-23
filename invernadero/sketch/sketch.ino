/*
 * ============================================================================
 *  sketch.ino  —  Lado MCU (STM32U585) del Arduino UNO Q
 *  Proyecto: Invernadero IoT — TC1004B Implementación del IoT
 * ----------------------------------------------------------------------------
 *  Componentes:
 *    - DHT11 ............ temperatura y humedad
 *    - Sensor de agua ... nivel de agua
 *    - Fotorresistor .... luz (energía solar disponible)
 *    - Buzzer ........... alarma
 *    - LCD 16x2 I2C ..... interfaz visual
 *
 *  Qué hace:
 *    1. Lee los sensores cada 5 s.
 *    2. Compara CADA sensor contra su rango permitido (mínimo y máximo).
 *       Si alguno se sale: suena el buzzer y la LCD muestra el aviso
 *       (ej. "Temp ALTA: 35C"). Si hay varias alertas, las va rotando.
 *    3. Sin alertas, la LCD rota entre clima, agua/luz y conexión.
 *    4. Manda cada lectura al lado Linux por el Bridge:
 *         Bridge.notify("lectura", ...)
 *       El Python (python/main.py) la recibe y la publica por MQTT.
 *    5. Recibe del Linux si MQTT está conectado ("estado_mqtt").
 *
 * ----------------------------------------------------------------------------
 *  CONEXIONES (UNO Q: pines a 3.3V)
 * ----------------------------------------------------------------------------
 *  DHT11          VCC -> 3.3V   GND -> GND   DATA -> D2
 *
 *  Sensor agua    +   -> 3.3V   -   -> GND   S    -> A0
 *
 *  Fotorresistor  Si es FOTORRESISTOR SUELTO (2 patas):
 *                   una pata -> 3.3V
 *                   otra pata -> A1  y de A1 una resistencia de 10k -> GND
 *                   (divisor de voltaje: más luz = valor más alto)
 *                 Si es MÓDULO de 3 pines (ej. KY-018):
 *                   VCC -> 3.3V   GND -> GND   S (o AO) -> A1
 *                   (muchos módulos dan MENOS valor con más luz:
 *                    si te pasa, pon LUZ_INVERTIDA = true)
 *
 *  Buzzer         + -> D8       - -> GND   (buzzer ACTIVO)
 *
 *  LCD I2C        VCC -> 5V  GND -> GND  SDA -> SDA  SCL -> SCL
 *                 (los pines SDA/SCL junto a AREF)
 *
 *  ⚠ Las entradas analógicas NO aguantan 5V: alimenta el sensor de agua y
 *    el fotorresistor con 3.3V, nunca con 5V.
 *
 * ----------------------------------------------------------------------------
 *  LIBRERÍAS
 *    - DHT11              (Dhruba Saha)      -> #include <DHT11.h>
 *    - LiquidCrystal I2C  (Frank de Brabander)
 *    - Arduino_RouterBridge viene incluida con el UNO Q
 * ============================================================================
 */

#include <Arduino_RouterBridge.h>
#include <DHT11.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>


// ============================================================================
//  BLOQUE 1: CONFIGURACIÓN
// ============================================================================

// --- Pines ---
#define PIN_DHT        2
#define PIN_AGUA       A0
#define PIN_LUZ        A1
#define PIN_BUZZER     8
#define LCD_DIRECCION  0x27   // si la LCD no muestra nada, prueba 0x3F

// --- Calibración (lectura cruda 0–1023) ---
// Mira los valores "crudo=" en los logs y ajusta con los tuyos.
const int  AGUA_CRUDO_SECO  = 0;      // sensor fuera del agua
const int  AGUA_CRUDO_LLENO = 700;    // sensor sumergido hasta la marca
const int  LUZ_CRUDO_OSCURO = 20;     // fotorresistor tapado con el dedo
const int  LUZ_CRUDO_SOL    = 1000;   // fotorresistor con lámpara/sol directo
const bool LUZ_INVERTIDA    = false;  // true si con más luz baja el valor

// --- RANGOS PERMITIDOS (fuera de aquí -> alarma) ---
//                 mínimo      máximo
const int TEMP_MIN = 15,  TEMP_MAX = 32;   // °C
const int HUM_MIN  = 40,  HUM_MAX  = 85;   // %
const int AGUA_MIN = 30,  AGUA_MAX = 100;  // %   (100 = nunca "alta")
const int LUZ_MIN  = 10,  LUZ_MAX  = 95;   // %

// La luz baja también es normal de noche. Si no quieren que suene de
// noche, pongan esto en false (se sigue midiendo y graficando).
const bool VIGILAR_LUZ = true;

// --- Tiempos (ms) ---
const unsigned long INTERVALO_LECTURA  = 5000;  // leer sensores
const unsigned long INTERVALO_PANTALLA = 3000;  // rotar pantallas normales
const unsigned long INTERVALO_ALERTA   = 2000;  // rotar mensajes de alerta
const unsigned long INTERVALO_PITIDO   = 300;   // parpadeo del buzzer


// ============================================================================
//  BLOQUE 2: OBJETOS Y VARIABLES GLOBALES
// ============================================================================

DHT11             dht11(PIN_DHT);
LiquidCrystal_I2C lcd(LCD_DIRECCION, 16, 2);

// Estado de cada sensor respecto a su rango
const int BAJO = -1;
const int OK   =  0;
const int ALTO =  1;

struct Lectura {
  int  temperatura = 0;   // °C (el DHT11 solo da enteros)
  int  humedad     = 0;   // %
  int  luz         = 0;   // %
  int  agua        = 0;   // %
  bool dhtOk       = false;

  // BAJO / OK / ALTO por sensor
  int  estadoTemp = OK;
  int  estadoHum  = OK;
  int  estadoAgua = OK;
  int  estadoLuz  = OK;

  // Variables de la función booleana (1 = fuera de rango)
  bool A = false;   // temperatura
  bool B = false;   // humedad
  bool C = false;   // agua
  bool D = false;   // luz
  bool alarma = false;
};
Lectura lectura;

// Lo escribe el Bridge desde otro hilo -> volatile
volatile bool mqttConectado = false;

unsigned long ultimaLectura        = 0;
unsigned long ultimoCambioPantalla = 0;
unsigned long ultimoCambioAlerta   = 0;
unsigned long ultimoPitido         = 0;
bool lcdPresente   = false;   // se detecta en setup()
unsigned long ultimoLatido = 0;
bool estadoLed     = false;
int  paginaLCD     = 0;
int  indiceAlerta  = 0;
bool estadoBuzzer  = false;
bool alarmaPrevia  = false;


// ============================================================================
//  BLOQUE 3: COMUNICACIÓN CON LINUX (Bridge)
// ============================================================================

// Linux nos avisa si el MQTT está conectado.
// Corre en el hilo del Bridge: SOLO guardar el valor, nada de imprimir.
void recibirEstadoMqtt(bool conectado) {
  mqttConectado = conectado;
}

// Manda la lectura completa al Python.
// ⚠ El ORDEN debe coincidir con recibir_lectura() en python/main.py
void enviarLectura() {
  Bridge.notify("lectura",
                lectura.temperatura,
                lectura.humedad,
                lectura.luz,
                lectura.agua,
                lectura.alarma,
                lectura.dhtOk);
}


// ============================================================================
//  BLOQUE 4: LECTURA DE SENSORES
// ============================================================================

int leerAnalogicoPromedio(int pin, int muestras = 10) {
  long suma = 0;
  for (int i = 0; i < muestras; i++) {
    suma += analogRead(pin);
    delay(2);
  }
  return suma / muestras;
}

int aPorcentaje(int crudo, int crudoMin, int crudoMax) {
  long p = map(crudo, crudoMin, crudoMax, 0, 100);
  return constrain(p, 0, 100);
}

void leerDHT() {
  int t = 0, h = 0;
  int resultado = dht11.readTemperatureHumidity(t, h);

  if (resultado != 0) {
    lectura.dhtOk = false;
    Monitor.print("[DHT11] Error: ");
    Monitor.println(DHT11::getErrorString(resultado));
    return;
  }
  lectura.temperatura = t;
  lectura.humedad     = h;
  lectura.dhtOk       = true;
}

void leerAgua() {
  int crudo = leerAnalogicoPromedio(PIN_AGUA);
  lectura.agua = aPorcentaje(crudo, AGUA_CRUDO_SECO, AGUA_CRUDO_LLENO);
  Monitor.print("[Agua] crudo="); Monitor.println(crudo);
}

void leerLuz() {
  int crudo = leerAnalogicoPromedio(PIN_LUZ);
  if (LUZ_INVERTIDA) crudo = 1023 - crudo;   // para módulos que bajan con la luz
  lectura.luz = aPorcentaje(crudo, LUZ_CRUDO_OSCURO, LUZ_CRUDO_SOL);
  Monitor.print("[Luz ] crudo="); Monitor.println(crudo);
}

void leerSensores() {
  leerDHT();
  leerAgua();
  leerLuz();
}


// ============================================================================
//  BLOQUE 5: LÓGICA DE ALARMA (función booleana para el reporte)
// ----------------------------------------------------------------------------
//  Cada variable vale 1 si su sensor está FUERA de rango:
//    A = (T < TEMP_MIN) + (T > TEMP_MAX)
//    B = (H < HUM_MIN)  + (H > HUM_MAX)
//    C = (Agua < AGUA_MIN) + (Agua > AGUA_MAX)
//    D = (Luz < LUZ_MIN)  + (Luz > LUZ_MAX)
//
//  ALARMA = A + B + C + D        (OR de 4 variables)
//  Equivalente:  ALARMA' = A'·B'·C'·D'
//  -> el buzzer solo calla cuando TODOS los sensores están en rango.
//
//   A B C D | ALARMA        A B C D | ALARMA
//   0 0 0 0 |   0           1 0 0 0 |   1
//   0 0 0 1 |   1           1 0 0 1 |   1
//   0 0 1 0 |   1           1 0 1 0 |   1
//   0 0 1 1 |   1           1 0 1 1 |   1
//   0 1 0 0 |   1           1 1 0 0 |   1
//   0 1 0 1 |   1           1 1 0 1 |   1
//   0 1 1 0 |   1           1 1 1 0 |   1
//   0 1 1 1 |   1           1 1 1 1 |   1
// ============================================================================

// Devuelve BAJO, OK o ALTO según el rango
int evaluarRango(int valor, int minimo, int maximo) {
  if (valor < minimo) return BAJO;
  if (valor > maximo) return ALTO;
  return OK;
}

void evaluarAlarma() {
  // Si el DHT11 falló no podemos juzgar temperatura/humedad
  lectura.estadoTemp = lectura.dhtOk ? evaluarRango(lectura.temperatura, TEMP_MIN, TEMP_MAX) : OK;
  lectura.estadoHum  = lectura.dhtOk ? evaluarRango(lectura.humedad,     HUM_MIN,  HUM_MAX)  : OK;
  lectura.estadoAgua = evaluarRango(lectura.agua, AGUA_MIN, AGUA_MAX);
  lectura.estadoLuz  = VIGILAR_LUZ ? evaluarRango(lectura.luz, LUZ_MIN, LUZ_MAX) : OK;

  lectura.A = (lectura.estadoTemp != OK);
  lectura.B = (lectura.estadoHum  != OK);
  lectura.C = (lectura.estadoAgua != OK);
  lectura.D = (lectura.estadoLuz  != OK);

  lectura.alarma = lectura.A || lectura.B || lectura.C || lectura.D;   // <-- función booleana
}

// Buzzer intermitente sin delay()
void controlarBuzzer() {
  if (!lectura.alarma) {
    estadoBuzzer = false;
    digitalWrite(PIN_BUZZER, LOW);
    return;
  }
  if (millis() - ultimoPitido >= INTERVALO_PITIDO) {
    ultimoPitido = millis();
    estadoBuzzer = !estadoBuzzer;
    digitalWrite(PIN_BUZZER, estadoBuzzer ? HIGH : LOW);
  }
}


// ============================================================================
//  BLOQUE 6: PANTALLA LCD
// ============================================================================

void escribirLinea(int fila, const char* texto) {
  if (!lcdPresente) return;     // sin LCD conectada: no hacer nada
  char linea[17];
  snprintf(linea, sizeof(linea), "%-16s", texto);
  lcd.setCursor(0, fila);
  lcd.print(linea);
}

const char* textoEstado(int estado) {
  return (estado == ALTO) ? "ALTA" : "BAJA";
}

// Escribe en 'destino' el mensaje de la alerta número 'n' (0, 1, 2...)
// Devuelve false si no existe esa alerta.
bool mensajeAlerta(int n, char* destino, size_t tam) {
  int i = 0;
  if (lectura.A) {
    if (i == n) { snprintf(destino, tam, "Temp %s: %dC", textoEstado(lectura.estadoTemp), lectura.temperatura); return true; }
    i++;
  }
  if (lectura.B) {
    if (i == n) { snprintf(destino, tam, "Humedad %s:%d%%", textoEstado(lectura.estadoHum), lectura.humedad); return true; }
    i++;
  }
  if (lectura.C) {
    if (i == n) { snprintf(destino, tam, "Agua %s: %d%%", textoEstado(lectura.estadoAgua), lectura.agua); return true; }
    i++;
  }
  if (lectura.D) {
    if (i == n) { snprintf(destino, tam, "Luz %s: %d%%", textoEstado(lectura.estadoLuz), lectura.luz); return true; }
    i++;
  }
  return false;
}

int contarAlertas() {
  return lectura.A + lectura.B + lectura.C + lectura.D;
}

// Pantalla de ALERTA: tiene prioridad sobre las normales
void mostrarAlerta() {
  int total = contarAlertas();
  if (indiceAlerta >= total) indiceAlerta = 0;

  char l1[17], l2[17];
  snprintf(l1, sizeof(l1), "!ALERTA! %d de %d", indiceAlerta + 1, total);
  if (!mensajeAlerta(indiceAlerta, l2, sizeof(l2))) {
    snprintf(l2, sizeof(l2), "Revisa sensores");
  }
  escribirLinea(0, l1);
  escribirLinea(1, l2);
}

// Pantallas normales (sin alertas)
void mostrarNormal() {
  char l1[17], l2[17];

  switch (paginaLCD) {
    case 0:  // Clima
      if (lectura.dhtOk) {
        snprintf(l1, sizeof(l1), "Temp: %d C", lectura.temperatura);
        snprintf(l2, sizeof(l2), "Humedad: %d%%", lectura.humedad);
      } else {
        snprintf(l1, sizeof(l1), "DHT11: error");
        snprintf(l2, sizeof(l2), "Revisa cable D2");
      }
      break;

    case 1:  // Agua y luz
      snprintf(l1, sizeof(l1), "Agua: %d%%", lectura.agua);
      snprintf(l2, sizeof(l2), "Luz: %d%%", lectura.luz);
      break;

    default: // Estado general
      snprintf(l1, sizeof(l1), "Todo en rango OK");
      snprintf(l2, sizeof(l2), "MQTT: %s", mqttConectado ? "conectado" : "sin conexion");
      break;
  }

  escribirLinea(0, l1);
  escribirLinea(1, l2);
}

void actualizarLCD() {
  if (lectura.alarma) mostrarAlerta();
  else                mostrarNormal();
}


// ============================================================================
//  BLOQUE 7: DEPURACIÓN
// ============================================================================

void imprimirResumen() {
  Monitor.println("----------------------------------------");
  Monitor.print("T="); Monitor.print(lectura.temperatura);
  Monitor.print("C H="); Monitor.print(lectura.humedad);
  Monitor.print("% Agua="); Monitor.print(lectura.agua);
  Monitor.print("% Luz="); Monitor.print(lectura.luz);
  Monitor.println("%");
  Monitor.print("A="); Monitor.print(lectura.A);
  Monitor.print(" B="); Monitor.print(lectura.B);
  Monitor.print(" C="); Monitor.print(lectura.C);
  Monitor.print(" D="); Monitor.print(lectura.D);
  Monitor.print(" -> ALARMA="); Monitor.println(lectura.alarma);

  char msg[17];
  for (int n = 0; mensajeAlerta(n, msg, sizeof(msg)); n++) {
    Monitor.print("  ! "); Monitor.println(msg);
  }
}


// ============================================================================
//  BLOQUE 8: "MAIN"  (setup + loop)
// ============================================================================

// Detecta si hay algo respondiendo en la dirección I2C de la LCD
bool detectarLCD() {
  Wire.beginTransmission(LCD_DIRECCION);
  return (Wire.endTransmission() == 0);
}

// LED de la placa: parpadea mientras el loop() esté vivo
void latido() {
#ifdef LED_BUILTIN
  if (millis() - ultimoLatido >= 500) {
    ultimoLatido = millis();
    estadoLed = !estadoLed;
    digitalWrite(LED_BUILTIN, estadoLed ? HIGH : LOW);
  }
#endif
}

void setup() {
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif

  // 1) Primero el Bridge y el Monitor, para poder ver mensajes desde ya
  //    (Bridge.begin() espera a que arranque el lado Linux)
  Bridge.begin();
  Monitor.begin();
  Bridge.provide("estado_mqtt", recibirEstadoMqtt);
  Monitor.println("[1] Bridge y Monitor OK");

  // 2) LCD: solo se usa si responde (sin LCD el programa sigue igual)
  Wire.begin();
  Monitor.println("[2] Wire (I2C) iniciado");
  lcdPresente = detectarLCD();
  if (lcdPresente) {
    lcd.init();
    lcd.backlight();
    Monitor.println("[3] LCD detectada");
  } else {
    Monitor.println("[3] LCD NO detectada (se sigue sin pantalla)");
  }
  escribirLinea(0, "Invernadero IoT");
  escribirLinea(1, "Bridge OK");

  // 3) Primera lectura inmediata
  leerSensores();
  evaluarAlarma();
  enviarLectura();
  actualizarLCD();
  alarmaPrevia = lectura.alarma;
  Monitor.println("[4] === MCU invernadero listo ===");
}

void loop() {
  unsigned long ahora = millis();

  // 1) Cada 5 s: leer -> evaluar -> mandar a Linux
  if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
    ultimaLectura = ahora;
    leerSensores();
    evaluarAlarma();
    imprimirResumen();
    enviarLectura();

    // Si la alarma acaba de activarse o apagarse, refrescar la LCD YA
    if (lectura.alarma != alarmaPrevia) {
      alarmaPrevia = lectura.alarma;
      indiceAlerta = 0;
      paginaLCD = 0;
      ultimoCambioAlerta = ahora;
      ultimoCambioPantalla = ahora;
      actualizarLCD();
    }
  }

  // 2) Rotar la LCD
  if (lectura.alarma) {
    if (ahora - ultimoCambioAlerta >= INTERVALO_ALERTA) {
      ultimoCambioAlerta = ahora;
      indiceAlerta = (indiceAlerta + 1) % max(1, contarAlertas());
      mostrarAlerta();
    }
  } else {
    if (ahora - ultimoCambioPantalla >= INTERVALO_PANTALLA) {
      ultimoCambioPantalla = ahora;
      paginaLCD = (paginaLCD + 1) % 3;
      mostrarNormal();
    }
  }

  // 3) Buzzer siempre (parpadeo fluido)
  controlarBuzzer();

  // 4) LED de la placa parpadeando = el programa está vivo
  latido();
}
