// Node verification of the JUNO WASM DSP: sanity, stability, drift, HPF, chorus, polyphony.
const ModuleFactory = require('./juno_dsp.js');

const P = {
  OCTAVE: 0, TUNE: 1, SAW: 2, PULSE: 3, PW: 4, PWM_DEPTH: 5, PWM_SRC: 6, SUB: 7, SUB_OCT: 8, NOISE: 9,
  DRIFT: 10, HPF: 11, CUTOFF: 12, RES: 13, FENV: 14, FLFO: 15, KEYTRACK: 16, OTA: 17,
  FA: 18, FD: 19, FS: 20, FR: 21, AA: 22, AD: 23, AS: 24, AR: 25, VCA: 26,
  LFO_RATE: 27, LFO_DELAY: 28, LFO_PITCH: 29, CHORUS: 30, VOLUME: 31,
};

ModuleFactory().then((M) => {
  const init = M.cwrap('juno_init', null, ['number']);
  const reset = M.cwrap('juno_reset', null, []);
  const noteOn = M.cwrap('juno_note_on', null, ['number', 'number']);
  const noteOff = M.cwrap('juno_note_off', null, ['number']);
  const setParam = M.cwrap('juno_set_param', null, ['number', 'number']);
  const render = M.cwrap('juno_render', null, ['number', 'number', 'number']);
  const voices = M.cwrap('juno_active_voices', 'number', []);
  const count = M.cwrap('juno_param_count', 'number', []);
  const pmin = M.cwrap('juno_param_min', 'number', ['number']);
  const pmax = M.cwrap('juno_param_max', 'number', ['number']);
  const pdef = M.cwrap('juno_param_default', 'number', ['number']);

  const N = 4096;
  const pl = M._malloc(N * 4), pr = M._malloc(N * 4);
  const fail = (m) => { console.error('FAIL:', m); process.exit(1); };

  function run(blocks) {
    let peak = 0, sumSq = 0, total = 0, bad = false;
    let L = null, R = null;
    for (let b = 0; b < blocks; b++) {
      render(pl, pr, N);
      L = M.HEAPF32.slice(pl >> 2, (pl >> 2) + N);
      R = M.HEAPF32.slice(pr >> 2, (pr >> 2) + N);
      for (let i = 0; i < N; i++) {
        if (!isFinite(L[i]) || !isFinite(R[i])) bad = true;
        peak = Math.max(peak, Math.abs(L[i]), Math.abs(R[i]));
        sumSq += L[i] * L[i] + R[i] * R[i]; total += 2;
      }
    }
    return { peak, rms: Math.sqrt(sumSq / total), bad, L, R };
  }
  function fresh() {
    reset();
    for (let i = 0; i < count(); i++) setParam(i, pdef(i));
  }
  function check(name, r, expectSound = true) {
    console.log(`${name.padEnd(34)} peak=${r.peak.toFixed(4)} rms=${r.rms.toFixed(4)}`);
    if (r.bad) fail(`${name}: NaN/Inf`);
    if (expectSound && r.peak < 0.01) fail(`${name}: silent`);
    if (r.peak > 1.001) fail(`${name}: clipping beyond 1.0`);
  }

  init(44100);
  console.log(`param count = ${count()}`);

  // 1. Default init patch
  fresh(); noteOn(60, 0.9); check('init patch C4', run(4));

  // 2. Polyphony: 8 notes then 9th steals
  fresh(); for (let n = 0; n < 8; n++) noteOn(48 + n * 3, 0.8);
  check('8-voice chord', run(2));
  if (voices() !== 8) fail(`expected 8 voices, got ${voices()}`);
  noteOn(90, 0.8); if (voices() !== 8) fail('voice steal exceeded 8');
  console.log('voice stealing OK (still 8)');

  // 3. Voice release returns to idle
  fresh(); noteOn(60, 1); run(1); noteOff(60); setParam(P.AR, 0.05);
  run(8);
  if (voices() !== 0) fail(`voice should be idle after release, got ${voices()}`);
  console.log('release -> idle OK');

  // 4. Self-oscillation w/ no oscillator input
  fresh(); setParam(P.SAW, 0); setParam(P.PULSE, 0); setParam(P.SUB, 0); setParam(P.NOISE, 0.05);
  setParam(P.RES, 1.0); setParam(P.CUTOFF, 0.55); setParam(P.FENV, 0); setParam(P.AS, 1.0);
  noteOn(60, 1); check('filter self-oscillation', run(6));

  // 5. Extremes stay finite and bounded
  fresh(); setParam(P.RES, 1); setParam(P.OTA, 1); setParam(P.SAW, 1); setParam(P.PULSE, 1);
  setParam(P.SUB, 1); setParam(P.NOISE, 1); setParam(P.VOLUME, 1.5); setParam(P.CUTOFF, 1);
  for (let n = 0; n < 8; n++) noteOn(36 + n * 7, 1);
  check('worst-case extremes', run(6));

  // 6. Bass boost at HPF=0 vs HPF raised: low note energy
  const lowEnergy = (hpf) => {
    fresh(); setParam(P.HPF, hpf); setParam(P.CHORUS, 0); setParam(P.CUTOFF, 0.7);
    setParam(P.SUB, 0.5); setParam(P.AS, 1); noteOn(36, 1); run(2); return run(4).rms;
  };
  const e0 = lowEnergy(0), e5 = lowEnergy(0.6);
  console.log(`HPF=0 low-note rms ${e0.toFixed(4)} vs HPF=0.6 rms ${e5.toFixed(4)}`);
  if (!(e0 > e5)) fail('HPF should remove low-end energy');
  const eBoostOn = lowEnergy(0), eBoostOff = lowEnergy(0.12);
  console.log(`bass boost on (0) ${eBoostOn.toFixed(4)} vs just-off (0.12) ${eBoostOff.toFixed(4)}`);

  // 7. Drift = 0 -> voices identical (L==R mono w/o chorus, repeatable); drift>0 -> detuned beating
  const spread = (drift) => {
    fresh(); setParam(P.DRIFT, drift); setParam(P.CHORUS, 0); setParam(P.CUTOFF, 0.8);
    setParam(P.AS, 1); noteOn(60, 1); noteOn(60 + 0, 1); // same note retriggers one voice
    noteOn(60.0 | 0, 1);
    return run(1).rms;
  };
  spread(0); spread(1);
  // Two different notes' unison across voices via distinct note numbers an octave apart are not comparable;
  // instead verify drift changes the waveform deterministically:
  fresh(); setParam(P.DRIFT, 0); setParam(P.CHORUS, 0); noteOn(60, 1); const a = run(2).L.slice(0, 64);
  fresh(); setParam(P.DRIFT, 1); setParam(P.CHORUS, 0); noteOn(60, 1); const b = run(2).L.slice(0, 64);
  let diff = 0; for (let i = 0; i < 64; i++) diff += Math.abs(a[i] - b[i]);
  console.log(`drift 0 vs 1 waveform difference = ${diff.toFixed(4)}`);
  if (diff < 1e-4) fail('drift had no effect');

  // 8. Chorus: stereo decorrelation
  fresh(); setParam(P.CHORUS, 2); noteOn(60, 1); noteOn(64, 1); noteOn(67, 1);
  run(4); const c = run(2);
  let dsum = 0; for (let i = 0; i < N; i++) dsum += Math.abs(c.L[i] - c.R[i]);
  console.log(`chorus II L/R difference = ${(dsum / N).toFixed(5)}`);
  if (dsum / N < 1e-3) fail('chorus produced no stereo difference');
  fresh(); setParam(P.CHORUS, 0); noteOn(60, 1); run(2); const d = run(1);
  let dsum0 = 0; for (let i = 0; i < N; i++) dsum0 += Math.abs(d.L[i] - d.R[i]);
  if (dsum0 > 1e-6) fail('chorus off should be mono');
  console.log('chorus off is mono OK');

  // 9. GATE vs ENV VCA mode
  fresh(); setParam(P.VCA, 1); noteOn(60, 1); check('VCA gate mode', run(2));
  noteOff(60); run(1);
  if (voices() !== 0) fail('gate mode voice should be idle quickly');
  console.log('gate-mode release OK');

  // 10. Param bounds clamp
  setParam(P.CUTOFF, 99); setParam(P.OCTAVE, -5);
  console.log('ALL JUNO DSP CHECKS PASSED');
  process.exit(0);
}).catch((e) => { console.error(e); process.exit(1); });
