/* =====================================================================
   ChargeGrid Intelligence — Estação de recarga solar-first
   Challenge GoodWe 2026 · FIAP · Grupo 2
   ESP32 + Wokwi + Firebase Realtime Database

   Vaga 1: veículo REAL (telemetria do Android via Battery Status API)
   Vagas 2-4: simuladas por potenciômetro
   ===================================================================== */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

/* ============ CONFIGURE AQUI ============ */
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const int   WIFI_CH   = 6;

const char* DB_URL = "https://SEU-PROJETO-default-rtdb.firebaseio.com";
/* ======================================== */

/* ---------- pinos (todos ADC1: ADC2 é bloqueado pelo Wi-Fi) ---------- */
#define PIN_SOLAR 34
const int PIN_SOC[4] = { 35, 32, 33, 39 };
const int PIN_BTN[4] = {  4,  5, 18, 19 };
#define PIN_LED   13
#define N_LEDS     8

/* ---------- parâmetros do modelo ---------- */
const float P_DISJUNTOR   = 40.0;   // kW — limite físico da entrada
const float P_REDE_PONTA  =  8.0;   // kW — teto de rede em horário de ponta
const float P_SOLAR_MAX   = 25.0;   // kW — geração de pico (escala 1:1000)
const float P_VAGA_MAX    = 11.0;   // kW — potência nominal por vaga
const float P_PISO        =  3.7;   // kW — ninguém fica parado
const float CAP_BATERIA   = 60.0;   // kWh — capacidade do "veículo"

const float TARIFA_PONTA  = 2.80;   // R$/kWh
const float TARIFA_REDE   = 2.20;
const float TARIFA_SOLAR  = 1.60;   // a tarifa segue o sol

const unsigned long TELEMETRIA_TTL = 15000;  // ms até considerar offline

/* ---------- identidade dos veículos (análogo ao EVCCID) ---------- */
const char* EVCCID[4] = {
  "A4F21B8C039E", "B7C3D91E2048", "C2E58A47F1B3", "D9014FA6C72E"
};

/* ---------- estado ---------- */
struct Vaga {
  bool     conectada  = false;
  bool     concluida  = false;
  int      soc        = 0;
  float    demanda    = 0;
  float    alocado    = 0;
  float    energia    = 0;   // kWh
  float    solarKwh   = 0;
  float    redeKwh    = 0;
  float    valor      = 0;
  uint32_t inicioMs   = 0;
  float    fase       = 0;   // animação do LED
};
Vaga vaga[4];

float solarKW      = 0;
float pLimite      = P_DISJUNTOR;
float pTotal       = 0;
float fracaoSolar  = 0;
bool  horarioPonta = false;
bool  deficitAtivo = false;

int  socRemoto = -1;
bool plugadoRemoto = false;
unsigned long ultimaTelemetria = 0;
bool telemetriaViva = false;

uint32_t contadorSessao = 0;
uint32_t contadorAlerta = 0;

/* ---------- objetos ---------- */
Adafruit_NeoPixel strip(N_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClientSecure  net;
Preferences       prefs;

unsigned long tCiclo = 0, tRede = 0, tPub = 0, tHist = 0, tLcd = 0;
bool alternaLeitura = false;

/* =====================================================================
   SETUP
   ===================================================================== */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[ChargeGrid] iniciando...");

  for (int i = 0; i < 4; i++) pinMode(PIN_BTN[i], INPUT_PULLUP);
  analogReadResolution(12);

  strip.begin();
  strip.setBrightness(120);
  strip.show();

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("ChargeGrid");
  lcd.setCursor(0, 1);
  lcd.print("conectando...");

  prefs.begin("chargegrid", false);
  contadorSessao = prefs.getUInt("sessao", 0);
  contadorAlerta = prefs.getUInt("alerta", 0);

  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CH);
  while (WiFi.status() != WL_CONNECTED) { delay(150); Serial.print("."); }
  Serial.printf("\n[WiFi] ok  IP %s\n", WiFi.localIP().toString().c_str());

  net.setInsecure();          // demo: sem validação de certificado

  lcd.clear();
  lcd.print("Estacao online");
}

/* =====================================================================
   ENTRADAS
   ===================================================================== */
void lerBotoes() {
  static bool ultimo[4] = { HIGH, HIGH, HIGH, HIGH };
  static unsigned long tUlt[4] = { 0, 0, 0, 0 };

  for (int i = 0; i < 4; i++) {
    bool agora = digitalRead(PIN_BTN[i]);
    if (agora != ultimo[i] && millis() - tUlt[i] > 60) {
      tUlt[i] = millis();
      ultimo[i] = agora;
      if (agora == LOW) {
        // vaga 1 obedece à telemetria quando ela está viva
        if (i == 0 && telemetriaViva) continue;
        if (vaga[i].conectada) desconectar(i);
        else                   conectar(i);
      }
    }
  }
}

void conectar(int i) {
  vaga[i] = Vaga();
  vaga[i].conectada = true;
  vaga[i].inicioMs  = millis();
  Serial.printf("[vaga %d] veiculo %s conectado\n", i + 1, EVCCID[i]);
}

void desconectar(int i) {
  if (vaga[i].conectada && vaga[i].energia > 0.01) enviarSessao(i);
  vaga[i] = Vaga();
  Serial.printf("[vaga %d] liberada\n", i + 1);
}

void lerSensores() {
  solarKW = (analogRead(PIN_SOLAR) / 4095.0) * P_SOLAR_MAX;

  for (int i = 0; i < 4; i++) {
    if (i == 0 && telemetriaViva) {
      vaga[0].soc = socRemoto;                      // veículo real
      if (plugadoRemoto && !vaga[0].conectada)  conectar(0);
      if (!plugadoRemoto &&  vaga[0].conectada) desconectar(0);
    } else {
      int leitura = 0;
      for (int k = 0; k < 5; k++) leitura += analogRead(PIN_SOC[i]);  // média móvel
      vaga[i].soc = map(leitura / 5, 0, 4095, 0, 100);
    }
  }
}

/* =====================================================================
   MODELO — demanda, rateio e energia
   ===================================================================== */
float demandaPorSoc(int soc) {
  if (soc >= 100) return 0.0;      // sessão concluída
  if (soc >=  95) return 2.0;      // taper CC-CV
  if (soc >=  80) return 7.4;      // tensão constante
  return P_VAGA_MAX;               // corrente constante
}

void calcularAlocacao() {
  pLimite = P_DISJUNTOR;
  if (horarioPonta) pLimite = min(P_DISJUNTOR, solarKW + P_REDE_PONTA);

  float demandaTotal = 0;
  int   ativas = 0;
  for (int i = 0; i < 4; i++) {
    vaga[i].alocado = 0;
    vaga[i].concluida = (vaga[i].conectada && vaga[i].soc >= 100);
    if (vaga[i].conectada && !vaga[i].concluida) {
      vaga[i].demanda = demandaPorSoc(vaga[i].soc);
      demandaTotal += vaga[i].demanda;
      ativas++;
    } else {
      vaga[i].demanda = 0;
    }
  }

  if (ativas == 0) { pTotal = 0; verificarDeficit(false); return; }

  if (demandaTotal <= pLimite) {
    for (int i = 0; i < 4; i++) vaga[i].alocado = vaga[i].demanda;
    verificarDeficit(false);
  } else {
    // prioriza quem tem menos bateria
    int ordem[4], n = 0;
    for (int i = 0; i < 4; i++)
      if (vaga[i].conectada && !vaga[i].concluida) ordem[n++] = i;
    for (int a = 0; a < n - 1; a++)
      for (int b = a + 1; b < n; b++)
        if (vaga[ordem[b]].soc < vaga[ordem[a]].soc) {
          int t = ordem[a]; ordem[a] = ordem[b]; ordem[b] = t;
        }

    float piso = min(P_PISO, pLimite / n);
    float restante = pLimite - piso * n;
    for (int k = 0; k < n; k++) {
      int i = ordem[k];
      float extra = min(vaga[i].demanda - piso, restante);
      if (extra < 0) extra = 0;
      vaga[i].alocado = piso + extra;
      restante -= extra;
    }
    verificarDeficit(true);
  }

  pTotal = 0;
  for (int i = 0; i < 4; i++) pTotal += vaga[i].alocado;
  fracaoSolar = (pTotal > 0.01) ? min(1.0f, solarKW / pTotal) : 0;
}

void acumularEnergia(float dtSegundos) {
  float h = dtSegundos / 3600.0;
  float tarifa = horarioPonta ? TARIFA_PONTA
               : (fracaoSolar > 0.5 ? TARIFA_SOLAR : TARIFA_REDE);

  for (int i = 0; i < 4; i++) {
    if (vaga[i].alocado <= 0) continue;
    float kwh = vaga[i].alocado * h;
    vaga[i].energia  += kwh;
    vaga[i].solarKwh += kwh * fracaoSolar;
    vaga[i].redeKwh  += kwh * (1.0 - fracaoSolar);
    vaga[i].valor    += kwh * tarifa;
  }
}

int minutosRestantes(int i) {
  if (vaga[i].alocado <= 0.01) return -1;
  float falta = (100 - vaga[i].soc) / 100.0 * CAP_BATERIA;   // kWh
  return (int)((falta / vaga[i].alocado) * 60.0);
}

const char* statusVaga(int i) {
  if (!vaga[i].conectada) return "livre";
  if (vaga[i].concluida)  return "concluida";
  if (vaga[i].alocado > 0) return "carregando";
  return "ocupada";
}

/* =====================================================================
   LEDS — 2 por vaga, velocidade proporcional à potência
   ===================================================================== */
void atualizarLeds() {
  for (int i = 0; i < 4; i++) {
    uint8_t r = 0, g = 0, b = 0;
    float brilhoA = 1.0, brilhoB = 1.0;

    if (!vaga[i].conectada) {
      r = 0; g = 60; b = 170;                       // azul: livre
      float p = (sin(millis() / 900.0) + 1) / 2;
      brilhoA = brilhoB = 0.25 + p * 0.45;
    } else if (vaga[i].concluida) {
      r = 0; g = 170; b = 70;                       // verde fixo
    } else if (vaga[i].alocado > 0) {
      if (fracaoSolar > 0.5) { r = 0;   g = 180; b = 90; }  // solar
      else                   { r = 232; g = 163; b = 61; }  // rede
      float vel = 0.05 + (vaga[i].alocado / P_VAGA_MAX) * 0.45;
      vaga[i].fase += vel;
      brilhoA = 0.30 + (sin(vaga[i].fase) + 1) / 2 * 0.70;
      brilhoB = 0.30 + (sin(vaga[i].fase + PI) + 1) / 2 * 0.70;
    } else {
      r = 214; g = 69; b = 60;                      // vermelho: sem potência
    }

    strip.setPixelColor(i * 2,     strip.Color(r * brilhoA, g * brilhoA, b * brilhoA));
    strip.setPixelColor(i * 2 + 1, strip.Color(r * brilhoB, g * brilhoB, b * brilhoB));
  }
  strip.show();
}

void atualizarLcd() {
  char l1[17], l2[17];
  snprintf(l1, 17, "%4.1f/%4.1fkW %s", pTotal, pLimite, horarioPonta ? "PT" : "  ");
  int ativas = 0;
  for (int i = 0; i < 4; i++) if (vaga[i].conectada) ativas++;
  snprintf(l2, 17, "Vagas %d/4  %s", ativas, fracaoSolar > 0.5 ? "SOLAR" : "REDE ");
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
}

/* =====================================================================
   FIREBASE
   ===================================================================== */
bool requisicao(const char* metodo, const String& caminho,
                const String& corpo, String& resposta) {
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(6000);
  if (!http.begin(net, String(DB_URL) + caminho)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(metodo, (uint8_t*)corpo.c_str(), corpo.length());
  bool ok = (code == 200);
  if (ok) resposta = http.getString();
  http.end();
  return ok;
}

/* Um único PATCH na raiz: evita repetir handshake TLS a cada nó. */
void publicarEstado() {
  StaticJsonDocument<3072> doc;

  JsonObject est = doc.createNestedObject("estacao");
  est["entrada_kw"]        = P_DISJUNTOR;
  est["limite_atual_kw"]   = round(pLimite * 10) / 10.0;
  est["geracao_solar_kw"]  = round(solarKW * 10) / 10.0;
  est["potencia_total_kw"] = round(pTotal * 10) / 10.0;
  est["fracao_solar"]      = round(fracaoSolar * 100) / 100.0;
  est["fonte_predominante"]= fracaoSolar > 0.5 ? "solar" : "rede";
  est["horario_ponta"]     = horarioPonta;
  est["veiculo_real"]      = telemetriaViva;
  est["uptime_s"]          = millis() / 1000;

  JsonObject vs = doc.createNestedObject("vagas");
  for (int i = 0; i < 4; i++) {
    JsonObject v = vs.createNestedObject(String("vaga") + (i + 1));
    v["status"]         = statusVaga(i);
    v["veiculo_id"]     = vaga[i].conectada ? EVCCID[i] : "";
    v["potencia_kw"]    = round(vaga[i].alocado * 10) / 10.0;
    v["potencia_max_kw"]= P_VAGA_MAX;
    v["soc"]            = vaga[i].soc;
    v["energia_kwh"]    = round(vaga[i].energia * 1000) / 1000.0;
    v["valor"]          = round(vaga[i].valor * 100) / 100.0;
    v["previsao_min"]   = vaga[i].conectada ? minutosRestantes(i) : -1;
    v["fonte_real"]     = (i == 0 && telemetriaViva);
  }

  String corpo, resp;
  serializeJson(doc, corpo);
  if (!requisicao("PATCH", "/.json", corpo, resp))
    Serial.println("[firebase] falha ao publicar");
}

void lerTelemetria() {
  String resp;
  if (!requisicao("GET", "/telemetria/vaga1.json", "", resp)) return;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, resp) || doc.isNull()) return;

  int  s = doc["soc"]     | -1;
  bool p = doc["plugado"] | false;
  if (s >= 0) {
    socRemoto = s;
    plugadoRemoto = p;
    ultimaTelemetria = millis();
    if (!telemetriaViva) Serial.println("[telemetria] veiculo real online");
  }
}

void lerComandos() {
  String resp;
  if (!requisicao("GET", "/comandos.json", "", resp)) return;

  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, resp) || doc.isNull()) return;

  for (JsonPair kv : doc.as<JsonObject>()) {
    JsonObject c = kv.value().as<JsonObject>();
    if (c["lido"] | true) continue;

    String alvo  = kv.key().c_str();
    String acao  = c["acao"] | "";
    float  valor = c["valor"] | 0.0;

    if (alvo == "estacao") {
      if (acao == "ponta") horarioPonta = (valor > 0.5);
      Serial.printf("[comando] ponta = %d\n", horarioPonta);
    } else {
      int i = alvo.substring(4).toInt() - 1;
      if (i >= 0 && i < 4) {
        if      (acao == "parar")   desconectar(i);
        else if (acao == "iniciar") conectar(i);
        else if (acao == "limitar") vaga[i].demanda = valor;
        Serial.printf("[comando] %s -> %s\n", alvo.c_str(), acao.c_str());
      }
    }
    String r;
    requisicao("PATCH", "/comandos/" + alvo + ".json", "{\"lido\":true}", r);
  }
}

void enviarSessao(int i) {
  StaticJsonDocument<384> doc;
  doc["vaga"]        = String("vaga") + (i + 1);
  doc["veiculo_id"]  = EVCCID[i];
  doc["energia_kwh"] = round(vaga[i].energia * 1000) / 1000.0;
  doc["solar_kwh"]   = round(vaga[i].solarKwh * 1000) / 1000.0;
  doc["rede_kwh"]    = round(vaga[i].redeKwh * 1000) / 1000.0;
  doc["valor"]       = round(vaga[i].valor * 100) / 100.0;
  doc["duracao_s"]   = (millis() - vaga[i].inicioMs) / 1000;
  doc["soc_final"]   = vaga[i].soc;

  String corpo, resp;
  serializeJson(doc, corpo);
  if (requisicao("POST", "/sessoes.json", corpo, resp)) {
    contadorSessao++;
    prefs.putUInt("sessao", contadorSessao);
    Serial.printf("[sessao %u] %.2f kWh  R$ %.2f\n",
                  contadorSessao, vaga[i].energia, vaga[i].valor);
  }
}

void verificarDeficit(bool emDeficit) {
  if (emDeficit && !deficitAtivo) {
    StaticJsonDocument<320> doc;
    doc["tipo"]       = "demanda_acima_do_limite";
    doc["severidade"] = "aviso";
    doc["mensagem"]   = String("Demanda excede ") + pLimite +
                        " kW disponiveis. Potencia rateada entre as vagas.";
    doc["uptime_s"]   = millis() / 1000;
    String corpo, resp;
    serializeJson(doc, corpo);
    if (requisicao("POST", "/alertas.json", corpo, resp)) {
      contadorAlerta++;
      prefs.putUInt("alerta", contadorAlerta);
    }
    Serial.println("[alerta] rateio ativado");
  }
  deficitAtivo = emDeficit;
}

void enviarHistorico() {
  StaticJsonDocument<256> doc;
  doc["potencia_kw"] = round(pTotal * 10) / 10.0;
  doc["solar_kw"]    = round(solarKW * 10) / 10.0;
  doc["limite_kw"]   = round(pLimite * 10) / 10.0;
  doc["uptime_s"]    = millis() / 1000;
  String corpo, resp;
  serializeJson(doc, corpo);
  requisicao("POST", "/historico.json", corpo, resp);
}

/* =====================================================================
   LOOP
   ===================================================================== */
void loop() {
  unsigned long agora = millis();

  // telemetria expirada -> volta ao potenciômetro sozinho
  telemetriaViva = (agora - ultimaTelemetria < TELEMETRIA_TTL) && socRemoto >= 0;

  if (agora - tCiclo >= 200) {
    float dt = (agora - tCiclo) / 1000.0;
    tCiclo = agora;

    lerBotoes();
    lerSensores();
    calcularAlocacao();
    acumularEnergia(dt);
    atualizarLeds();
  }

  if (agora - tLcd >= 500)  { tLcd  = agora; atualizarLcd(); }

  if (agora - tRede >= 1000) {
    tRede = agora;
    alternaLeitura = !alternaLeitura;
    if (alternaLeitura) lerTelemetria();
    else                lerComandos();
  }

  if (agora - tPub  >= 2000)  { tPub  = agora; publicarEstado(); }
  if (agora - tHist >= 30000) { tHist = agora; enviarHistorico(); }
}
