// Mirrors docs/api.md and app/internal/session/session.go's Msg/Start types.
// Keep in sync with the server contract — this file has no runtime logic.

export type CoreStatus = string // "ok" | "not configured" | <error string>

export interface ReadyResponse {
  ready: boolean
  cores: Record<'stt' | 'mt' | 'tts_magpie' | 'tts_vits', CoreStatus>
}

export interface CapabilitiesResponse {
  cores: Record<'stt' | 'mt' | 'tts_magpie' | 'tts_vits', CoreStatus>
  tts: {
    magpie: string[]
    vits: string[]
  }
  stt_langs: string[]
}

export interface Timings {
  stt_ms: number
  mt_ms?: number
  tts_ms?: number
  total_ms: number
}

export type SpeechToSpeechStage = 'ok' | 'empty_transcript' | 'mt_unavailable' | 'tts_unavailable'

export interface SpeechToSpeechResponse {
  request_id: string
  transcript: string
  translation: string
  audio?: string // base64 WAV, omitted if none produced
  audio_format?: string
  sample_rate?: number
  timings: Timings
  stage: SpeechToSpeechStage
}

export interface TranscribeResponse {
  request_id: string
  transcript: string
  timings: Timings
}

export interface TranslateResponse {
  request_id: string
  translation: string
}

export interface SpeakBase64Response {
  request_id: string
  audio: string
  audio_format: string
  sample_rate: number
}

export interface ApiErrorBody {
  error: {
    code: string
    message: string
    stage?: 'stt' | 'mt' | 'tts'
    request_id: string
  }
}

export class ApiError extends Error {
  code: string
  stage?: string
  requestId?: string
  httpStatus: number

  constructor(body: ApiErrorBody['error'], httpStatus: number) {
    super(body.message)
    this.code = body.code
    this.stage = body.stage
    this.requestId = body.request_id
    this.httpStatus = httpStatus
  }
}

// ---- WebSocket /v1/stream ------------------------------------------------

export type StreamFormat = 'f32' | 's16'

export interface StreamStart {
  type: 'start'
  source_lang: string
  target_lang: string
  voice: string
  sample_rate: number
  format: StreamFormat
}

export interface StreamStop {
  type: 'stop'
}

export type StreamServerMsgType = 'partial' | 'transcript' | 'translation' | 'audio' | 'error' | 'done'

export interface StreamServerMsg {
  type: StreamServerMsgType
  text?: string // caption/transcript/translation text, or an error's detail message
  seq: number
  t0_ms?: number
  t1_ms?: number
  n_samples?: number
  sample_rate?: number
  code?: string
  stage?: string
}
