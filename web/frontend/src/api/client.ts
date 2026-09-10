import {
  ApiError,
  type ApiErrorBody,
  type CapabilitiesResponse,
  type ReadyResponse,
  type SpeakBase64Response,
  type SpeechToSpeechResponse,
  type TranscribeResponse,
  type TranslateResponse,
} from './types'

function authHeaders(token: string): HeadersInit {
  return token ? { Authorization: `Bearer ${token}` } : {}
}

async function asJson<T>(res: Response): Promise<T> {
  if (!res.ok) {
    let body: ApiErrorBody | null = null
    try {
      body = await res.json()
    } catch {
      // non-JSON error body (e.g. proxy failure) — fall through to a synthetic error
    }
    if (body?.error) throw new ApiError(body.error, res.status)
    throw new ApiError({ code: 'unknown', message: res.statusText || 'request failed', request_id: '' }, res.status)
  }
  return res.json() as Promise<T>
}

export async function getHealth(): Promise<{ status: string }> {
  const res = await fetch('/health')
  return asJson(res)
}

export async function getReady(): Promise<ReadyResponse> {
  const res = await fetch('/ready')
  // /ready returns 503 with the same body shape when not ready — read the body regardless.
  return res.json() as Promise<ReadyResponse>
}

export async function getCapabilities(token: string): Promise<CapabilitiesResponse> {
  const res = await fetch('/v1/capabilities', { headers: authHeaders(token) })
  return asJson(res)
}

export interface SpeechToSpeechParams {
  file: File
  sourceLang: string
  targetLang: string
  voice?: string
  token: string
}

export async function speechToSpeech(p: SpeechToSpeechParams): Promise<SpeechToSpeechResponse> {
  const form = new FormData()
  form.set('file', p.file)
  form.set('source_lang', p.sourceLang || 'auto')
  form.set('target_lang', p.targetLang)
  if (p.voice) form.set('voice', p.voice)
  const res = await fetch('/v1/speech-to-speech', { method: 'POST', body: form, headers: authHeaders(p.token) })
  return asJson(res)
}

export async function transcribe(file: File, sourceLang: string, token: string): Promise<TranscribeResponse> {
  const form = new FormData()
  form.set('file', file)
  form.set('source_lang', sourceLang || 'auto')
  const res = await fetch('/v1/transcribe', { method: 'POST', body: form, headers: authHeaders(token) })
  return asJson(res)
}

export async function translate(
  text: string,
  sourceLang: string,
  targetLang: string,
  token: string,
): Promise<TranslateResponse> {
  const res = await fetch('/v1/translate', {
    method: 'POST',
    headers: { 'content-type': 'application/json', ...authHeaders(token) },
    body: JSON.stringify({ text, source_lang: sourceLang || 'auto', target_lang: targetLang }),
  })
  return asJson(res)
}

export async function speak(
  text: string,
  lang: string,
  voice: string,
  token: string,
): Promise<SpeakBase64Response> {
  const res = await fetch('/v1/speak', {
    method: 'POST',
    headers: { 'content-type': 'application/json', ...authHeaders(token) },
    body: JSON.stringify({ text, lang, voice, encoding: 'base64' }),
  })
  return asJson(res)
}
