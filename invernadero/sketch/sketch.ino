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
 *    2. Detecta qué sensores están conectados (DHT11, agua, luz y LCD).
 *    3. Compara CADA sensor detectado contra su rango (mínimo y máximo).
 *       Si alguno se sale: suena el buzzer y hay ALARMA.
 *    4. Mini interfaz en la LCD (rota las pantallas que apliquen):
 *         "!! ALARMA !! 1/2" / "Temp ALTA: 35C"     <- hay alarma
 *         "No se detecta:"   / "DHT11 Agua"         <- falta algún sensor
 *         "Arduino NO"       / "conectado a PC"     <- sin MQTT
 *         "Todo detectado"   / "con normalidad"     <- todo bien
 *       y después clima y agua/luz.
 *    5. Manda cada lectura al lado Linux por el Bridge:
 *         Bridge.notify("lectura", ...)
 *       El Python (python/main.py) arma el MISMO mensaje y lo publica por
 *       MQTT, para que Grafana "retransmita" lo que dice la LCD.
 *    6. Recibe del Linux si MQTT está conectado ("estado_mqtt").
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

// --- Detección de sensores ---
// DHT11 y LCD se detectan seguro (son digitales: contestan o no).
// Agua y luz son analógicos: se detecta si el pin está "al aire" jalándolo
// con las resistencias internas (pull-up y pull-down, seguras para la placa).
// Pin al aire  -> sigue al jalón -> diferencia grande (> UMBRAL_FLOTANTE).
// Con sensor   -> el sensor manda -> diferencia chica.
// Si marca "NO detectado" con el sensor puesto: mira "prueba alto/bajo"
// en el monitor y sube el umbral, o pon DETECTAR_ANALOGICOS = false.
const bool DETECTAR_ANALOGICOS = true;
const int  UMBRAL_FLOTANTE     = 300;     // en lectura cruda (0–1023)
const unsigned long TIMEOUT_MQTT = 15000; // sin aviso del Linux en 15 s = no conectado

// --- Tiempos (ms) ---
const unsigned long INTERVALO_LECTURA  = 5000;  // leer sensores
const unsigned long INTERVALO_PANTALLA = 3000;  // rotar pantallas sin alarma
const unsigned long INTERVALO_ALERTA   = 2000;  // rotar pantallas con alarma
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
volatile bool          mqttConectado   = false;
volatile unsigned long ultimoAvisoMqtt = 0;   // millis() del último aviso del Linux

// Sensores detectados (cambian solo si el resultado se repite 2 veces seguidas)
bool dhtDetectado  = false;  int seguidasDHT  = 0;
bool aguaDetectada = true;   int seguidasAgua = 0;
bool luzDetectada  = true;   int seguidasLuz  = 0;
bool primeraLectura = false;  // en la 1a lectura se acepta el resultado directo
int  pruebaAlto = 0, pruebaBajo = 0;   // última prueba de pin al aire (monitor)

unsigned long ultimaLectura        = 0;
unsigned long ultimoCambioPantalla = 0;
unsigned long ultimoPitido         = 0;
bool lcdPresente   = false;   // se revisa cada 5 s (se puede conectar después)
unsigned long ultimoLatido = 0;
bool estadoLed     = false;
int  paginaLCD     = 0;
int  firmaPrevia   = -1;      // para saber si cambió el estado general
bool estadoBuzzer  = false;


// ============================================================================
//  BLOQUE 3: COMUNICACIÓN CON LINUX (Bridge)
// ============================================================================

// Linux nos avisa si el MQTT está conectado.
// Corre en el hilo del Bridge: SOLO guardar el valor, nada de imprimir.
void recibirEstadoMqtt(bool conectado) {
  mqttConectado   = conectado;
  ultimoAvisoMqtt = millis();
}

// bit0 = DHT11, bit1 = agua, bit2 = luz, bit3 = LCD   (1 = detectado)
int bitsSensores() {
  return (dhtDetectado  ? 1 : 0) |
         (aguaDetectada ? 2 : 0) |
         (luzDetectada  ? 4 : 0) |
         (lcdPresente   ? 8 : 0);
}

// 2 bits por sensor: 0 = BAJO, 1 = OK, 2 = ALTO
// bits 0-1 temp, 2-3 humedad, 4-5 agua, 6-7 luz
int bitsAlertas() {
  return ((lectura.estadoTemp + 1) & 3)       |
         (((lectura.estadoHum  + 1) & 3) << 2) |
         (((lectura.estadoAgua + 1) & 3) << 4) |
         (((lectura.estadoLuz  + 1) & 3) << 6);
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
                lectura.dhtOk,
                bitsSensores(),
                bitsAlertas());
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

// Cambia 'detectado' solo si 'resultado' se repite 2 lecturas seguidas
// (evita que un error suelto haga parpadear "No se detecta").
void confirmarDeteccion(bool resultado, bool &detectado, int &seguidas) {
  if (primeraLectura || resultado == detectado) {
    detectado = resultado;
    seguidas = 0;
    return;
  }
  if (++seguidas >= 2) {
    detectado = resultado;
    seguidas = 0;
  }
}

// Prueba de pin "al aire" para sensores analógicos.
// Jala el pin a 3.3V y luego a GND con las resistencias INTERNAS (~40k,
// no dañan nada) y lee con el ADC justo después:
//   pin al aire -> la lectura sigue al jalón (alto ≈ 1000, bajo ≈ 0)
//   con sensor  -> el sensor manda; alto y bajo salen casi iguales
// analogRead() regresa solo el pin a modo analógico.
bool pinAnalogicoConectado(int pin) {
  pinMode(pin, INPUT_PULLUP);
  delay(2);
  pruebaAlto = analogRead(pin);
  pinMode(pin, INPUT_PULLDOWN);
  delay(2);
  pruebaBajo = analogRead(pin);
  return (pruebaAlto - pruebaBajo) < UMBRAL_FLOTANTE;
}

void imprimirDeteccion(int crudo, bool detectado) {
  Monitor.print(crudo);
  if (DETECTAR_ANALOGICOS) {
    Monitor.print("  prueba alto/bajo="); Monitor.print(pruebaAlto);
    Monitor.print("/");                   Monitor.print(pruebaBajo);
  }
  Monitor.println(detectado ? "  -> detectado" : "  -> NO detectado");
}

void leerDHT() {
  int t = 0, h = 0;
  int resultado = dht11.readTemperatureHumidity(t, h);
  confirmarDeteccion(resultado == 0, dhtDetectado, seguidasDHT);

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
  bool conectado = DETECTAR_ANALOGICOS ? pinAnalogicoConectado(PIN_AGUA) : true;
  confirmarDeteccion(conectado, aguaDetectada, seguidasAgua);

  int crudo = leerAnalogicoPromedio(PIN_AGUA);
  lectura.agua = aPorcentaje(crudo, AGUA_CRUDO_SECO, AGUA_CRUDO_LLENO);
  Monitor.print("[Agua] crudo="); imprimirDeteccion(crudo, aguaDetectada);
}

void leerLuz() {
  bool conectado = DETECTAR_ANALOGICOS ? pinAnalogicoConectado(PIN_LUZ) : true;
  confirmarDeteccion(conectado, luzDetectada, seguidasLuz);

  int crudo = leerAnalogicoPromedio(PIN_LUZ);
  if (LUZ_INVERTIDA) crudo = 1023 - crudo;   // para módulos que bajan con la luz
  lectura.luz = aPorcentaje(crudo, LUZ_CRUDO_OSCURO, LUZ_CRUDO_SOL);
  Monitor.print("[Luz ] crudo="); imprimirDeteccion(crudo, luzDetectada);
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
  // Un sensor que falló o no está conectado NO puede dar alarma
  // (su lectura sería ruido). Eso se avisa aparte: "No se detecta".
  lectura.estadoTemp = lectura.dhtOk ? evaluarRango(lectura.temperatura, TEMP_MIN, TEMP_MAX) : OK;
  lectura.estadoHum  = lectura.dhtOk ? evaluarRango(lectura.humedad,     HUM_MIN,  HUM_MAX)  : OK;
  lectura.estadoAgua = aguaDetectada ? evaluarRango(lectura.agua, AGUA_MIN, AGUA_MAX) : OK;
  lectura.estadoLuz  = (VIGILAR_LUZ && luzDetectada) ? evaluarRango(lectura.luz, LUZ_MIN, LUZ_MAX) : OK;

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

// ---------------------------------------------------------------------------
//  MINI INTERFAZ: estado general
// ---------------------------------------------------------------------------

// "Conectado" = el Linux dice que MQTT está conectado Y nos avisó hace poco
// (si la App de Python se cae, dejan de llegar avisos -> no conectado)
bool arduinoConectado() {
  return mqttConectado && (millis() - ultimoAvisoMqtt < TIMEOUT_MQTT);
}

int contarFaltantes() {
  return (dhtDetectado ? 0 : 1) + (aguaDetectada ? 0 : 1) + (luzDetectada ? 0 : 1);
}

// Ej: "DHT11 Agua Luz" (máx. 14 caracteres, cabe en la LCD)
void listaFaltantes(char* destino, size_t tam) {
  snprintf(destino, tam, "%s%s%s",
           dhtDetectado  ? "" : "DHT11 ",
           aguaDetectada ? "" : "Agua ",
           luzDetectada  ? "" : "Luz");
}

// Número que cambia cuando cambia algo importante (para refrescar la LCD ya)
int firmaEstado() {
  return (lectura.alarma      ? 1  : 0) |
         (dhtDetectado        ? 0  : 2) |
         (aguaDetectada       ? 0  : 4) |
         (luzDetectada        ? 0  : 8) |
         (arduinoConectado()  ? 0  : 16);
}

// Arma la pantalla número n. Devuelve false si ya no hay más pantallas.
// Orden (prioridad):
//   1. ALARMA (una pantalla por cada sensor fuera de rango)
//   2. "No se detecta: ..."          si falta algún sensor
//   3. "Arduino NO conectado a PC"   si no hay MQTT
//   4. "Todo detectado con normalidad" si nada de lo anterior
//   5. Clima y agua/luz (solo cuando NO hay alarma)
bool generarPagina(int n, char* l1, char* l2) {
  const size_t T = 17;
  int i = 0;

  if (lectura.alarma) {
    int total = contarAlertas();
    for (int k = 0; k < total; k++, i++) {
      if (i == n) {
        snprintf(l1, T, "!! ALARMA !! %d/%d", k + 1, total);
        if (!mensajeAlerta(k, l2, T)) snprintf(l2, T, "Revisa sensores");
        return true;
      }
    }
  }

  if (contarFaltantes() > 0) {
    if (i == n) {
      snprintf(l1, T, "No se detecta:");
      listaFaltantes(l2, T);
      return true;
    }
    i++;
  }

  if (!arduinoConectado()) {
    if (i == n) {
      snprintf(l1, T, "Arduino NO");
      snprintf(l2, T, "conectado a PC");
      return true;
    }
    i++;
  }

  if (lectura.alarma) return false;   // con alarma no se muestran datos normales

  if (contarFaltantes() == 0 && arduinoConectado()) {
    if (i == n) {
      snprintf(l1, T, "Todo detectado");
      snprintf(l2, T, "con normalidad");
      return true;
    }
    i++;
  }

  if (dhtDetectado && lectura.dhtOk) {
    if (i == n) {
      snprintf(l1, T, "Temp: %d C", lectura.temperatura);
      snprintf(l2, T, "Humedad: %d%%", lectura.humedad);
      return true;
    }
    i++;
  }

  if (aguaDetectada || luzDetectada) {
    if (i == n) {
      if (aguaDetectada) snprintf(l1, T, "Agua: %d%%", lectura.agua);
      else               snprintf(l1, T, "Agua: --");
      if (luzDetectada)  snprintf(l2, T, "Luz: %d%%", lectura.luz);
      else               snprintf(l2, T, "Luz: --");
      return true;
    }
    i++;
  }

  return false;
}

int contarPaginas() {
  char a[17], b[17];
  int n = 0;
  while (generarPagina(n, a, b)) n++;
  return n;
}

// Muestra la pantalla actual (paginaLCD). Si se pasó del final, vuelve a 0.
void mostrarPagina() {
  int total = contarPaginas();
  if (total == 0) return;
  if (paginaLCD >= total) paginaLCD = 0;

  char l1[17], l2[17];
  generarPagina(paginaLCD, l1, l2);
  escribirLinea(0, l1);
  escribirLinea(1, l2);
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
  Monitor.print("Detectados: DHT11="); Monitor.print(dhtDetectado);
  Monitor.print(" Agua="); Monitor.print(aguaDetectada);
  Monitor.print(" Luz="); Monitor.print(luzDetectada);
  Monitor.print(" LCD="); Monitor.print(lcdPresente);
  Monitor.print(" | Conectado a PC="); Monitor.println(arduinoConectado());
}


// ============================================================================
//  BLOQUE 8: "MAIN"  (setup + loop)
// ============================================================================

// Detecta si hay algo respondiendo en la dirección I2C de la LCD
bool detectarLCD() {
  Wire.beginTransmission(LCD_DIRECCION);
  return (Wire.endTransmission() == 0);
}

// Revisa cada 5 s si la LCD está: así se puede conectar después de prender
// (⚠ mejor conectar/desconectar cables con la placa APAGADA)
void vigilarLCD() {
  bool hay = detectarLCD();
  if (hay && !lcdPresente) {
    lcdPresente = true;
    lcd.init();
    lcd.backlight();
    firmaPrevia = -1;            // forzar a redibujar
    Monitor.println("[LCD] conectada");
  } else if (!hay && lcdPresente) {
    lcdPresente = false;
    Monitor.println("[LCD] desconectada");
  }
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
  primeraLectura = true;
  leerSensores();
  evaluarAlarma();
  enviarLectura();
  primeraLectura = false;
  firmaPrevia = firmaEstado();
  mostrarPagina();
  Monitor.println("[4] === MCU invernadero listo ===");
}

void loop() {
  unsigned long ahora = millis();

  // 1) Cada 5 s: revisar LCD -> leer -> evaluar -> mandar a Linux
  if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
    ultimaLectura = ahora;
    vigilarLCD();
    leerSensores();
    evaluarAlarma();
    imprimirResumen();
    enviarLectura();
  }

  // 2) Si cambió algo importante (alarma, sensores o conexión):
  //    mostrarlo YA, empezando por la primera pantalla
  int firma = firmaEstado();
  if (firma != firmaPrevia) {
    firmaPrevia = firma;
    paginaLCD = 0;
    ultimoCambioPantalla = ahora;
    mostrarPagina();
  }

  // 3) Rotar las pantallas (más rápido si hay alarma)
  unsigned long intervalo = lectura.alarma ? INTERVALO_ALERTA : INTERVALO_PANTALLA;
  if (ahora - ultimoCambioPantalla >= intervalo) {
    ultimoCambioPantalla = ahora;
    paginaLCD++;
    mostrarPagina();
  }

  // 4) Buzzer siempre (parpadeo fluido)
  controlarBuzzer();

  // 5) LED de la placa parpadeando = el programa está vivo
  latido();
}
