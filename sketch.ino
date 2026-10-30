/*
 * Pelé Academia - Monitoramento de Ambiente de Treino (Edge Computing)
 * Sprint 4 - Edge Computing & Computer Systems
 *
 * ESP32 lê temperatura e umidade (DHT22), decide LOCALMENTE se o ambiente
 * está em Normal ou Alerta, mostra tudo num display OLED (IHM local) e
 * envia os dados para o Blynk (acompanhamento remoto).
 *
 * Se a nuvem cair, o ESP32 continua lendo, decidindo e atualizando o OLED.
 * O botão do circuito simula a queda da internet para demonstrar isso.
 */

// ===== Credenciais do Blynk (copie do seu Template no Blynk Console) =====
#define BLYNK_TEMPLATE_ID   "TMPL248VYL7Qk"
#define BLYNK_TEMPLATE_NAME "Pele Academia Edge"
#define BLYNK_AUTH_TOKEN    "iVMOMxfZxEZO2aorF27qPIvicJH_sFim"

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ===== Wi-Fi do Wokwi =====
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// ===== Pinos =====
#define PINO_DHT        15
#define PINO_LED_VERDE  26
#define PINO_LED_VERM   27
#define PINO_BUZZER     25
#define PINO_BOTAO       4   // simula queda da nuvem

// ===== Sensor e display =====
#define TIPO_DHT DHT22
DHT dht(PINO_DHT, TIPO_DHT);

#define LARGURA_OLED 128
#define ALTURA_OLED   64
Adafruit_SSD1306 oled(LARGURA_OLED, ALTURA_OLED, &Wire, -1);

// ===== Regra de decisão =====
float limiteTemperatura = 30.0;   // acima disso: ALERTA (calor no treino)
const float HISTERESE   = 0.5;    // evita ficar piscando perto do limite

// ===== Estado do sistema =====
float temperatura = NAN;
float umidade     = NAN;
bool  emAlerta    = false;
bool  modoOffline = false;        // true = nuvem desligada pelo botão

// ===== Temporizadores (millis, sem delay) =====
const unsigned long INTERVALO_LEITURA = 2000;
const unsigned long INTERVALO_ENVIO   = 2000;
const unsigned long INTERVALO_RECONEX = 10000;
const unsigned long INTERVALO_BIP     = 1000;
unsigned long ultimaLeitura   = 0;
unsigned long ultimoEnvio     = 0;
unsigned long ultimaTentWifi  = 0;
unsigned long ultimaTentBlynk = 0;
unsigned long ultimoBip       = 0;

bool estadoBotaoAnterior = HIGH;
unsigned long ultimoClique = 0;

// ---------------------------------------------------------------------------
// BORDA: leitura do sensor
// ---------------------------------------------------------------------------
void lerSensor() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    Serial.println("[BORDA] Falha ao ler o DHT22");
    return;
  }
  temperatura = t;
  umidade = h;
}

// ---------------------------------------------------------------------------
// BORDA: tomada de decisão local (não depende da nuvem)
// ---------------------------------------------------------------------------
void decidir() {
  if (isnan(temperatura)) return;

  if (!emAlerta && temperatura >= limiteTemperatura) {
    emAlerta = true;
    Serial.printf("[BORDA] ALERTA: %.1f C >= limite %.1f C\n", temperatura, limiteTemperatura);
  } else if (emAlerta && temperatura < limiteTemperatura - HISTERESE) {
    emAlerta = false;
    Serial.printf("[BORDA] Normal: %.1f C\n", temperatura);
  }
}

// ---------------------------------------------------------------------------
// BORDA: acionamento dos atuadores
// ---------------------------------------------------------------------------
void acionarAtuadores() {
  digitalWrite(PINO_LED_VERDE, emAlerta ? LOW : HIGH);
  digitalWrite(PINO_LED_VERM,  emAlerta ? HIGH : LOW);

  if (emAlerta) {
    if (millis() - ultimoBip >= INTERVALO_BIP) {
      ultimoBip = millis();
      tone(PINO_BUZZER, 1000, 200);   // bipe curto a cada 1 s
    }
  } else {
    noTone(PINO_BUZZER);
  }
}

// ---------------------------------------------------------------------------
// BORDA: IHM local (display OLED)
// ---------------------------------------------------------------------------
void atualizarOLED() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("PELE ACADEMIA - CT");

  oled.setTextSize(2);
  oled.setCursor(0, 12);
  if (isnan(temperatura)) {
    oled.print("--.- C");
  } else {
    oled.printf("%.1f C", temperatura);
  }

  oled.setTextSize(1);
  oled.setCursor(0, 32);
  if (isnan(umidade)) {
    oled.print("Umid: --%");
  } else {
    oled.printf("Umid: %.0f%%", umidade);
  }
  oled.setCursor(72, 32);
  oled.printf("Lim:%.1f", limiteTemperatura);

  oled.setCursor(0, 44);
  oled.print("Status: ");
  oled.print(emAlerta ? "ALERTA" : "NORMAL");

  oled.setCursor(0, 56);
  oled.print("Nuvem: ");
  if (modoOffline)            oled.print("OFF (simulado)");
  else if (Blynk.connected()) oled.print("ONLINE");
  else                        oled.print("OFFLINE");

  oled.display();
}

// ---------------------------------------------------------------------------
// NUVEM: envio dos dados para o Blynk
// ---------------------------------------------------------------------------
void enviarParaBlynk() {
  if (modoOffline || !Blynk.connected() || isnan(temperatura)) return;

  Blynk.virtualWrite(V0, temperatura);
  Blynk.virtualWrite(V1, umidade);
  Blynk.virtualWrite(V2, emAlerta ? "ALERTA" : "NORMAL");
  Blynk.virtualWrite(V3, emAlerta ? 1 : 0);
}

// Ao conectar, informa ao painel o limite que está valendo no ESP32
BLYNK_CONNECTED() {
  Serial.println("[NUVEM] Conectado ao Blynk");
  Blynk.virtualWrite(V4, limiteTemperatura);
}

// Slider do Blynk pode ajustar o limite; o valor fica guardado no ESP32
BLYNK_WRITE(V4) {
  limiteTemperatura = param.asFloat();
  Serial.printf("[NUVEM] Novo limite recebido: %.1f C\n", limiteTemperatura);
}

// ---------------------------------------------------------------------------
// NUVEM: conexão Wi-Fi/Blynk sem travar o loop
// ---------------------------------------------------------------------------
void gerenciarNuvem() {
  if (modoOffline) return;

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - ultimaTentWifi >= INTERVALO_RECONEX) {
      ultimaTentWifi = millis();
      Serial.println("[NUVEM] Tentando reconectar Wi-Fi...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
    }
    return;
  }

  if (!Blynk.connected()) {
    if (millis() - ultimaTentBlynk >= INTERVALO_RECONEX) {
      ultimaTentBlynk = millis();
      Serial.println("[NUVEM] Tentando conectar ao Blynk...");
      Blynk.connect(2000);
    }
    return;
  }

  Blynk.run();
}

// ---------------------------------------------------------------------------
// Botão: liga/desliga a nuvem para demonstrar a operação na borda
// ---------------------------------------------------------------------------
void verificarBotao() {
  bool estado = digitalRead(PINO_BOTAO);
  if (estado == LOW && estadoBotaoAnterior == HIGH && millis() - ultimoClique > 300) {
    ultimoClique = millis();
    modoOffline = !modoOffline;

    if (modoOffline) {
      Serial.println("[TESTE] Nuvem DESLIGADA - ESP32 segue operando na borda");
      Blynk.disconnect();
      WiFi.disconnect();
    } else {
      Serial.println("[TESTE] Nuvem RELIGADA - reconectando...");
      ultimaTentWifi = 0;
      ultimaTentBlynk = 0;
      WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
    }
  }
  estadoBotaoAnterior = estado;
}

void setup() {
  Serial.begin(115200);

  pinMode(PINO_LED_VERDE, OUTPUT);
  pinMode(PINO_LED_VERM, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP);

  dht.begin();

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Falha ao iniciar o OLED");
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("Iniciando...");
  oled.display();

  // A nuvem é configurada, mas o ESP32 NÃO espera ela conectar para funcionar
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  ultimaTentWifi = millis();
  Blynk.config(BLYNK_AUTH_TOKEN);
}

void loop() {
  verificarBotao();

  // ----- BORDA: funciona sempre, com ou sem internet -----
  if (millis() - ultimaLeitura >= INTERVALO_LEITURA) {
    ultimaLeitura = millis();
    lerSensor();
    decidir();
    atualizarOLED();
  }
  acionarAtuadores();

  // ----- NUVEM: só quando houver conexão -----
  gerenciarNuvem();
  if (millis() - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = millis();
    enviarParaBlynk();
  }
}
