// ═══════════════════════════════════════════════════════════════════
// ROBÔ CONVERSACIONAL — ESP32-S3
// Hardware: XIAO ESP32-S3 + INMP441 + MAX98357A
// Backend:  Pipedream (Whisper STT → Claude AI → OpenAI TTS)
// ═══════════════════════════════════════════════════════════════════

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <SPIFFS.h>
#include "AudioFileSourceSPIFFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// ───────────────────────────────────────────────────────────────────
// CONFIGURAÇÕES — EDITE AQUI
// ───────────────────────────────────────────────────────────────────
const char* WIFI_SSID       = "SEU_WIFI";
const char* WIFI_PASSWORD   = "SUA_SENHA_WIFI";
const char* PIPEDREAM_URL   = "https://eoXXXXXXXXXX.m.pipedream.net";
const char* ROBOT_TOKEN     = "meu_token_secreto_aqui";  // mesmo valor do Pipedream env

// ───────────────────────────────────────────────────────────────────
// PINOS
// ───────────────────────────────────────────────────────────────────
#define BTN_PIN      0    // Botão push-to-talk (INPUT_PULLUP → LOW ao pressionar)
#define LED_PIN      21   // LED onboard XIAO ESP32-S3

// Microfone INMP441 — I2S porta 0 (RX)
#define MIC_SCK      7
#define MIC_WS       8
#define MIC_SD       9

// Alto-falante MAX98357A — I2S porta 1 (TX)
#define SPK_BCLK     4
#define SPK_LRC      5
#define SPK_DIN      6

// ───────────────────────────────────────────────────────────────────
// CONSTANTES DE ÁUDIO
// ───────────────────────────────────────────────────────────────────
#define SAMPLE_RATE      16000   // 16 kHz — qualidade adequada para voz
#define BITS_PER_SAMPLE  16      // PCM 16-bit signed
#define MAX_RECORD_SEC   8       // Tempo máximo de gravação em segundos
// Buffer: 16000 amostras/s × 8s × 2 bytes = 256 KB → vai para PSRAM
#define AUDIO_BUF_BYTES  (SAMPLE_RATE * MAX_RECORD_SEC * 2)

// Limiar de silêncio para parada automática
// RMS abaixo desse valor por 1,5s encerra a gravação automaticamente
#define SILENCE_THRESHOLD   300.0f
#define SILENCE_WINDOWS     15    // 15 janelas de ~100ms = 1,5s de silêncio

// ───────────────────────────────────────────────────────────────────
// VARIÁVEIS GLOBAIS
// ───────────────────────────────────────────────────────────────────
int16_t* audioBuffer = nullptr;   // Buffer de gravação (PSRAM)

// Objetos de reprodução MP3
AudioGeneratorMP3*    mp3       = nullptr;
AudioFileSourceSPIFFS* source   = nullptr;
AudioOutputI2S*       i2sOut    = nullptr;

// Máquina de estados
enum State { IDLE, RECORDING, SENDING, PLAYING, ERR };
State state = IDLE;

// Controle de LED não bloqueante
unsigned long lastLedToggle = 0;
bool ledState = false;

// ═══════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n╔══════════════════════════════════╗");
  Serial.println("║   ROBÔ CONVERSACIONAL v1.0      ║");
  Serial.println("╚══════════════════════════════════╝");

  // ── Pinos ──────────────────────────────────────────────────────
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // ── PSRAM ──────────────────────────────────────────────────────
  // O buffer de áudio (256 KB) PRECISA estar na PSRAM
  // Se falhar: verifique Board Settings → PSRAM = OPI PSRAM
  audioBuffer = (int16_t*) ps_malloc(AUDIO_BUF_BYTES);
  if (!audioBuffer) {
    Serial.println("[ERRO FATAL] Falha ao alocar PSRAM!");
    Serial.println("[ERRO FATAL] Configure: Tools → PSRAM → OPI PSRAM");
    blinkForever(200);  // Pisca rápido para indicar erro
  }
  Serial.printf("[RAM] Buffer de áudio: %d bytes na PSRAM\n", AUDIO_BUF_BYTES);

  // ── SPIFFS ─────────────────────────────────────────────────────
  // Armazena o arquivo /resp.mp3 recebido do Pipedream
  if (!SPIFFS.begin(true)) {
    Serial.println("[ERRO FATAL] Falha ao montar SPIFFS!");
    blinkForever(500);
  }
  Serial.printf("[SPIFFS] Espaço total: %d KB | Usado: %d KB\n",
    SPIFFS.totalBytes() / 1024, SPIFFS.usedBytes() / 1024);

  // ── Wi-Fi ──────────────────────────────────────────────────────
  connectWiFi();

  // ── I2S Microfone ──────────────────────────────────────────────
  setupMicI2S();

  // ── Watchdog de software ───────────────────────────────────────
  // Reinicia o ESP32 se o loop() ficar travado por mais de 30s
  // (requer esp_task_wdt.h — descomente se necessário)
  // esp_task_wdt_init(30, true);
  // esp_task_wdt_add(NULL);

  Serial.println("[OK] Sistema pronto. Pressione o botão para falar.");
  // Tom de inicialização: LED pisca 3 vezes
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(100);
  }
}

// ═══════════════════════════════════════════════════════════════════
// LOOP PRINCIPAL — máquina de estados
// ═══════════════════════════════════════════════════════════════════
void loop() {
  // esp_task_wdt_reset();  // Descomente se usar watchdog

  updateLED();

  // Reconecta Wi-Fi automaticamente se cair
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Conexão perdida. Reconectando...");
    connectWiFi();
  }

  switch (state) {

    // ── IDLE: aguarda botão ──────────────────────────────────────
    case IDLE:
      if (digitalRead(BTN_PIN) == LOW) {
        delay(50);  // debounce
        if (digitalRead(BTN_PIN) == LOW) {
          Serial.println("\n[IDLE→RECORDING] Botão pressionado");
          state = RECORDING;
        }
      }
      break;

    // ── RECORDING: grava áudio via I2S ──────────────────────────
    case RECORDING: {
      size_t bytesRecorded = recordAudio();
      if (bytesRecorded > 1000) {  // Mínimo ~30ms de áudio
        state = SENDING;
        sendAndPlay(bytesRecorded);
      } else {
        Serial.println("[RECORDING] Áudio muito curto, ignorando.");
        state = IDLE;
      }
      break;
    }

    // ── SENDING: gerenciado dentro de sendAndPlay() ──────────────
    case SENDING:
      // Esse estado é transitório, gerenciado dentro de sendAndPlay()
      break;

    // ── PLAYING: ESP8266Audio processa em fatias no loop ────────
    case PLAYING:
      if (mp3 && mp3->isRunning()) {
        if (!mp3->loop()) {
          mp3->stop();
          Serial.println("[PLAYING→IDLE] Reprodução finalizada\n");
          cleanupAudio();
          state = IDLE;
        }
      } else {
        cleanupAudio();
        state = IDLE;
      }
      break;

    // ── ERRO: aguarda 5s e volta ao IDLE ────────────────────────
    case ERR:
      Serial.println("[ERR] Aguardando 5s para retry...");
      delay(5000);
      state = IDLE;
      break;
  }
}

// ═══════════════════════════════════════════════════════════════════
// CONEXÃO WI-FI
// ═══════════════════════════════════════════════════════════════════
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // Desativa power saving — melhora latência
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("[WIFI] Conectando a '%s'", WIFI_SSID);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" OK!\n[WIFI] IP: %s | RSSI: %d dBm\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("\n[WIFI] FALHA! Verifique SSID e senha.");
  }
}

// ═══════════════════════════════════════════════════════════════════
// CONFIGURAÇÃO I2S — MICROFONE (I2S_NUM_0, modo RX)
// ═══════════════════════════════════════════════════════════════════
void setupMicI2S() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,   // INMP441 com L/R=GND
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,      // 8 buffers DMA em fila
    .dma_buf_len          = 1024,   // 1024 amostras por buffer DMA
    .use_apll             = true,   // Clock PLL de áudio — maior precisão
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };

  i2s_pin_config_t pins = {
    .bck_io_num   = MIC_SCK,
    .ws_io_num    = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_SD
  };

  ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
  i2s_zero_dma_buffer(I2S_NUM_0);

  Serial.println("[I2S] Microfone INMP441 configurado (I2S_NUM_0)");
}

// ═══════════════════════════════════════════════════════════════════
// GRAVAÇÃO DE ÁUDIO
// Para quando: botão é solto OU timeout de MAX_RECORD_SEC OU silêncio
// Retorna: número de bytes gravados no audioBuffer
// ═══════════════════════════════════════════════════════════════════
size_t recordAudio() {
  Serial.println("[REC] Gravando... (solte o botão para parar)");

  size_t totalBytes = 0;
  size_t bytesRead  = 0;
  uint8_t* ptr      = (uint8_t*)audioBuffer;
  unsigned long startMs = millis();

  // Controle de silêncio automático
  int silenceCount = 0;
  const int WINDOW_SAMPLES = SAMPLE_RATE / 10;  // 1600 amostras = 100ms
  int16_t windowBuf[WINDOW_SAMPLES];
  size_t windowBytes = WINDOW_SAMPLES * 2;

  while (true) {
    // Para se botão foi solto
    if (digitalRead(BTN_PIN) == HIGH) {
      Serial.println("[REC] Botão solto — parando gravação");
      break;
    }
    // Para se atingiu timeout máximo
    if ((millis() - startMs) >= (unsigned long)(MAX_RECORD_SEC * 1000)) {
      Serial.println("[REC] Timeout máximo atingido");
      break;
    }
    // Para se buffer está cheio
    if (totalBytes + windowBytes > AUDIO_BUF_BYTES) {
      Serial.println("[REC] Buffer cheio");
      break;
    }

    // Lê uma janela de amostras
    i2s_read(I2S_NUM_0, windowBuf, windowBytes, &bytesRead, portMAX_DELAY);
    if (bytesRead == 0) continue;

    // Copia janela para o buffer principal
    memcpy(ptr + totalBytes, windowBuf, bytesRead);
    totalBytes += bytesRead;

    // Calcula RMS da janela para detecção de silêncio
    float rms = calculateRMS(windowBuf, bytesRead / 2);
    if (rms < SILENCE_THRESHOLD) {
      silenceCount++;
      if (silenceCount >= SILENCE_WINDOWS) {
        Serial.println("[REC] Silêncio detectado — parando gravação");
        break;
      }
    } else {
      silenceCount = 0;  // Reseta contador ao detectar voz
    }
  }

  float duration = (float)totalBytes / (SAMPLE_RATE * 2);
  Serial.printf("[REC] Gravados: %d bytes | Duração: %.1fs\n", totalBytes, duration);
  return totalBytes;
}

// RMS (Root Mean Square) — mede a energia do sinal de áudio
float calculateRMS(int16_t* buf, size_t count) {
  if (count == 0) return 0;
  double sum = 0;
  for (size_t i = 0; i < count; i++) {
    sum += (double)buf[i] * (double)buf[i];
  }
  return (float)sqrt(sum / count);
}

// ═══════════════════════════════════════════════════════════════════
// CABEÇALHO WAV
// O Whisper da OpenAI exige formato WAV válido.
// WAV = 44 bytes de header + dados PCM brutos
// ═══════════════════════════════════════════════════════════════════
void buildWavHeader(uint8_t* hdr, uint32_t dataSize) {
  uint32_t chunkSize  = dataSize + 36;
  uint16_t channels   = 1;
  uint32_t sampleRate = SAMPLE_RATE;
  uint16_t bitsPS     = BITS_PER_SAMPLE;
  uint16_t blockAlign = channels * (bitsPS / 8);
  uint32_t byteRate   = sampleRate * blockAlign;
  uint16_t audioFmt   = 1;   // PCM
  uint16_t subchunk1  = 16;

  memcpy(hdr,    "RIFF",      4);
  memcpy(hdr+4,  &chunkSize,  4);
  memcpy(hdr+8,  "WAVEfmt ",  8);
  memcpy(hdr+16, &subchunk1,  4);
  memcpy(hdr+20, &audioFmt,   2);
  memcpy(hdr+22, &channels,   2);
  memcpy(hdr+24, &sampleRate, 4);
  memcpy(hdr+28, &byteRate,   4);
  memcpy(hdr+32, &blockAlign, 2);
  memcpy(hdr+34, &bitsPS,     2);
  memcpy(hdr+36, "data",      4);
  memcpy(hdr+40, &dataSize,   4);
}

// ═══════════════════════════════════════════════════════════════════
// ENVIO PARA PIPEDREAM + RECEBIMENTO E REPRODUÇÃO DO MP3
// ═══════════════════════════════════════════════════════════════════
void sendAndPlay(size_t audioBytes) {
  state = SENDING;
  Serial.println("[HTTP] Montando payload WAV...");

  // ── Monta payload WAV completo (header 44 bytes + PCM) ─────────
  uint32_t wavSize   = 44 + audioBytes;
  uint8_t* wavData   = (uint8_t*) ps_malloc(wavSize);
  if (!wavData) {
    Serial.println("[ERRO] ps_malloc falhou para wavData");
    state = ERR; return;
  }
  buildWavHeader(wavData, (uint32_t)audioBytes);
  memcpy(wavData + 44, audioBuffer, audioBytes);

  // ── Monta body multipart/form-data ─────────────────────────────
  // O Pipedream recebe o arquivo WAV no campo "audio"
  String boundary = "RoboBoundary7MA4YWxk";
  String partHead =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"audio\"; filename=\"rec.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
  String partTail = "\r\n--" + boundary + "--\r\n";

  uint32_t bodySize = partHead.length() + wavSize + partTail.length();
  uint8_t* body     = (uint8_t*) ps_malloc(bodySize);
  if (!body) {
    Serial.println("[ERRO] ps_malloc falhou para body");
    free(wavData); state = ERR; return;
  }

  uint32_t off = 0;
  memcpy(body + off, partHead.c_str(), partHead.length()); off += partHead.length();
  memcpy(body + off, wavData,          wavSize);            off += wavSize;
  memcpy(body + off, partTail.c_str(), partTail.length());
  free(wavData);

  Serial.printf("[HTTP] Payload: %d bytes → enviando para Pipedream...\n", bodySize);

  // ── HTTP POST ───────────────────────────────────────────────────
  // Usando WiFiClientSecure para HTTPS (Pipedream usa TLS)
  WiFiClientSecure client;
  client.setInsecure();  // Não valida certificado — OK para uso pessoal
                          // Para produção: use client.setCACert(rootCA)

  HTTPClient http;
  http.begin(client, PIPEDREAM_URL);
  http.setTimeout(25000);  // 25s — STT + LLM + TTS pode demorar
  http.addHeader("Content-Type",
    "multipart/form-data; boundary=" + boundary);
  http.addHeader("X-Robot-Token", ROBOT_TOKEN);  // Autenticação simples
  http.addHeader("Content-Length", String(bodySize));

  int httpCode = http.POST(body, bodySize);
  free(body);

  Serial.printf("[HTTP] Código de resposta: %d\n", httpCode);

  if (httpCode == 200) {
    // ── Salva MP3 recebido em SPIFFS ───────────────────────────
    Serial.println("[HTTP] Recebendo MP3...");
    WiFiClient* stream   = http.getStreamPtr();
    int         mpSize   = http.getSize();  // -1 se chunked

    File f = SPIFFS.open("/resp.mp3", FILE_WRITE);
    if (!f) {
      Serial.println("[ERRO] Falha ao abrir /resp.mp3 no SPIFFS");
      http.end(); state = ERR; return;
    }

    uint8_t tmpBuf[512];
    int received = 0;
    unsigned long dlStart = millis();

    // Lê o stream em blocos de 512 bytes
    while ((http.connected() || stream->available()) &&
           (millis() - dlStart) < 15000) {
      int avail = stream->available();
      if (avail > 0) {
        int toRead = min(avail, (int)sizeof(tmpBuf));
        int r = stream->read(tmpBuf, toRead);
        if (r > 0) { f.write(tmpBuf, r); received += r; }
      } else {
        delay(5);
      }
      if (mpSize > 0 && received >= mpSize) break;
    }

    f.close();
    http.end();
    Serial.printf("[HTTP] MP3 salvo: %d bytes\n", received);

    if (received > 100) {
      playMP3();
    } else {
      Serial.println("[ERRO] MP3 muito pequeno — possível erro no Pipedream");
      state = ERR;
    }

  } else {
    Serial.printf("[HTTP] Erro: código %d | Body: %s\n",
      httpCode, http.getString().c_str());
    http.end();
    state = ERR;
  }
}

// ═══════════════════════════════════════════════════════════════════
// REPRODUÇÃO MP3 via ESP8266Audio + I2S porta 1 → MAX98357A
// ═══════════════════════════════════════════════════════════════════
void playMP3() {
  Serial.println("[PLAY] Iniciando reprodução...");

  cleanupAudio();  // Garante que objetos anteriores foram destruídos

  source  = new AudioFileSourceSPIFFS("/resp.mp3");
  i2sOut  = new AudioOutputI2S(I2S_NUM_1);  // I2S porta 1 = speaker
  i2sOut->SetPinout(SPK_BCLK, SPK_LRC, SPK_DIN);
  i2sOut->SetOutputModeMono(true);
  i2sOut->SetGain(0.75f);  // Volume: 0.0 (mudo) a 1.0 (máximo)
                             // Ajuste conforme o volume desejado

  mp3 = new AudioGeneratorMP3();
  if (!mp3->begin(source, i2sOut)) {
    Serial.println("[PLAY] Falha ao iniciar MP3");
    cleanupAudio();
    state = ERR;
    return;
  }

  state = PLAYING;
  // A reprodução continua frame a frame no loop() via mp3->loop()
}

// ═══════════════════════════════════════════════════════════════════
// LIMPEZA DOS OBJETOS DE ÁUDIO
// ═══════════════════════════════════════════════════════════════════
void cleanupAudio() {
  if (mp3)    { if (mp3->isRunning()) mp3->stop(); delete mp3; mp3 = nullptr; }
  if (source) { delete source; source = nullptr; }
  if (i2sOut) { delete i2sOut; i2sOut = nullptr; }
}

// ═══════════════════════════════════════════════════════════════════
// LED — indicação visual de estado (não bloqueante)
// ═══════════════════════════════════════════════════════════════════
void updateLED() {
  unsigned long now = millis();
  switch (state) {
    case IDLE:
      digitalWrite(LED_PIN, LOW);   // Apagado
      break;
    case RECORDING:
      digitalWrite(LED_PIN, HIGH);  // Aceso fixo
      break;
    case SENDING:
      // Pisca a 5 Hz (100ms)
      if (now - lastLedToggle >= 100) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
        lastLedToggle = now;
      }
      break;
    case PLAYING:
      // Pisca a 1 Hz (500ms)
      if (now - lastLedToggle >= 500) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
        lastLedToggle = now;
      }
      break;
    case ERR:
      // Pisca a 2,5 Hz (200ms)
      if (now - lastLedToggle >= 200) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
        lastLedToggle = now;
      }
      break;
  }
}

// ═══════════════════════════════════════════════════════════════════
// PISCA PARA SEMPRE — usado em erros fatais no setup()
// ═══════════════════════════════════════════════════════════════════
void blinkForever(int ms) {
  while (true) {
    digitalWrite(LED_PIN, HIGH); delay(ms);
    digitalWrite(LED_PIN, LOW);  delay(ms);
  }
}
