# Ally
### ESP32-S3 + Pipedream + Chat GPT

# Visão Geral
- Robô embedded simples
- Escuta o cliente através de um microfone digital
- Escuta em Pipedream
- Recebe áudio de resposta e reproduz em um alto-falante

# Hardware
- SoC: ESP32-S3
- Flash: 8MB
- RAM: 8MB
- I2S: RX mic; TX speaker
- GPIO: 11 pinos
- Tensão: 3.3V

## Microfone INMP441
- VDD -> 3.3V
- GND -> GND
- SCK -> GPIO 7
- WS -> GPIO 8
- SD -> GPIO 9
- L/R -> GND

## MAX98357A
- VIN -> 5V
- GND -> GND
- BCLK -> GPIO 4
- LRC -> GPIO 5
- DIN -> GPIO 6
- SD -> 3.3V

## Push-to-Talk
- Um único votão NO entre GPIO 0 e GND. 
- O firmware usa INPUT_PULLUP, ao pressionar lê LOW e inicia a gravação.
- Evita acidentes e poupa energia.

# Segurança
- Chaves API expostas no firmware: Chaves para Pipedream, firmware conhece a URL do Webhook
-URL exposta: token secreto para ESP32 envia header, pipedream valida antes de processar
- Relay de áudio por terceiros: timestamp no header e rejeite requests com + 30s


