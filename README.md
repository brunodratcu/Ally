# 🤖 Allay — Robô Conversacional

> Robô físico doméstico que ouve, pensa com Claude AI e responde em voz.  
> Construído com ESP32-S3, sem tela, sem partes móveis — estética robô de lixão.

---

## O que o Allay faz

1. Você pressiona o botão PTT e fala
2. O ESP32-S3 grava sua voz em WAV (16kHz, 16-bit)
3. Envia o áudio via HTTPS para um workflow no Pipedream
4. Pipedream transcreve com Whisper → gera resposta com Claude → sintetiza com TTS
5. Retorna o MP3 para o ESP32 → Allay fala a resposta pelo alto-falante

---

## Stack Tecnológica

```
Hardware  →  XIAO ESP32-S3 (MCU) + INMP441 (mic I2S) + MAX98357A (amp I2S)
Firmware  →  Arduino IDE · ESP32 Arduino Core 3.x
Estrutura →  PLA impresso em 3D — cubo 100x100x120mm
Pipeline  →  Pipedream (serverless Node.js)
              ├─ Step 1: Whisper STT   (OpenAI)     voz → texto
              ├─ Step 2: Claude AI     (Anthropic)  texto → resposta
              ├─ Step 3: TTS           (OpenAI)     resposta → áudio MP3
              └─ Step 4: GitHub sync   (opcional)   salva gravações
```

---

## Estrutura do Repositório

```
Ally/
├── firmware/
│   └── robo_voz/
│       └── robo_voz.ino          ← firmware completo (Arduino IDE)
│
├── pipedream/
│   ├── step1_stt_whisper.js      ← transcrição de voz (Whisper)
│   ├── step2_claude_response.js  ← resposta com Claude AI
│   ├── step3_tts_and_respond.js  ← síntese de voz (TTS)
│   └── step4_save_github.js      ← backup de gravações (opcional)
│
├── 3d/
│   ├── torso.stl                 ← cubo principal 100x100x120
│   ├── cabeca.stl                ← cubo superior 65x65x60
│   ├── pescoco.stl               ← conector cabeça-torso
│   ├── ombro.stl                 ← suporte lateral dos braços
│   ├── braco.stl                 ← braço (imprima 2x)
│   ├── garra.stl                 ← mão (imprima 2x)
│   ├── perna.stl                 ← perna (imprima 2x)
│   ├── pe.stl                    ← pé (imprima 2x)
│   ├── tampa_traseira.stl        ← acesso à eletrônica
│   └── grade_speaker.stl         ← painel frontal com furos
│
├── docs/
│   └── Allay_Guia_Completo.docx  ← documentação completa
│
├── recordings/                   ← gravações sincronizadas (Step 4)
└── logs/                         ← histórico de conversas em JSON
```

---

## Sistemas Explicados

### Sistema 1 — ESP32-S3 (Cérebro)

O **XIAO ESP32-S3** é o microcontrolador central. Escolhido por:
- **8MB PSRAM**: buffer de áudio de 256KB não cabe na RAM interna (512KB)
- **I2S duplo**: microfone (I2S_NUM_0) e speaker (I2S_NUM_1) funcionam simultaneamente
- **OTA nativo**: atualização de firmware por Wi-Fi sem USB
- **Processador 240MHz dual-core**: processa I2S + HTTP + OTA sem travamentos

### Sistema 2 — Microfone INMP441

Microfone digital MEMS com protocolo I2S. Diferente de microfones analógicos:
- Entrega amostras digitais 24-bit diretamente, sem ADC externo
- O pino L/R=GND define que opera no canal esquerdo (mono)
- O firmware detecta silêncio por RMS: para de gravar automaticamente após 1,5s quieto

### Sistema 3 — Amplificador MAX98357A

Amplificador de Classe D com decodificador I2S integrado:
- Recebe sinal digital I2S e converte em potência de áudio para o speaker
- 3W / 8Ω — volume claro para ambiente doméstico
- Pino SD=3,3V define ganho padrão +9dB — sem resistores adicionais
- Eficiência 85-90%: não esquenta em uso normal

### Sistema 4 — Pipeline Pipedream

Workflow serverless de 3 steps executado a cada conversa:

| Step | Serviço | Entrada | Saída |
|------|---------|---------|-------|
| 1 | OpenAI Whisper | WAV (16kHz) | Texto transcrito em pt-BR |
| 2 | Anthropic Claude | Texto + histórico | Resposta em texto |
| 3 | OpenAI TTS | Texto da resposta | MP3 (voz nova/onyx) |
| 4 | GitHub (opcional) | WAV + JSON log | Backup permanente |

### Sistema 5 — OTA (Over-The-Air)

Atualização de firmware pelo IP sem cabo USB:
```
Browser → http://192.168.1.100 → Upload .bin → Allay reinicia com firmware novo
```
O ESP32 grava o novo firmware em uma partição espelho e define como ativa.
Se o novo firmware falhar no boot, **reverte automaticamente** para o anterior.

### Sistema 6 — SPIFFS (Armazenamento Local)

Sistema de arquivos na flash interna do ESP32:
- ~1,5MB disponível → ~9 gravações de 5s simultâneas
- Acesso via HTTP: `GET /recordings` lista, download individual por nome
- Índice persistente em `/rec_idx.txt` — sobrevive a reinicializações

---

## Configuração Rápida

### 1. Firmware

Edite as 4 constantes no início do `robo_voz.ino`:
```cpp
const char* WIFI_SSID      = "SEU_WIFI";
const char* WIFI_PASSWORD  = "SUA_SENHA";
const char* PIPEDREAM_URL  = "https://eoXXXXXXXXXX.m.pipedream.net";
const char* ROBOT_TOKEN    = "token_secreto_aqui";
```

Board settings no Arduino IDE:

| Parâmetro | Valor |
|-----------|-------|
| Board | XIAO_ESP32S3 |
| PSRAM | **OPI PSRAM** (obrigatório) |
| Partition Scheme | 8M with spiffs |
| Upload Speed | 921600 |

### 2. Pipedream

1. Criar conta em pipedream.com
2. New Workflow → Trigger: HTTP Webhook (POST)
3. Copiar URL do webhook → colar no firmware como `PIPEDREAM_URL`
4. Adicionar 3 steps (Node.js) com os arquivos da pasta `/pipedream/`
5. Settings → Environment Variables:

| Variável | Valor |
|----------|-------|
| `OPENAI_API_KEY` | Chave da OpenAI |
| `ANTHROPIC_API_KEY` | Chave da Anthropic |
| `ROBOT_SECRET_TOKEN` | Mesmo valor de `ROBOT_TOKEN` no firmware |
| `GITHUB_TOKEN` | Token GitHub (repo write) — para o Step 4 |
| `GITHUB_USER` | Seu username GitHub |
| `GITHUB_REPO` | Nome do repositório (ex: Ally) |

### 3. IP Fixo (Recomendado)

Adicione no `setup()` antes do `WiFi.begin()`:
```cpp
IPAddress local_IP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
WiFi.config(local_IP, gateway, subnet);
```

---

## API REST do Allay

| Método | Endpoint | Descrição |
|--------|----------|-----------|
| GET | `/` | Página web de OTA update |
| GET | `/info` | Status, RAM, RSSI, uptime |
| GET | `/status` | Estado atual do robô |
| POST | `/record/start` | Inicia gravação sem botão físico |
| POST | `/record/stop` | Para gravação e envia ao Pipedream |
| GET | `/recordings` | Lista todos os WAVs salvos |
| DELETE | `/recordings` | Apaga todas as gravações |
| POST | `/config` | Atualiza volume, silêncio etc. em runtime |
| POST | `/update` | Upload de firmware OTA (.bin) |
| POST | `/restart` | Reinicia remotamente |

---

## LED de Status

| Estado | Padrão |
|--------|--------|
| IDLE | Apagado |
| RECORDING | Aceso fixo |
| SENDING | Pisca rápido (5 Hz) |
| PLAYING | Pisca lento (1 Hz) |
| ERRO | Pisca médio (2,5 Hz) |

---

## Atualizar Ferramentas do Allay

Para adicionar novas capacidades (novos comportamentos, ferramentas do Claude):

1. Edite o `SYSTEM_PROMPT` em `step2_claude_response.js` → deploy no Pipedream
2. Ou edite `robo_voz.ino` para novas funcionalidades de hardware
3. Para firmware: compile → Export Compiled Binary → acesse `http://192.168.1.100` → upload
4. Para Pipedream: edite o step diretamente na interface web — sem recompilar nada

---

## Créditos

- Hardware: Seeed Studio XIAO ESP32-S3
- STT: OpenAI Whisper
- AI: Anthropic Claude
- TTS: OpenAI Speech
- Pipeline: Pipedream
- Autor: Bruno Dratcu · 2026
