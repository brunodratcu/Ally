// ═══════════════════════════════════════════════════════════════════
// PIPEDREAM — STEP 1: STT com Whisper (OpenAI)
// Nome do step: stt_whisper
//
// Recebe o arquivo WAV enviado pelo ESP32 via multipart/form-data
// e retorna o texto transcrito.
//
// Como configurar:
//   1. No Pipedream, crie um step "Run Node.js code"
//   2. Cole este código
//   3. Certifique-se que OPENAI_API_KEY está em Settings > Environment Variables
// ═══════════════════════════════════════════════════════════════════

import { OpenAI } from "openai";
import { toFile } from "openai/uploads";
import Busboy from "busboy";

export default defineComponent({
  async run({ steps, $ }) {

    // ── 1. Autenticação do robô ─────────────────────────────────
    // Valida o token enviado pelo ESP32 no header X-Robot-Token
    const receivedToken = steps.trigger.event.headers?.["x-robot-token"];
    if (receivedToken !== process.env.ROBOT_SECRET_TOKEN) {
      await $.respond({ status: 403, body: "Unauthorized" });
      return $.flow.exit("Token inválido — requisição rejeitada");
    }

    // ── 2. Extrai o arquivo WAV do body multipart ───────────────
    // O ESP32 envia: Content-Type: multipart/form-data; boundary=...
    // O campo se chama "audio" e contém o arquivo "rec.wav"
    const rawBody   = steps.trigger.event.body;          // Buffer ou string base64
    const headers   = steps.trigger.event.headers;
    const contentType = headers["content-type"] || headers["Content-Type"] || "";

    let wavBuffer;

    // O Pipedream pode entregar o body como:
    // (a) Buffer direto
    // (b) String base64 (quando o payload é binário)
    // Tentamos extrair o WAV de ambos os casos
    if (rawBody && typeof rawBody === "object" && Buffer.isBuffer(rawBody)) {
      wavBuffer = await extractWavFromMultipart(rawBody, contentType);
    } else if (rawBody && typeof rawBody === "string") {
      // Pode ser base64 ou multipart como string
      const bodyBuf = Buffer.from(rawBody, "base64");
      wavBuffer = await extractWavFromMultipart(bodyBuf, contentType);
    } else {
      // Fallback: tenta usar o body diretamente
      wavBuffer = Buffer.from(rawBody || "", "binary");
    }

    if (!wavBuffer || wavBuffer.length < 100) {
      console.error("Erro: WAV não extraído. Body recebido:", typeof rawBody, rawBody?.length || 0);
      await $.respond({ status: 400, body: "Arquivo WAV não encontrado no body" });
      return $.flow.exit("WAV não encontrado");
    }

    console.log(`[STT] WAV recebido: ${wavBuffer.length} bytes`);

    // ── 3. Chama o Whisper para transcrição ────────────────────
    const openai = new OpenAI({ apiKey: process.env.OPENAI_API_KEY });

    const audioFile = await toFile(wavBuffer, "audio.wav", { type: "audio/wav" });

    const transcript = await openai.audio.transcriptions.create({
      file:     audioFile,
      model:    "whisper-1",
      language: "pt",          // Força português — melhora precisão
      // prompt: "Robô assistente doméstico", // Opcional: dica de contexto
    });

    const userText = transcript.text?.trim() || "";
    console.log(`[STT] Transcrição: "${userText}"`);

    if (!userText) {
      // Sem texto detectado — retorna resposta padrão
      console.warn("[STT] Transcrição vazia — áudio sem fala detectada");
      return { userText: "", empty: true };
    }

    return { userText, empty: false };
  }
});

// ── Função auxiliar: extrai o campo "audio" do multipart ────────
async function extractWavFromMultipart(bodyBuffer, contentType) {
  return new Promise((resolve, reject) => {
    const chunks = [];

    try {
      const bb = Busboy({ headers: { "content-type": contentType } });

      bb.on("file", (fieldname, fileStream, info) => {
        console.log(`[Multipart] Campo: ${fieldname} | Arquivo: ${info.filename}`);
        fileStream.on("data", (chunk) => chunks.push(chunk));
        fileStream.on("end", () => {});
        fileStream.on("error", reject);
      });

      bb.on("close", () => {
        if (chunks.length > 0) {
          resolve(Buffer.concat(chunks));
        } else {
          resolve(null);
        }
      });

      bb.on("error", reject);
      bb.write(bodyBuffer);
      bb.end();

    } catch (err) {
      console.error("Erro ao parsear multipart:", err);
      resolve(null);
    }
  });
}