#!/usr/bin/env node
// bench.mjs — measures the Speech Bridge latency budget against a running
// sb-server and prints a table. Port of the former app/cmd/sb-bench (Go);
// rewritten in Node (a build-time-only dependency already used for the
// frontend) so the project needs no Go toolchain at all. Requires models
// loaded on the server — invoked via scripts/bench.sh.
//
//   node bench.mjs --addr 127.0.0.1:8080 --wav clip.wav --src ru --dst uz

import fs from 'node:fs';

function parseArgs(argv) {
  const out = { addr: '127.0.0.1:8080', wav: '', src: 'ru', dst: 'uz', token: process.env.SB_AUTH_TOKEN || '' };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => argv[++i];
    if (a === '--addr') out.addr = val();
    else if (a === '--wav') out.wav = val();
    else if (a === '--src') out.src = val();
    else if (a === '--dst') out.dst = val();
    else if (a === '--token') out.token = val();
  }
  return out;
}

function decodeWav(buf) {
  const rate = buf.readUInt32LE(24);
  const ch = buf.readUInt16LE(22);
  const bits = buf.readUInt16LE(34);
  let pos = 12;
  let data = null;
  while (pos + 8 <= buf.length) {
    const id = buf.toString('ascii', pos, pos + 4);
    const sz = buf.readUInt32LE(pos + 4);
    pos += 8;
    if (id === 'data') {
      data = buf.subarray(pos, Math.min(pos + sz, buf.length));
      break;
    }
    pos += sz;
  }
  const step = bits / 8;
  const n = Math.floor(data.length / step / ch);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let acc = 0;
    for (let c = 0; c < ch; c++) {
      const off = (i * ch + c) * step;
      acc += bits === 16 ? data.readInt16LE(off) / 32768 : data.readFloatLE(off);
    }
    out[i] = acc / ch;
  }
  return { samples: out, rate };
}

function resampleLinear(input, from, to) {
  if (from === to || input.length === 0) return input;
  const ratio = from / to;
  const out = new Float32Array(Math.floor(input.length / ratio));
  for (let i = 0; i < out.length; i++) {
    const j = Math.min(Math.floor(i * ratio), input.length - 1);
    out[i] = input[j];
  }
  return out;
}

function floatsToLE(f) {
  const buf = new ArrayBuffer(f.length * 4);
  const view = new DataView(buf);
  for (let i = 0; i < f.length; i++) view.setFloat32(i * 4, f[i], true);
  return buf;
}

function fmtMs(ms) {
  return `${Math.round(ms)} ms`;
}

function check(name, ok) {
  console.log(`  [${ok ? 'ok  ' : 'FAIL'}] ${name}`);
}

async function runBatch(base, token, wavBuf, src, dst) {
  const form = new FormData();
  form.set('source_lang', src);
  form.set('target_lang', dst);
  form.set('file', new Blob([wavBuf]), 'clip.wav');
  const headers = token ? { Authorization: `Bearer ${token}` } : {};
  const start = performance.now();
  const res = await fetch(`${base}/v1/speech-to-speech`, { method: 'POST', body: form, headers });
  const wallMs = performance.now() - start;
  const body = await res.json();
  return { timings: body.timings || {}, stage: body.stage || 'unknown', wallMs };
}

function runStream(addr, token, samples, rate, src, dst) {
  return new Promise((resolve) => {
    const url = new URL(`ws://${addr}/v1/stream`);
    if (token) url.searchParams.set('access_token', token);
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';

    const result = { firstPartial: 0, eouToTranslation: 0, eouToAudio: 0 };
    let audioStart = 0;
    let eouAt = 0;
    let finished = false;
    const finish = () => {
      if (finished) return;
      finished = true;
      clearTimeout(timeout);
      ws.close();
      resolve(result);
    };
    const timeout = setTimeout(finish, 30000);

    ws.onopen = () => {
      ws.send(JSON.stringify({ type: 'start', source_lang: src, target_lang: dst, sample_rate: rate, format: 'f32' }));
      audioStart = performance.now();

      const buf16k = resampleLinear(samples, rate, 16000);
      const frame = 4000;
      let off = 0;
      const iv = setInterval(() => {
        if (off >= buf16k.length) {
          clearInterval(iv);
          ws.send(JSON.stringify({ type: 'stop' }));
          return;
        }
        const end = Math.min(off + frame, buf16k.length);
        ws.send(floatsToLE(buf16k.slice(off, end)));
        off = end;
      }, 240);
    };

    ws.onmessage = (ev) => {
      if (typeof ev.data !== 'string') {
        if (eouAt && !result.eouToAudio) result.eouToAudio = performance.now() - eouAt;
        return;
      }
      const m = JSON.parse(ev.data);
      if (m.type === 'partial') {
        if (!result.firstPartial) result.firstPartial = performance.now() - audioStart;
      } else if (m.type === 'transcript') {
        eouAt = performance.now();
      } else if (m.type === 'translation') {
        if (eouAt && !result.eouToTranslation) result.eouToTranslation = performance.now() - eouAt;
      } else if (m.type === 'audio') {
        if (eouAt && !result.eouToAudio) result.eouToAudio = performance.now() - eouAt;
      } else if (m.type === 'done') {
        finish();
      }
    };
    ws.onerror = () => finish();
    ws.onclose = () => finish();
  });
}

async function main() {
  const opt = parseArgs(process.argv.slice(2));
  if (!opt.wav) {
    console.error('bench: --wav is required');
    process.exit(2);
  }
  const wavBuf = fs.readFileSync(opt.wav);
  const { samples, rate } = decodeWav(wavBuf);
  const clipSec = samples.length / rate;

  console.log('== Speech Bridge bench ==');
  console.log(`server   : ${opt.addr}`);
  console.log(`clip     : ${opt.wav}  (${clipSec.toFixed(1)}s, ${rate} Hz)`);
  console.log(`langs    : ${opt.src} -> ${opt.dst}\n`);

  const base = `http://${opt.addr}`;
  const batchStart = performance.now();
  const { timings, stage } = await runBatch(base, opt.token, wavBuf, opt.src, opt.dst);
  const batchWallMs = performance.now() - batchStart;

  console.log(`${'batch end-to-end (wall)'.padEnd(34)} ${fmtMs(batchWallMs).padStart(8)}`);
  console.log(`${'  stt_ms'.padEnd(34)} ${fmtMs(timings.stt_ms || 0).padStart(8)}`);
  console.log(`${'  mt_ms'.padEnd(34)} ${fmtMs(timings.mt_ms || 0).padStart(8)}`);
  console.log(`${'  tts_ms'.padEnd(34)} ${fmtMs(timings.tts_ms || 0).padStart(8)}`);
  console.log(`${'  total_ms (server)'.padEnd(34)} ${fmtMs(timings.total_ms || 0).padStart(8)}   stage=${stage}\n`);

  const st = await runStream(opt.addr, opt.token, samples, rate, opt.src, opt.dst);
  console.log(`${'stream: first partial after audio'.padEnd(34)} ${fmtMs(st.firstPartial).padStart(8)}`);
  console.log(`${'stream: EOU -> translation'.padEnd(34)} ${fmtMs(st.eouToTranslation).padStart(8)}`);
  console.log(`${'stream: EOU -> first audio'.padEnd(34)} ${fmtMs(st.eouToAudio).padStart(8)}`);

  console.log('\n-- budget --');
  check('STT partial lag < 300 ms', st.firstPartial > 0 && st.firstPartial < 300);
  if (!st.eouToTranslation) {
    console.log('  [n/a ] EOU -> translation < 2 s   (no MT engine loaded)');
  } else {
    check('EOU -> translation < 2 s', st.eouToTranslation < 2000);
  }
  check('EOU -> first audio < 4 s', st.eouToAudio > 0 && st.eouToAudio < 4000);
  check(`batch ${clipSec.toFixed(0)}s clip end-to-end < ${(clipSec * 1.5).toFixed(0)}s`, batchWallMs < clipSec * 1.5 * 1000);
  console.log('\nNote: CPU-only figures. On Apple Silicon with --metal (STT+MT), expect notably lower latency.');
}

main().catch((e) => {
  console.error('bench:', e.message || e);
  process.exit(1);
});
