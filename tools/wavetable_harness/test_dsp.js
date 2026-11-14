// Node verification of the Wavetable WASM DSP: warp modes, WT envelopes, .wav load, .pti round-trip.
const ModuleFactory = require('./wt_dsp.js');

const P = {
  TABLE: 0, POSITION: 1, WINDOW_SIZE: 2, WARP_MODE: 3, WARP_AMOUNT: 4,
  SUB_WAVE: 5, SUB_LEVEL: 6,
  WT_ENV_MODE: 7, WT_ENV_SHAPE: 8, WT_ENV_WARP_X: 9, WT_ENV_WARP_Y: 10,
  WT_ENV_SPEED: 11, WT_ENV_SUST: 12, WT_ENV_TO_POS: 13, WT_ENV_TO_WARP: 14,
  FILTER_ENABLE: 15, FILTER_TYPE: 16, CUTOFF: 17, RESONANCE: 18,
  AENV_ATTACK: 19, AENV_DECAY: 20, AENV_SUSTAIN: 21, AENV_RELEASE: 22,
  TUNE: 23, FINETUNE: 24, VOLUME: 25,
};

ModuleFactory().then((M) => {
  const init = M.cwrap('wt_init', null, ['number']);
  const reset = M.cwrap('wt_reset', null, []);
  const noteOn = M.cwrap('wt_note_on', null, ['number', 'number']);
  const noteOff = M.cwrap('wt_note_off', null, ['number']);
  const setParam = M.cwrap('wt_set_param', null, ['number', 'number']);
  const getParam = M.cwrap('wt_get_param', 'number', ['number']);
  const render = M.cwrap('wt_render', null, ['number', 'number', 'number']);
  const voices = M.cwrap('wt_active_voices', 'number', []);
  const count = M.cwrap('wt_param_count', 'number', []);
  const pdef = M.cwrap('wt_param_default', 'number', ['number']);
  const loadPcm16 = M.cwrap('wt_load_pcm16', 'number', ['number', 'number', 'number']);
  const getWinSize = M.cwrap('wt_get_window_size', 'number', []);
  const getWinCount = M.cwrap('wt_get_window_count', 'number', []);
  const getDisplayWave = M.cwrap('wt_get_display_wave', null, ['number', 'number']);
  const getEnvPreview = M.cwrap('wt_get_env_preview', null, ['number', 'number']);
  const exportPtiSize = M.cwrap('wt_export_pti_size', 'number', []);
  const exportPti = M.cwrap('wt_export_pti', 'number', ['number', 'number', 'string']);
  const importPti = M.cwrap('wt_import_pti', 'number', ['number', 'number']);

  const N = 1024;
  const pl = M._malloc(N * 4);
  const pr = M._malloc(N * 4);
  const fail = (m) => { console.error('FAIL:', m); process.exit(1); };

  function fresh() {
    reset();
    for (let i = 0; i < count(); i++) setParam(i, pdef(i));
  }

  function run(blocks) {
    let peak = 0, sumSq = 0, total = 0, bad = false;
    for (let b = 0; b < blocks; b++) {
      render(pl, pr, N);
      const L = M.HEAPF32.slice(pl >> 2, (pl >> 2) + N);
      const R = M.HEAPF32.slice(pr >> 2, (pr >> 2) + N);
      for (let i = 0; i < N; i++) {
        if (!isFinite(L[i]) || !isFinite(R[i])) bad = true;
        peak = Math.max(peak, Math.abs(L[i]), Math.abs(R[i]));
        sumSq += L[i] * L[i] + R[i] * R[i];
        total += 2;
      }
    }
    return { peak, rms: Math.sqrt(sumSq / total), bad };
  }

  function check(name, r, expectSound = true) {
    console.log(`${name.padEnd(36)} peak=${r.peak.toFixed(4)} rms=${r.rms.toFixed(4)}`);
    if (r.bad) fail(`${name}: NaN/Inf`);
    if (expectSound && r.peak < 0.01) fail(`${name}: silent`);
    if (r.peak > 1.01) fail(`${name}: clipping beyond 1.0`);
  }

  init(44100);
  console.log(`param count = ${count()}, winSize = ${getWinSize()}, winCount = ${getWinCount()}`);

  // 1. Default patch
  fresh(); noteOn(60, 0.9); check('Default patch C4', run(4));

  // 2. All Warp Modes (Off, Asym, Sync, Fold) across full range + Sub
  for (let m = 0; m <= 3; m++) {
    for (const amt of [0, 35, 75, 100]) {
      fresh();
      setParam(P.WARP_MODE, m);
      setParam(P.WARP_AMOUNT, amt);
      setParam(P.SUB_WAVE, 1);
      setParam(P.SUB_LEVEL, 30);
      noteOn(48, 0.9);
      check(`Warp mode=${m} amt=${amt}% + Sub`, run(4));
    }
  }

  // 3. Polyphony & Voice release
  fresh();
  for (let i = 0; i < 8; i++) noteOn(48 + i * 2, 0.8);
  check('8-voice chord', run(2));
  if (voices() !== 8) fail(`Expected 8 voices, got ${voices()}`);
  noteOn(79, 0.8);
  if (voices() !== 8) fail('Voice stealing exceeded 8');
  for (let i = 0; i < 8; i++) noteOff(48 + i * 2);
  noteOff(79);
  setParam(P.AENV_RELEASE, 0.05);
  run(16);
  if (voices() !== 0) fail(`Voices should return to idle, got ${voices()}`);

  // 4. Display waveform & Envelope preview buffers
  const wavePtr = M._malloc(256 * 4);
  const envPtr = M._malloc(128 * 4);
  fresh(); setParam(P.WARP_MODE, 3); setParam(P.WARP_AMOUNT, 80);
  getDisplayWave(wavePtr, 256);
  getEnvPreview(envPtr, 128);
  const wArr = M.HEAPF32.slice(wavePtr >> 2, (wavePtr >> 2) + 256);
  const eArr = M.HEAPF32.slice(envPtr >> 2, (envPtr >> 2) + 128);
  if (wArr.some(v => !isFinite(v)) || eArr.some(v => !isFinite(v))) fail('Preview returned NaN/Inf');
  M._free(wavePtr);
  M._free(envPtr);

  // 5. Custom PCM16 wavetable load + .pti Export & Round-trip Import
  const customWin = 1024;
  const customFrames = 4;
  const customLen = customWin * customFrames;
  const pcmPtr = M._malloc(customLen * 2);
  for (let f = 0; f < customFrames; f++) {
    for (let i = 0; i < customWin; i++) {
      const s = Math.round(28000 * Math.sin(2 * Math.PI * (f + 1) * i / customWin));
      M.HEAP16[(pcmPtr >> 1) + f * customWin + i] = s;
    }
  }
  if (loadPcm16(pcmPtr, customLen, customWin) !== 1) fail('loadPcm16 failed');
  M._free(pcmPtr);
  if (getWinSize() !== 1024 || getWinCount() !== 4) fail('Custom table dims mismatch');

  setParam(P.POSITION, 0.666);
  setParam(P.WARP_MODE, 3); // Fold
  setParam(P.WARP_AMOUNT, 62);
  setParam(P.SUB_WAVE, 2); // Tri
  setParam(P.SUB_LEVEL, 45);
  setParam(P.WT_ENV_MODE, 2); // WT LFO
  setParam(P.WT_ENV_SHAPE, 42);
  setParam(P.WT_ENV_WARP_X, 3);
  setParam(P.WT_ENV_WARP_Y, -2);
  setParam(P.WT_ENV_TO_POS, 0.55);
  setParam(P.WT_ENV_TO_WARP, -0.35);
  setParam(P.CUTOFF, 0.64);
  setParam(P.RESONANCE, 0.40);

  const ptiBytes = exportPtiSize();
  const expectedBytes = 392 + customLen * 2;
  if (ptiBytes !== expectedBytes) fail(`Expected pti size ${expectedBytes}, got ${ptiBytes}`);
  const ptiPtr = M._malloc(ptiBytes);
  const written = exportPti(ptiPtr, ptiBytes, 'TestFoldWT');
  if (written !== ptiBytes) fail(`exportPti wrote ${written}, expected ${ptiBytes}`);

  // Verify TI header & 372-byte size
  const ptiCopy = M.HEAPU8.slice(ptiPtr, ptiPtr + ptiBytes);
  if (ptiCopy[0] !== 0x54 || ptiCopy[1] !== 0x49) fail('Missing TI magic bytes');
  const instSize = ptiCopy[12] | (ptiCopy[13] << 8);
  if (instSize !== 372) fail(`Expected instrumentSize 372, got ${instSize}`);

  // Reset synth & re-import .pti
  fresh();
  const rePtr = M._malloc(ptiBytes);
  M.HEAPU8.set(ptiCopy, rePtr);
  if (importPti(rePtr, ptiBytes) !== 1) fail('importPti failed');
  M._free(ptiPtr);
  M._free(rePtr);

  if (getWinSize() !== 1024 || getWinCount() !== 4) fail('Imported table dims mismatch');
  if (Math.round(getParam(P.WARP_MODE)) !== 3) fail('Imported warp mode mismatch');
  if (Math.round(getParam(P.WARP_AMOUNT)) !== 62) fail('Imported warp amount mismatch');
  if (Math.round(getParam(P.SUB_WAVE)) !== 2) fail('Imported sub wave mismatch');
  if (Math.round(getParam(P.SUB_LEVEL)) !== 45) fail('Imported sub level mismatch');
  if (Math.round(getParam(P.WT_ENV_MODE)) !== 2) fail('Imported WT env mode mismatch');
  if (Math.round(getParam(P.WT_ENV_SHAPE)) !== 42) fail('Imported WT env shape mismatch');
  if (Math.round(getParam(P.WT_ENV_WARP_X)) !== 3) fail('Imported WT env warp X mismatch');
  if (Math.round(getParam(P.WT_ENV_WARP_Y)) !== -2) fail('Imported WT env warp Y mismatch');
  if (Math.abs(getParam(P.CUTOFF) - 0.64) > 0.01) fail('Imported cutoff mismatch');
  if (Math.abs(getParam(P.RESONANCE) - 0.40) > 0.01) fail('Imported resonance mismatch');

  noteOn(55, 0.9);
  check('Round-tripped .pti playback', run(4));

  M._free(pl);
  M._free(pr);
  console.log('\nALL WAVETABLE DSP & .PTI TESTS PASSED!');
});
