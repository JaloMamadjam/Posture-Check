/*****************************************************
 *  ESP32 – SISTEMA DE MONITORAMENTO DE POSTURA
 *  MQTT + HiveMQ Cloud (TLS)
 *****************************************************/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

/* ================= HARDWARE ================= */
#define LED_VERDE     2
#define LED_AMARELO   4
#define LED_VERMELHO  5
#define BUZZER        18
#define BOTAO_CALIB   19

/* ================= WIFI ================= */
const char* ssid     = "Wokwi-GUEST";
const char* password = "";

/* ================= MQTT (HiveMQ Cloud) ================= */
const char* mqtt_server = "4b0ca456ff7e462f88c310fffd260e40.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "esp32";
const char* mqtt_pass   = "Posture123";

#define TOPIC_DATA   "posture/esp32/data"
#define TOPIC_CMD    "posture/esp32/command"
#define TOPIC_STATUS "posture/esp32/status"

/* ================= POSTURA ================= */
#define TOLERANCIA    12.0
#define TEMPO_ATENCAO 3000
#define TEMPO_CRITICO 6000

/* ================= OBJETOS ================= */
WiFiClientSecure espClient;
PubSubClient client(espClient);
Adafruit_MPU6050 mpu;

/* ================= ESTADOS ================= */
enum EstadoPostura {
  ST_OK,
  ST_ATENCAO,
  ST_CRITICO
};

const char* nomesEstados[] = {
  "OK",
  "ATENCAO",
  "CRITICO"
};

EstadoPostura estadoAtual = ST_OK;

/* ================= VARIÁVEIS ================= */
float anguloIdeal = 0.0;
bool sistemaCalibrado = false;
unsigned long tempoInicioErro = 0;
unsigned long ultimoEnvio = 0;

/* ================= PROTÓTIPOS ================= */
void setupWiFi();
void reconnectMQTT();
void callback(char* topic, byte* payload, unsigned int length);
float lerAngulo();
void logicaPostura(float atual);
void atualizarSaidas(int v, int a, int verm, bool som);

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_AMARELO, OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BOTAO_CALIB, INPUT_PULLUP);

  if (!mpu.begin()) {
    Serial.println("❌ MPU6050 não encontrado");
    while (1);
  }

  setupWiFi();

  espClient.setInsecure();       // TLS sem certificado
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setKeepAlive(60);       // 🔒 estabilidade MQTT

  Serial.println("✅ Sistema pronto");
}

/* ================= LOOP ================= */
void loop() {
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  float anguloAtual = lerAngulo();

  // 📏 Calibração por botão
  if (digitalRead(BOTAO_CALIB) == LOW) {
    anguloIdeal = anguloAtual;
    sistemaCalibrado = true;
    Serial.println("📏 Calibrado via botão");
    delay(500);
  }

  if (!sistemaCalibrado) return;

  logicaPostura(anguloAtual);

  // 📡 Envio MQTT a cada 5s
  if (millis() - ultimoEnvio > 5000) {
    unsigned long tempoErro =
      (tempoInicioErro > 0) ? (millis() - tempoInicioErro) : 0;

    String payload = "{";
    payload += "\"angulo\":" + String(anguloAtual, 2) + ",";
    payload += "\"estado\":\"" + String(nomesEstados[estadoAtual]) + "\",";
    payload += "\"tempo_erro\":" + String(tempoErro);
    payload += "}";

    client.publish(TOPIC_DATA, payload.c_str());
    Serial.println("📡 MQTT enviado: " + payload);

    ultimoEnvio = millis();
  }

  delay(50);
}

/* ================= WIFI ================= */
void setupWiFi() {
  Serial.print("🔌 Conectando Wi-Fi...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n✅ Wi-Fi conectado");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

/* ================= MQTT ================= */
void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("🔄 MQTT...");
    String clientId = "ESP32-POSTURA-";
    clientId += String(random(0xffff), HEX);

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      client.subscribe(TOPIC_CMD);
      Serial.println(" conectado");
    } else {
      Serial.print(" falhou rc=");
      Serial.println(client.state());
      delay(3000);
    }
  }
}

/* ================= CALLBACK MQTT ================= */
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("📩 MQTT recebido: ");
  Serial.println(msg);

  if (String(topic) == TOPIC_CMD && msg.indexOf("calibrate") != -1) {
    anguloIdeal = lerAngulo();
    sistemaCalibrado = true;

    client.publish(TOPIC_STATUS, "{\"status\":\"calibrado\"}");
    Serial.println("🎯 Calibrado via dashboard");
  }
}

/* ================= SENSOR ================= */
float lerAngulo() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  return atan2(a.acceleration.y, a.acceleration.z) * 57.2958;
}

/* ================= LÓGICA POSTURA ================= */
void logicaPostura(float atual) {
  float erro = abs(atual - anguloIdeal);

  if (erro <= TOLERANCIA) {
    estadoAtual = ST_OK;
    tempoInicioErro = 0;
    atualizarSaidas(HIGH, LOW, LOW, false);
  } else {
    if (tempoInicioErro == 0) tempoInicioErro = millis();
    unsigned long duracao = millis() - tempoInicioErro;

    if (duracao > TEMPO_CRITICO) {
      estadoAtual = ST_CRITICO;
      atualizarSaidas(LOW, LOW, HIGH, true);
    } else if (duracao > TEMPO_ATENCAO) {
      estadoAtual = ST_ATENCAO;
      atualizarSaidas(LOW, HIGH, LOW, false);
    }
  }
}

/* ================= SAÍDAS ================= */
void atualizarSaidas(int v, int a, int verm, bool som) {
  digitalWrite(LED_VERDE, v);
  digitalWrite(LED_AMARELO, a);
  digitalWrite(LED_VERMELHO, verm);

  if (som) {
    if ((millis() % 400) < 200)
      tone(BUZZER, 1000);
    else
      noTone(BUZZER);
  } else {
    noTone(BUZZER);
  }
}
