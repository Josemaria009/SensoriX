/*
  SensoriX <-> Wokwi  |  ESP32 + DHT22 + sensor de agua (analogico)
  Cada lectura se asocia al SITE por su NOMBRE (el mismo que registraste en SensoriX).
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DHT.h>

// ======== CONFIGURA ESTO ========
const char* SCRIPT_URL  = "https://script.google.com/macros/s/PEGA_AQUI_TU_ID/exec";
const char* API_KEY     = "sensorix123";        // igual que en Apps_Script_SensoriX.gs
const char* SITE_NOMBRE = "Sala Servidores 1";  // NOMBRE del SITE tal cual aparece en SensoriX
// ================================

const char* WIFI_SSID = "Wokwi-GUEST";          // red WiFi simulada de Wokwi
const char* WIFI_PASS = "";
const unsigned long INTERVALO_MS = 10000;       // cada cuanto se envia una lectura
const unsigned long REINTENTO_MS = 3000;        // espera entre reintentos si un envio falla
const int UMBRAL_ON  = 35;                      // % de nivel: por encima = AGUA DETECTADA
const int UMBRAL_OFF = 25;                      // % de nivel: por debajo = SECO

#define DHT_PIN   15
#define WATER_PIN 34                            // sensor de agua analogico (slider), pin ADC1
#define LED_PIN   2

struct Lectura { float t; float h; bool agua; int nivel; };

DHT dht(DHT_PIN, DHT22);
WiFiClientSecure client;                        // global: se reutiliza la conexion TLS (el handshake es lento en Wokwi)
HTTPClient http;
Lectura ultima = { 0, 0, false, 0 };
unsigned long ultimoEnvio = 0, proximoIntento = 0;
bool aguaPrev = false, pendiente = false, verificado = false;
int fallos = 0;

unsigned long wifiDesde = 0, ultimoPunto = 0;
bool wifiVisto = false;

void iniciarWiFi() {
  WiFi.disconnect(true);                        // reinicia la pila WiFi antes de cada intento
  delay(300);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  wifiDesde = millis();
  Serial.print("Conectando a WiFi");
}

// No bloquea: devuelve true si hay conexion. Si pasan 15 s sin conectar, reinicia el intento.
bool wifiListo() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiVisto) {
      wifiVisto = true;
      Serial.printf(" OK (%lu s) IP %s\n", (millis() - wifiDesde) / 1000, WiFi.localIP().toString().c_str());
    }
    return true;
  }
  if (wifiVisto) {                              // se cayo la conexion
    wifiVisto = false;
    client.stop();
    Serial.println("\nWiFi perdido, reconectando...");
    iniciarWiFi();
    return false;
  }
  unsigned long ahora = millis();
  if (ahora - ultimoPunto >= 1000) { ultimoPunto = ahora; Serial.print("."); }
  if (ahora - wifiDesde >= 15000) {
    Serial.printf("\nSin WiFi tras 15 s (estado %d). Si el icono WiFi de Wokwi dice Error, falla la red de Wokwi\n(Brave Shields, VPN o bloqueador). Reintentando...\n", WiFi.status());
    iniciarWiFi();
  }
  return false;
}

int leerNivel() {                               // nivel de agua 0-100 %
  int v = analogRead(WATER_PIN);
  return constrain(map(v, 0, 4095, 0, 100), 0, 100);
}

// Codifica el texto para la URL (espacios, acentos, enes...)
String urlEncode(const String& s) {
  String out;
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += ch;
    else { snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)ch); out += buf; }
  }
  return out;
}

// Se ejecuta tras 3 fallos seguidos para saber DONDE falla
void diagnostico() {
  client.stop();
  String host = String(SCRIPT_URL).substring(8);   // quita "https://"
  host = host.substring(0, host.indexOf('/'));
  IPAddress ip;
  Serial.printf("  WiFi: %s | IP: %s\n", WiFi.status() == WL_CONNECTED ? "conectado" : "SIN CONEXION", WiFi.localIP().toString().c_str());
  Serial.printf("  DNS %s: %s\n", host.c_str(), WiFi.hostByName(host.c_str(), ip) == 1 ? ip.toString().c_str() : "FALLO");
  Serial.printf("  Memoria libre: %u bytes\n", ESP.getFreeHeap());
  WiFiClient p;
  Serial.printf("  TCP a %s:80: %s\n", host.c_str(), p.connect(host.c_str(), 80) ? "OK" : "FALLO");
  p.stop();
  WiFiClientSecure c;
  c.setInsecure();
  c.setHandshakeTimeout(30);
  unsigned long t0 = millis();
  bool tls = c.connect(host.c_str(), 443);
  Serial.printf("  TLS a %s:443: %s (%lu ms)\n", host.c_str(), tls ? "OK" : "FALLO", millis() - t0);
  c.stop();
  Serial.println("  TCP OK pero TLS FALLO = Wokwi no logra el cifrado con Google.");
  Serial.println("  Todo OK = revisa SCRIPT_URL: https://script.google.com/macros/s/.../exec");
}

// Solo la primera vez: lee la respuesta del puente para confirmar que la clave es correcta
void verificar(const String& loc) {
  client.stop();                                // libera memoria TLS antes de abrir otra conexion
  WiFiClientSecure c2;
  c2.setInsecure();
  c2.setHandshakeTimeout(30);
  HTTPClient h2;
  h2.setConnectTimeout(10000);
  h2.setTimeout(10000);
  if (h2.begin(c2, loc) && h2.GET() == 200) Serial.printf("Puente -> %s\n", h2.getString().c_str());
  else Serial.println("(No se pudo leer la confirmacion del puente, pero la lectura fue enviada)");
  h2.end();
  verificado = true;
}

// Un solo intento, una sola peticion. Devuelve true si el puente recibio la lectura.
bool enviarUnaVez(const Lectura& l) {
  String url = String(SCRIPT_URL) + "?action=push&key=" + urlEncode(API_KEY) +
               "&site=" + urlEncode(SITE_NOMBRE) + "&temp=" + String(l.t, 1) +
               "&hum=" + String(l.h, 1) + "&water=" + (l.agua ? "1" : "0") + "&lvl=" + String(l.nivel);
  const char* cab[] = { "Location" };
  http.collectHeaders(cab, 1);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);  // Apps Script ya guardo la lectura antes de redirigir
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  if (!http.begin(client, url)) { Serial.println("SCRIPT_URL invalida"); return false; }
  int code = http.GET();
  String loc, cuerpo;
  if (code > 0) { loc = http.header("Location"); cuerpo = http.getString(); }
  http.end();                                   // la conexion queda abierta para el siguiente envio

  if (code <= 0) {
    Serial.printf("HTTP %d -> %s (reintento en %lu s)\n", code, HTTPClient::errorToString(code).c_str(), REINTENTO_MS / 1000);
    client.stop();                              // el siguiente intento abre una conexion limpia
    if (++fallos == 3) diagnostico();
    return false;
  }
  fallos = 0;
  if (code == 200) { Serial.printf("HTTP 200 -> %s\n", cuerpo.c_str()); return cuerpo.indexOf("\"ok\":true") >= 0; }
  if (code == 301 || code == 302 || code == 303 || code == 307) {
    if (loc.indexOf("accounts.google.com") >= 0) {
      Serial.println("ERROR: el Web App NO es publico. Implementar > Nueva implementacion > Acceso: Cualquier persona.");
      return false;
    }
    if (!verificado) verificar(loc);
    return true;
  }
  Serial.printf("HTTP %d -> revisa SCRIPT_URL y el acceso del Web App\n", code);
  return false;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  dht.begin();
  client.setInsecure();                         // en Wokwi no se valida el certificado
  client.setHandshakeTimeout(30);
  String u = SCRIPT_URL;
  if (!u.startsWith("https://script.google.com/") || !u.endsWith("/exec") || u.indexOf("PEGA_AQUI") >= 0)
    Serial.println("AVISO: SCRIPT_URL no parece valida (https://script.google.com/macros/s/.../exec)");
  Serial.printf("SITE: \"%s\"\n", SITE_NOMBRE);
  Serial.printf("Sensor de agua: %d %% (AGUA si >= %d %%, SECO si <= %d %%)\n", leerNivel(), UMBRAL_ON, UMBRAL_OFF);
  iniciarWiFi();
}

void loop() {
  bool online = wifiListo();                    // el sensor y el LED siguen funcionando aunque no haya WiFi

  int nivel = leerNivel();
  bool agua = aguaPrev;                         // histeresis: evita parpadeos cerca del umbral
  if (nivel >= UMBRAL_ON) agua = true;
  else if (nivel <= UMBRAL_OFF) agua = false;
  digitalWrite(LED_PIN, agua);

  unsigned long ahora = millis();
  if (agua != aguaPrev || ultimoEnvio == 0 || ahora - ultimoEnvio >= INTERVALO_MS) {
    aguaPrev = agua;                            // lectura nueva (o cambio de agua = envio inmediato)
    ultimoEnvio = ahora;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h)) Serial.println("Error leyendo el DHT22");
    else { ultima.t = t; ultima.h = h; pendiente = true; proximoIntento = 0; }
  }

  if (online && pendiente && ahora >= proximoIntento) {
    ultima.agua = agua;                         // siempre se envia el estado de agua mas reciente
    ultima.nivel = nivel;
    if (enviarUnaVez(ultima)) {
      pendiente = false;
      Serial.printf("T=%.1f C  H=%.1f %%  Agua=%s (%d %%)  -> enviado\n", ultima.t, ultima.h, agua ? "SI" : "NO", nivel);
    } else proximoIntento = millis() + REINTENTO_MS;
  }
  delay(50);
}
