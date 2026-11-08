#include "TearDSP.h"
#include <cmath>
#include <algorithm>

namespace tear {

TearDSP::TearDSP() {
    // Default parameters
    std::fill(std::begin(params), std::end(params), 0.0f);

    params[PARAM_SYNC_RATIO] = 12.0f;       // 12 semitones (+1 octave sync)
    params[PARAM_SYNC_MODE] = static_cast<float>(SYNC_HARD_TEAR);
    params[PARAM_SLAVE_SHAPE] = 0.0f;       // Saw
    params[PARAM_SUB_LEVEL] = 0.25f;        // 25% sub
    params[PARAM_SUB_OCTAVE] = 0.0f;       // -1 oct
    params[PARAM_SYNC_ENV_DECAY] = 0.35f;   // 350 ms decay
    params[PARAM_SYNC_ENV_DEPTH] = 24.0f;   // +2 octaves sweep
    params[PARAM_SYNC_LFO_RATE] = 1.5f;     // 1.5 Hz
    params[PARAM_SYNC_LFO_DEPTH] = 0.0f;    // 0 by default
    params[PARAM_CUTOFF] = 8000.0f;         // 8 kHz
    params[PARAM_RESONANCE] = 0.2f;         // 20%
    params[PARAM_FILTER_TYPE] = static_cast<float>(FILTER_LADDER_24);
    params[PARAM_FILTER_ENV_AMOUNT] = 0.3f;
    params[PARAM_AMP_ATTACK] = 0.005f;      // 5ms
    params[PARAM_AMP_DECAY] = 0.4f;         // 400ms
    params[PARAM_AMP_SUSTAIN] = 0.7f;       // 70%
    params[PARAM_AMP_RELEASE] = 0.25f;      // 250ms
    params[PARAM_FILTER_ATTACK] = 0.01f;
    params[PARAM_FILTER_DECAY] = 0.5f;
    params[PARAM_FILTER_SUSTAIN] = 0.2f;
    params[PARAM_FILTER_RELEASE] = 0.3f;
    params[PARAM_DRIVE] = 0.2f;             // 20% drive
    params[PARAM_SATURATION_TYPE] = static_cast<float>(SAT_TUBE_VALVE);

    init(44100.0f);
}

void TearDSP::init(float fs) {
    sampleRate = fs > 8000.0f ? fs : 44100.0f;
    invSampleRate = 1.0f / sampleRate;
    reset();
}

void TearDSP::reset() {
    masterPhase = 0.0f;
    slavePhase1 = 0.0f;
    slavePhase2 = 0.0f;
    slaveDirection = 1.0f;
    subPhase = 0.0f;
    syncLfoPhase = 0.0f;
    syncEnvVal = 0.0f;
    tapeHist = 0.0f;
    transMag = 0.0f;

    for (int i = 0; i < 4; ++i) ladderS[i] = 0.0f;
    svfIc1eq = 0.0f;
    svfIc2eq = 0.0f;

    ampEnv.state = ADSR::IDLE;
    ampEnv.value = 0.0f;
    filterEnv.state = ADSR::IDLE;
    filterEnv.value = 0.0f;
}

void TearDSP::noteOn(uint8_t note, float velocity) {
    currentNote = note;
    currentVelocity = std::clamp(velocity, 0.0f, 1.0f);

    // MIDI note to frequency: 440 * 2^((note - 69) / 12)
    baseFrequency = 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f);

    // Setup envelopes
    ampEnv.setAttack(params[PARAM_AMP_ATTACK], sampleRate);
    ampEnv.setDecay(params[PARAM_AMP_DECAY], sampleRate);
    ampEnv.setSustain(params[PARAM_AMP_SUSTAIN]);
    ampEnv.setRelease(params[PARAM_AMP_RELEASE], sampleRate);
    ampEnv.gate(true);

    filterEnv.setAttack(params[PARAM_FILTER_ATTACK], sampleRate);
    filterEnv.setDecay(params[PARAM_FILTER_DECAY], sampleRate);
    filterEnv.setSustain(params[PARAM_FILTER_SUSTAIN]);
    filterEnv.setRelease(params[PARAM_FILTER_RELEASE], sampleRate);
    filterEnv.gate(true);

    // Sync Decay envelope: starts at 1.0 and decays to 0.0
    syncEnvVal = 1.0f;
    float syncDecaySec = std::max(0.005f, params[PARAM_SYNC_ENV_DECAY]);
    syncEnvRate = 1.0f / (syncDecaySec * sampleRate);

    // Reset phases for clean attack
    masterPhase = 0.0f;
    slavePhase1 = 0.0f;
    slavePhase2 = 0.0f;
    slaveDirection = 1.0f;
    subPhase = 0.0f;
}

void TearDSP::noteOff() {
    ampEnv.gate(false);
    filterEnv.gate(false);
}

void TearDSP::setParam(uint8_t paramId, float value) {
    if (paramId >= PARAM_COUNT) return;
    params[paramId] = value;

    // Update dynamic envelope rates
    if (paramId == PARAM_AMP_ATTACK) ampEnv.setAttack(value, sampleRate);
    else if (paramId == PARAM_AMP_DECAY) ampEnv.setDecay(value, sampleRate);
    else if (paramId == PARAM_AMP_SUSTAIN) ampEnv.setSustain(value);
    else if (paramId == PARAM_AMP_RELEASE) ampEnv.setRelease(value, sampleRate);
    else if (paramId == PARAM_FILTER_ATTACK) filterEnv.setAttack(value, sampleRate);
    else if (paramId == PARAM_FILTER_DECAY) filterEnv.setDecay(value, sampleRate);
    else if (paramId == PARAM_FILTER_SUSTAIN) filterEnv.setSustain(value);
    else if (paramId == PARAM_FILTER_RELEASE) filterEnv.setRelease(value, sampleRate);
    else if (paramId == PARAM_SYNC_ENV_DECAY) {
        float syncDecaySec = std::max(0.005f, value);
        syncEnvRate = 1.0f / (syncDecaySec * sampleRate);
    }
}

float TearDSP::getParam(uint8_t paramId) const {
    if (paramId >= PARAM_COUNT) return 0.0f;
    return params[paramId];
}

// Helper to evaluate raw waveform from phase
static inline float evalShape(float phase, float shape, float dt) {
    // shape 0.0 = Sawtooth, 0.5 = Square, 1.0 = DualSaw
    // Sawtooth in [-1, +1]
    float saw = 2.0f * phase - 1.0f;
    saw -= polyBLEP(phase, dt);

    if (shape <= 0.5f) {
        // Blend between Saw and Square
        float morph = shape * 2.0f; // 0..1
        float sq = (phase < 0.5f) ? 1.0f : -1.0f;
        sq += polyBLEP(phase, dt);
        float pShift = phase + 0.5f;
        if (pShift >= 1.0f) pShift -= 1.0f;
        sq -= polyBLEP(pShift, dt);
        return saw * (1.0f - morph) + sq * morph;
    } else {
        // Blend between Square and DualSaw (octave/detuned wave)
        float morph = (shape - 0.5f) * 2.0f; // 0..1
        float sq = (phase < 0.5f) ? 1.0f : -1.0f;
        sq += polyBLEP(phase, dt);
        float pShift = phase + 0.5f;
        if (pShift >= 1.0f) pShift -= 1.0f;
        sq -= polyBLEP(pShift, dt);

        // DualSaw: sum of saw and 180-deg phase saw
        float saw2Phase = phase + 0.5f;
        if (saw2Phase >= 1.0f) saw2Phase -= 1.0f;
        float saw2 = 2.0f * saw2Phase - 1.0f;
        saw2 -= polyBLEP(saw2Phase, dt);
        float dualSaw = 0.5f * (saw + saw2);

        return sq * (1.0f - morph) + dualSaw * morph;
    }
}

float TearDSP::processSample() {
    if (!ampEnv.isActive()) return 0.0f;

    // 1. Process Envelopes & Modulations
    float amp = ampEnv.process() * currentVelocity;
    float fEnv = filterEnv.process();

    if (syncEnvVal > 0.0f) {
        syncEnvVal -= syncEnvRate;
        if (syncEnvVal < 0.0f) syncEnvVal = 0.0f;
    }

    // Sync LFO (Sine)
    float lfoFreq = std::clamp(params[PARAM_SYNC_LFO_RATE], 0.05f, 30.0f);
    syncLfoPhase += lfoFreq * invSampleRate;
    if (syncLfoPhase >= 1.0f) syncLfoPhase -= 1.0f;
    float lfoVal = std::sin(syncLfoPhase * TWO_PI);

    // Calculate effective Sync pitch offset in semitones
    float syncSemitones = params[PARAM_SYNC_RATIO]
                        + (syncEnvVal * params[PARAM_SYNC_ENV_DEPTH])
                        + (lfoVal * params[PARAM_SYNC_LFO_DEPTH]);
    syncSemitones = std::clamp(syncSemitones, -24.0f, 60.0f);

    float syncFreqMult = std::pow(2.0f, syncSemitones / 12.0f);

    // Master & Slave Frequencies
    float fMaster = std::clamp(baseFrequency, 10.0f, sampleRate * 0.45f);
    float dtMaster = fMaster * invSampleRate;

    float fSlave1 = std::clamp(fMaster * syncFreqMult, 10.0f, sampleRate * 0.48f);
    float dtSlave1 = fSlave1 * invSampleRate;

    // Slave 2 (for Dual SuperTear, slightly detuned by +14 cents)
    float fSlave2 = std::clamp(fSlave1 * 1.0081f, 10.0f, sampleRate * 0.48f);
    float dtSlave2 = fSlave2 * invSampleRate;

    // 2. Advance Master Phase and detect sync wrap
    float oldMasterPhase = masterPhase;
    masterPhase += dtMaster;
    bool syncTriggered = false;
    float tFrac = 0.0f; // Fractional sample offset where master wrapped

    if (masterPhase >= 1.0f) {
        masterPhase -= 1.0f;
        syncTriggered = true;
        // Subsample offset fraction: tFrac in [0, 1)
        tFrac = masterPhase / dtMaster;
        if (tFrac < 0.0f) tFrac = 0.0f;
        if (tFrac >= 1.0f) tFrac = 0.999f;
    }

    // 3. Process Slave Phase & Sync
    SyncMode mode = static_cast<SyncMode>(static_cast<int>(params[PARAM_SYNC_MODE]) % 5);
    float slaveShape = std::clamp(params[PARAM_SLAVE_SHAPE], 0.0f, 1.0f);

    float slaveOut = 0.0f;

    if (mode == SYNC_SOFT_REVERSE) {
        // Soft reverse: on sync trigger, slave phase flips direction
        if (syncTriggered) {
            slaveDirection = -slaveDirection;
        }
        slavePhase1 += dtSlave1 * slaveDirection;
        if (slavePhase1 >= 1.0f) {
            slavePhase1 -= 1.0f;
        } else if (slavePhase1 < 0.0f) {
            slavePhase1 += 1.0f;
        }
        slaveOut = evalShape(slavePhase1, slaveShape, dtSlave1);
    }
    else if (mode == SYNC_WINDOWED_FORMANT) {
        // Windowed Formant: Reset slave at master wrap, but window slave amplitude smoothly
        if (syncTriggered) {
            slavePhase1 = tFrac * dtSlave1;
        } else {
            slavePhase1 += dtSlave1;
            if (slavePhase1 >= 1.0f) slavePhase1 -= 1.0f;
        }
        float rawSlave = evalShape(slavePhase1, slaveShape, dtSlave1);
        // Formant window: raised cosine over master phase
        float win = 0.5f * (1.0f - std::cos(masterPhase * TWO_PI));
        slaveOut = rawSlave * win;
    }
    else if (mode == SYNC_DUAL_SUPERTEAR) {
        // Dual SuperTear: Two detuned slaves synced to master
        if (syncTriggered) {
            slavePhase1 = tFrac * dtSlave1;
            slavePhase2 = tFrac * dtSlave2;
        } else {
            slavePhase1 += dtSlave1;
            if (slavePhase1 >= 1.0f) slavePhase1 -= 1.0f;
            slavePhase2 += dtSlave2;
            if (slavePhase2 >= 1.0f) slavePhase2 -= 1.0f;
        }
        float s1 = evalShape(slavePhase1, slaveShape, dtSlave1);
        float s2 = evalShape(slavePhase2, slaveShape, dtSlave2);
        slaveOut = 0.5f * (s1 + s2);
    }
    else if (mode == SYNC_RING_SHRED) {
        // Ring Shred: Master waveform multiplied by Hard-Synced Slave
        if (syncTriggered) {
            slavePhase1 = tFrac * dtSlave1;
        } else {
            slavePhase1 += dtSlave1;
            if (slavePhase1 >= 1.0f) slavePhase1 -= 1.0f;
        }
        float s = evalShape(slavePhase1, slaveShape, dtSlave1);
        // Master saw
        float m = 2.0f * masterPhase - 1.0f - polyBLEP(masterPhase, dtMaster);
        slaveOut = s * m * 1.5f;
    }
    else {
        // Mode 0: Hard Tear (Subsample PolyBLEP Hard Sync)
        if (syncTriggered) {
            // Value just before reset
            float valBefore = evalShape(slavePhase1 + dtSlave1 * (1.0f - tFrac), slaveShape, dtSlave1);
            // Reset phase to subsample advance
            slavePhase1 = tFrac * dtSlave1;
            float valAfter = evalShape(slavePhase1, slaveShape, dtSlave1);
            
            // Step discontinuity:
            float delta = valAfter - valBefore;
            slaveOut = valAfter - delta * (1.0f - tFrac);
        } else {
            slavePhase1 += dtSlave1;
            if (slavePhase1 >= 1.0f) slavePhase1 -= 1.0f;
            slaveOut = evalShape(slavePhase1, slaveShape, dtSlave1);
        }
    }

    // 4. Sub-Oscillator (-1 or -2 octaves)
    float subDiv = (params[PARAM_SUB_OCTAVE] >= 0.5f) ? 0.25f : 0.5f;
    float dtSub = dtMaster * subDiv;
    subPhase += dtSub;
    if (subPhase >= 1.0f) subPhase -= 1.0f;
    // Sub is a warm triangle wave: 4 * |phase - 0.5| - 1
    float subTri = 4.0f * std::abs(subPhase - 0.5f) - 1.0f;
    float subLevel = std::clamp(params[PARAM_SUB_LEVEL], 0.0f, 1.0f);

    float oscMix = (slaveOut * (1.0f - 0.5f * subLevel)) + (subTri * subLevel);

    // 5. Zero-Delay Feedback Filter
    float baseCutoff = params[PARAM_CUTOFF];
    // Filter Envelope modulation (-4 to +4 octaves)
    float fEnvMod = (fEnv * 2.0f - 1.0f) * params[PARAM_FILTER_ENV_AMOUNT] * 4.0f;
    float targetCutoff = baseCutoff * std::pow(2.0f, fEnvMod);
    targetCutoff = std::clamp(targetCutoff, 20.0f, sampleRate * 0.45f);

    float res = std::clamp(params[PARAM_RESONANCE], 0.0f, 0.98f);
    FilterType fType = static_cast<FilterType>(static_cast<int>(params[PARAM_FILTER_TYPE]) % 3);

    float filtered = oscMix;
    if (fType == FILTER_LADDER_24) {
        filtered = renderLadder(oscMix, targetCutoff, res);
    } else if (fType == FILTER_WARM_SVF_12) {
        filtered = renderWarmSVF(oscMix, targetCutoff, res);
    }

    // 6. Saturation Stage & Drive
    float drive = std::clamp(params[PARAM_DRIVE], 0.0f, 1.0f);
    SaturationType satType = static_cast<SaturationType>(static_cast<int>(params[PARAM_SATURATION_TYPE]) % 5);
    float saturated = applySaturation(filtered, drive, satType);

    // 7. Output with Amp Envelope
    return saturated * amp;
}

// 4-Pole ZDF Ladder Filter implementation
float TearDSP::renderLadder(float in, float cutoffHz, float res) {
    float omega = PI * cutoffHz * invSampleRate;
    float g = std::tan(omega);
    if (g > 2.0f) g = 2.0f;

    float G = g / (1.0f + g);
    float k = 3.82f * (0.35f * res + 0.65f * res * res);

    float G2 = G * G;
    float G3 = G2 * G;
    float G4 = G3 * G;

    float S_equiv = G3 * ladderS[0] + G2 * ladderS[1] + G * ladderS[2] + ladderS[3];
    float u = (in - k * S_equiv) / (1.0f + k * G4);

    // Clamping to prevent runaway
    u = std::clamp(u, -3.0f, 3.0f);

    float v0 = (u - ladderS[0]) * G;
    float y0 = v0 + ladderS[0];
    ladderS[0] = y0 + v0;

    float v1 = (y0 - ladderS[1]) * G;
    float y1 = v1 + ladderS[1];
    ladderS[1] = y1 + v1;

    float v2 = (y1 - ladderS[2]) * G;
    float y2 = v2 + ladderS[2];
    ladderS[2] = y2 + v2;

    float v3 = (y2 - ladderS[3]) * G;
    float y3 = v3 + ladderS[3];
    ladderS[3] = y3 + v3;

    // Stage clamping
    for (int i = 0; i < 4; ++i) {
        ladderS[i] = std::clamp(ladderS[i], -4.0f, 4.0f);
    }

    return y3;
}

// 2-Pole ZDF SVF Filter implementation
float TearDSP::renderWarmSVF(float in, float cutoffHz, float res) {
    float omega = PI * cutoffHz * invSampleRate;
    float g = std::tan(omega);
    if (g > 2.0f) g = 2.0f;

    // 2 * damping factor. res in [0, 1) -> 2*R in [2.0, 0.05]
    float damping = 2.0f * (1.0f - res * 0.97f);
    float h = 1.0f / (1.0f + g * (g + damping));

    float hp = (in - svfIc1eq * (g + damping) - svfIc2eq) * h;
    float v1 = g * hp;
    float bp = v1 + svfIc1eq;
    svfIc1eq = bp + v1;

    float v2 = g * bp;
    float lp = v2 + svfIc2eq;
    svfIc2eq = lp + v2;

    // Gentle warm SVF saturation
    svfIc1eq = std::clamp(svfIc1eq, -3.0f, 3.0f);
    svfIc2eq = std::clamp(svfIc2eq, -3.0f, 3.0f);

    return lp;
}

// 5 Saturation Flavors (from Tracker 1.0.4 NL firmware)
float TearDSP::applySaturation(float in, float drive, SaturationType type) {
    if (drive <= 0.001f) return in;

    // Gain scalar up to ~4x drive
    float gain = 1.0f + drive * 3.0f;
    float x = in * gain;

    switch (type) {
        case SAT_MODERN_SOFT:
            return fastTanh(x);

        case SAT_CLASSIC_HARD:
            return std::clamp(x * 1.2f, -1.0f, 1.0f);

        case SAT_TUBE_VALVE: {
            // Asymmetric triode emulation: 2nd harmonic richness + gentle compression
            float asym = x + 0.22f * drive * x * x;
            return fastTanh(asym);
        }

        case SAT_TAPE: {
            // Symmetrical odd-harmonic saturation with dynamic high-frequency absorption
            float sat = x - (x * x * x) * 0.16f;
            sat = fastTanh(sat);
            // 1-pole lowpass filter for tape head flux saturation
            tapeHist += (sat - tapeHist) * (0.45f + 0.35f * (1.0f - drive));
            return tapeHist;
        }

        case SAT_TRANSFORMER: {
            // Iron core magnetic saturation + hysteresis
            float diff = x - transMag;
            transMag += diff * (0.28f + 0.15f * drive);
            float hyst = 0.82f * x + 0.18f * transMag;
            return fastTanh(hyst);
        }

        default:
            return fastTanh(x);
    }
}

void TearDSP::processBlock(float* output, int numSamples) {
    for (int i = 0; i < numSamples; ++i) {
        output[i] = processSample();
    }
}

} // namespace tear
