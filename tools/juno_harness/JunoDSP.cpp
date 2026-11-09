#include "JunoDSP.h"

namespace juno {

const ParamInfo kParamInfo[P_COUNT] = {
    /* P_OCTAVE          */ {0.0f, 2.0f, 1.0f},
    /* P_TUNE            */ {-12.0f, 12.0f, 0.0f},
    /* P_SAW_LEVEL       */ {0.0f, 1.0f, 0.8f},
    /* P_PULSE_LEVEL     */ {0.0f, 1.0f, 0.0f},
    /* P_PULSE_WIDTH     */ {0.05f, 0.95f, 0.5f},
    /* P_PWM_DEPTH       */ {0.0f, 1.0f, 0.3f},
    /* P_PWM_SOURCE      */ {0.0f, 2.0f, 1.0f},
    /* P_SUB_LEVEL       */ {0.0f, 1.0f, 0.2f},
    /* P_SUB_OCTAVE      */ {0.0f, 1.0f, 0.0f},
    /* P_NOISE_LEVEL     */ {0.0f, 1.0f, 0.0f},
    /* P_DRIFT           */ {0.0f, 1.0f, 0.35f},
    /* P_HPF             */ {0.0f, 1.0f, 0.0f},
    /* P_CUTOFF          */ {0.0f, 1.0f, 0.62f},
    /* P_RESONANCE       */ {0.0f, 1.0f, 0.15f},
    /* P_FILTER_ENV      */ {-1.0f, 1.0f, 0.4f},
    /* P_FILTER_LFO      */ {0.0f, 1.0f, 0.0f},
    /* P_FILTER_KEYTRACK */ {0.0f, 1.0f, 0.5f},
    /* P_OTA_DRIVE       */ {0.0f, 1.0f, 0.25f},
    /* P_FENV_ATTACK     */ {0.001f, 3.0f, 0.005f},
    /* P_FENV_DECAY      */ {0.002f, 8.0f, 0.6f},
    /* P_FENV_SUSTAIN    */ {0.0f, 1.0f, 0.3f},
    /* P_FENV_RELEASE    */ {0.002f, 8.0f, 0.4f},
    /* P_AENV_ATTACK     */ {0.001f, 3.0f, 0.01f},
    /* P_AENV_DECAY      */ {0.002f, 8.0f, 0.5f},
    /* P_AENV_SUSTAIN    */ {0.0f, 1.0f, 0.8f},
    /* P_AENV_RELEASE    */ {0.002f, 8.0f, 0.4f},
    /* P_VCA_MODE        */ {0.0f, 1.0f, 0.0f},
    /* P_LFO_RATE        */ {0.1f, 30.0f, 5.0f},
    /* P_LFO_DELAY       */ {0.0f, 3.0f, 0.0f},
    /* P_LFO_PITCH       */ {0.0f, 1.0f, 0.0f},
    /* P_CHORUS_MODE     */ {0.0f, 3.0f, 1.0f},
    /* P_VOLUME          */ {0.0f, 1.5f, 0.9f},
};

namespace {

constexpr int kCtl = 16;  // control-rate chunk (samples)

inline float polyBLEP(float t, float dt) {
    if (t < dt) {
        float r = t / dt;
        return r + r - r * r - 1.0f;
    } else if (t > 1.0f - dt) {
        float r = (t - 1.0f) / dt;
        return r * r + r + r + 1.0f;
    }
    return 0.0f;
}

inline float fastTanh(float x) {
    x = std::clamp(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float wrap01(float p) {
    p -= std::floor(p);
    return p;
}

inline float tri(float phase) { return 4.0f * std::fabs(phase - std::floor(phase) - 0.5f) - 1.0f; }

inline float timeCoef(float seconds, float ratio, float sr) {
    return 1.0f - std::exp(-ratio / (std::max(seconds, 0.0005f) * sr));
}

}  // namespace

JunoDSP::JunoDSP() {
    init(44100.0f);
}

void JunoDSP::init(float sampleRate) {
    sr = sampleRate;
    for (int i = 0; i < P_COUNT; i++) params[i] = kParamInfo[i].defV;

    // Fixed per-voice component tolerances (deterministic so patches sound the same each load)
    uint32_t s = 0xC0FFEEu;
    auto rnd = [&s]() {
        s = s * 1664525u + 1013904223u;
        return (static_cast<float>(s >> 8) / 8388608.0f) - 1.0f;  // -1..1
    };
    for (int i = 0; i < kVoices; i++) {
        voices[i] = Voice();
        voices[i].tolPitchCents = rnd() * 2.0f;
        voices[i].tolCutoff = rnd();
        voices[i].tolEnvTime = rnd();
        voices[i].tolPw = rnd();
    }
    reset();
}

void JunoDSP::reset() {
    for (auto& v : voices) {
        v.gate = false;
        v.active = false;
        v.note = -1;
        v.fenv = Adsr();
        v.aenv = Adsr();
        v.gateSlew = 0.0f;
        for (float& x : v.s) x = 0.0f;
        v.shelfLp = v.hp1 = v.hp2 = 0.0f;
    }
    lfoPhase = 0.0f;
    std::fill(chorusBuf, chorusBuf + kChorusBuf, 0.0f);
    chorusWrite = 0;
    chorusPhase = 0.0f;
    chorusLpL = chorusLpR = chorusLpL2 = chorusLpR2 = 0.0f;
    chorusDepthSm = 0.0f;
    chorusRateSm = 0.5f;
    chorusMix = 0.0f;
}

float JunoDSP::noise() {
    noiseState ^= noiseState << 13;
    noiseState ^= noiseState >> 17;
    noiseState ^= noiseState << 5;
    return (static_cast<float>(noiseState >> 8) / 8388608.0f) - 1.0f;
}

float JunoDSP::driftNoise() {
    driftState ^= driftState << 13;
    driftState ^= driftState >> 17;
    driftState ^= driftState << 5;
    return (static_cast<float>(driftState >> 8) / 8388608.0f) - 1.0f;
}

void JunoDSP::setParam(int id, float value) {
    if (id < 0 || id >= P_COUNT) return;
    const ParamInfo& pi = kParamInfo[id];
    value = std::clamp(value, pi.minV, pi.maxV);
    switch (id) {
        case P_OCTAVE:
        case P_PWM_SOURCE:
        case P_SUB_OCTAVE:
        case P_VCA_MODE:
        case P_CHORUS_MODE:
            value = std::round(value);
            break;
        default:
            break;
    }
    params[id] = value;
}

float JunoDSP::getParam(int id) const {
    return (id >= 0 && id < P_COUNT) ? params[id] : 0.0f;
}

int JunoDSP::activeVoices() const {
    int n = 0;
    for (const auto& v : voices) n += v.active ? 1 : 0;
    return n;
}

int JunoDSP::allocateVoice(int note) {
    // Retrigger same note
    for (int i = 0; i < kVoices; i++) {
        if (voices[i].active && voices[i].note == note) return i;
    }
    for (int i = 0; i < kVoices; i++) {
        if (!voices[i].active) return i;
    }
    // Steal oldest releasing voice, else oldest overall
    int best = -1;
    uint32_t bestAge = 0xFFFFFFFFu;
    for (int i = 0; i < kVoices; i++) {
        if (!voices[i].gate && voices[i].age < bestAge) {
            bestAge = voices[i].age;
            best = i;
        }
    }
    if (best >= 0) return best;
    best = 0;
    bestAge = voices[0].age;
    for (int i = 1; i < kVoices; i++) {
        if (voices[i].age < bestAge) {
            bestAge = voices[i].age;
            best = i;
        }
    }
    return best;
}

void JunoDSP::noteOn(int note, float velocity) {
    const int idx = allocateVoice(note);
    Voice& v = voices[idx];
    if (!v.active) {
        for (float& x : v.s) x = 0.0f;
        v.shelfLp = v.hp1 = v.hp2 = 0.0f;
        v.fenv.level = 0.0f;
        v.aenv.level = 0.0f;
        v.gateSlew = 0.0f;
        v.driftPitch = driftNoise() * 0.5f;
        v.driftCutoff = driftNoise() * 0.5f;
    }
    v.note = note;
    v.velocity = std::clamp(velocity, 0.0f, 1.0f);
    v.gate = true;
    v.active = true;
    v.age = ++ageCounter;
    v.fenv.gateOn();
    v.aenv.gateOn();
    v.lfoFade = (params[P_LFO_DELAY] <= 0.001f) ? 1.0f : 0.0f;
}

void JunoDSP::noteOff(int note) {
    for (auto& v : voices) {
        if (v.active && v.gate && v.note == note) {
            v.gate = false;
            v.fenv.gateOff();
            v.aenv.gateOff();
        }
    }
}

void JunoDSP::allNotesOff() {
    for (auto& v : voices) {
        if (v.active && v.gate) {
            v.gate = false;
            v.fenv.gateOff();
            v.aenv.gateOff();
        }
    }
}

void JunoDSP::processBlock(float* outL, float* outR, int n) {
    const float invSr = 1.0f / sr;
    const float drift = params[P_DRIFT];
    const bool gateMode = params[P_VCA_MODE] > 0.5f;
    const float lfoRate = params[P_LFO_RATE];
    const float lfoDelay = params[P_LFO_DELAY];
    const float lfoPitch = params[P_LFO_PITCH];
    const float filterLfo = params[P_FILTER_LFO];
    const int pwmSrc = static_cast<int>(params[P_PWM_SOURCE]);
    const float pwmDepth = params[P_PWM_DEPTH];

    const float sawL = params[P_SAW_LEVEL];
    const float pulseL = params[P_PULSE_LEVEL];
    const float subL = params[P_SUB_LEVEL];
    const float noiseL = params[P_NOISE_LEVEL];
    const float subDiv = params[P_SUB_OCTAVE] > 0.5f ? 0.25f : 0.5f;

    const float res = params[P_RESONANCE];
    const float ladderK = res * 4.4f;
    const float inGain = 1.0f + params[P_OTA_DRIVE] * 3.0f;
    const float driveComp = 1.0f / (1.0f + params[P_OTA_DRIVE] * 1.2f);
    const float cutoffNorm = params[P_CUTOFF];
    const float envAmt = params[P_FILTER_ENV];
    const float keyTrack = params[P_FILTER_KEYTRACK];

    // HPF: 0 = bass boost (+3.5dB low shelf @ 70Hz), then continuous cutoff 20Hz..1.2kHz
    const float hpf = params[P_HPF];
    const float boostMix = std::clamp(1.0f - hpf / 0.08f, 0.0f, 1.0f);
    const float shelfGainExtra = (std::pow(10.0f, 3.5f / 20.0f) - 1.0f) * boostMix;
    const float shelfA = 1.0f - std::exp(-2.0f * kPi * 70.0f * invSr);
    const float hpHz = 20.0f * std::pow(60.0f, hpf);
    const float hpA = 1.0f - std::exp(-2.0f * kPi * hpHz * invSr);

    // Chorus mode targets
    const int chMode = static_cast<int>(params[P_CHORUS_MODE]);
    float chDepthTarget = 0.0f, chRateTarget = 0.5f;
    if (chMode == 1) { chDepthTarget = 0.0015f; chRateTarget = 0.513f; }
    else if (chMode == 2) { chDepthTarget = 0.0021f; chRateTarget = 0.863f; }
    else if (chMode == 3) { chDepthTarget = 0.00025f; chRateTarget = 9.75f; }
    const float chMixTarget = chMode > 0 ? 1.0f : 0.0f;
    const float chCenter = 0.0033f * sr;
    const float chLpA = 1.0f - std::exp(-2.0f * kPi * 8500.0f * invSr);

    const float volume = params[P_VOLUME];
    const float gateCoef = 1.0f - std::exp(-1.0f / (0.003f * sr));

    int done = 0;
    while (done < n) {
        const int chunk = std::min(kCtl, n - done);

        // ---- control-rate updates ----
        lfoPhase = wrap01(lfoPhase + lfoRate * invSr * chunk);
        const float lfo = tri(lfoPhase);

        struct VCtl {
            float dt, subDt, pw, G, G4, oneMinusG;
            float aA, aD, aR, fA, fD, fR;
            float velGain;
        } vc[kVoices];

        for (int vi = 0; vi < kVoices; vi++) {
            Voice& v = voices[vi];
            if (!v.active) continue;
            VCtl& c = vc[vi];

            // thermal wander (leaky random walk)
            const float wa = 0.0002f;
            v.driftPitch = std::clamp(v.driftPitch * (1.0f - wa) + driftNoise() * wa * 90.0f, -1.0f, 1.0f);
            v.driftCutoff = std::clamp(v.driftCutoff * (1.0f - wa) + driftNoise() * wa * 90.0f, -1.0f, 1.0f);

            if (lfoDelay > 0.001f && v.lfoFade < 1.0f) {
                v.lfoFade = std::min(1.0f, v.lfoFade + chunk * invSr / lfoDelay);
            }
            const float voiceLfo = lfo * v.lfoFade;

            // Pitch
            const float octShift = (params[P_OCTAVE] - 1.0f) * 12.0f;
            float semis = static_cast<float>(v.note) + octShift + params[P_TUNE];
            semis += drift * (v.tolPitchCents + v.driftPitch * 3.5f) * 0.01f;
            semis += voiceLfo * lfoPitch * 1.5f;
            float freq = 440.0f * std::exp2((semis - 69.0f) * (1.0f / 12.0f));
            freq = std::clamp(freq, 8.0f, sr * 0.45f);
            c.dt = freq * invSr;
            c.subDt = c.dt * subDiv;

            // Pulse width
            float pwMod = 0.0f;
            if (pwmSrc == 1) pwMod = lfo * pwmDepth * 0.45f;
            else if (pwmSrc == 2) pwMod = v.fenv.level * pwmDepth * 0.45f;
            c.pw = std::clamp(params[P_PULSE_WIDTH] + pwMod + drift * v.tolPw * 0.015f, 0.05f, 0.95f);

            // Filter cutoff (octave domain)
            float cutHz = 20.0f * std::pow(900.0f, cutoffNorm);
            float oct = envAmt * v.fenv.level * 5.0f;
            oct += filterLfo * voiceLfo * 1.5f;
            oct += keyTrack * (static_cast<float>(v.note) - 60.0f) * (1.0f / 12.0f);
            oct += drift * (v.tolCutoff * 0.08f + v.driftCutoff * 0.05f);
            cutHz = std::clamp(cutHz * std::exp2(oct), 20.0f, sr * 0.45f);
            const float g = std::tan(kPi * cutHz * invSr);
            c.G = g / (1.0f + g);
            c.oneMinusG = 1.0f - c.G;
            c.G4 = c.G * c.G * c.G * c.G;

            // Envelope coefficients (component spread on timing)
            const float tf = 1.0f + drift * v.tolEnvTime * 0.08f;
            c.aA = timeCoef(params[P_AENV_ATTACK] * tf, 1.466f, sr);
            c.aD = timeCoef(params[P_AENV_DECAY] * tf, 4.6f, sr);
            c.aR = timeCoef(params[P_AENV_RELEASE] * tf, 4.6f, sr);
            c.fA = timeCoef(params[P_FENV_ATTACK] * tf, 1.466f, sr);
            c.fD = timeCoef(params[P_FENV_DECAY] * tf, 4.6f, sr);
            c.fR = timeCoef(params[P_FENV_RELEASE] * tf, 4.6f, sr);

            c.velGain = 0.35f + 0.65f * v.velocity;
        }

        // chorus smoothing at control rate
        chorusDepthSm += (chDepthTarget - chorusDepthSm) * 0.02f;
        chorusRateSm += (chRateTarget - chorusRateSm) * 0.02f;
        chorusMix += (chMixTarget - chorusMix) * 0.02f;

        // ---- audio-rate loop ----
        for (int i = 0; i < chunk; i++) {
            float mono = 0.0f;

            for (int vi = 0; vi < kVoices; vi++) {
                Voice& v = voices[vi];
                if (!v.active) continue;
                const VCtl& c = vc[vi];

                const float fenvLevel = v.fenv.process(c.fA, c.fD, params[P_FENV_SUSTAIN], c.fR);
                (void)fenvLevel;
                const float aenvLevel = v.aenv.process(c.aA, c.aD, params[P_AENV_SUSTAIN], c.aR);
                v.gateSlew += ((v.gate ? 1.0f : 0.0f) - v.gateSlew) * gateCoef;

                // DCO
                v.phase += c.dt;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                v.subPhase += c.subDt;
                if (v.subPhase >= 1.0f) v.subPhase -= 1.0f;

                float osc = 0.0f;
                if (sawL > 0.0f) {
                    float saw = 2.0f * v.phase - 1.0f;
                    saw -= polyBLEP(v.phase, c.dt);
                    osc += saw * sawL * 0.5f;
                }
                if (pulseL > 0.0f) {
                    float p = (v.phase < c.pw) ? 1.0f : -1.0f;
                    p += polyBLEP(v.phase, c.dt);
                    p -= polyBLEP(wrap01(v.phase - c.pw + 1.0f), c.dt);
                    p -= (2.0f * c.pw - 1.0f);
                    osc += p * pulseL * 0.5f;
                }
                if (subL > 0.0f) {
                    float sq = (v.subPhase < 0.5f) ? 1.0f : -1.0f;
                    sq += polyBLEP(v.subPhase, c.subDt);
                    sq -= polyBLEP(wrap01(v.subPhase + 0.5f), c.subDt);
                    osc += sq * subL * 0.5f;
                }
                if (noiseL > 0.0f) osc += noise() * noiseL * 0.4f;

                // HPF (shelf boost + 12dB highpass)
                v.shelfLp += shelfA * (osc - v.shelfLp);
                float x = osc + shelfGainExtra * v.shelfLp;
                v.hp1 += hpA * (x - v.hp1);
                x -= v.hp1;
                v.hp2 += hpA * (x - v.hp2);
                x -= v.hp2;

                // 4-pole OTA ladder (TPT with instantaneous feedback solve)
                const float G = c.G;
                const float S = c.oneMinusG * (G * G * G * v.s[0] + G * G * v.s[1] + G * v.s[2] + v.s[3]);
                float u = (x * inGain - ladderK * S) / (1.0f + ladderK * c.G4);
                u = fastTanh(u);
                float in = u;
                for (int k = 0; k < 4; k++) {
                    const float vv = G * (in - v.s[k]);
                    const float y = vv + v.s[k];
                    v.s[k] = y + vv;
                    in = y;
                }
                float filtered = in * driveComp;

                // VCA
                const float amp = gateMode ? v.gateSlew : aenvLevel;
                mono += filtered * amp * c.velGain * 0.8f;

                // voice lifecycle
                if (gateMode) {
                    if (!v.gate && v.gateSlew < 0.0001f) v.active = false;
                } else {
                    if (!v.gate && v.aenv.stage == Adsr::IDLE) v.active = false;
                }
                if (!v.active) {
                    for (float& st : v.s) st = 0.0f;
                }
            }

            // ---- Stereo BBD chorus ----
            chorusBuf[chorusWrite] = mono;
            chorusPhase += chorusRateSm * invSr;
            if (chorusPhase >= 1.0f) chorusPhase -= 1.0f;
            const float modL = tri(chorusPhase);
            const float modR = -modL;
            const float dL = chCenter + modL * chorusDepthSm * sr;
            const float dR = chCenter + modR * chorusDepthSm * sr;
            auto readDelay = [&](float d) {
                float rp = static_cast<float>(chorusWrite) - d;
                while (rp < 0.0f) rp += kChorusBuf;
                const int i0 = static_cast<int>(rp);
                const float fr = rp - i0;
                const int i1 = (i0 + 1) % kChorusBuf;
                return chorusBuf[i0 % kChorusBuf] * (1.0f - fr) + chorusBuf[i1] * fr;
            };
            float wetL = readDelay(dL);
            float wetR = readDelay(dR);
            chorusLpL += chLpA * (wetL - chorusLpL);
            chorusLpL2 += chLpA * (chorusLpL - chorusLpL2);
            chorusLpR += chLpA * (wetR - chorusLpR);
            chorusLpR2 += chLpA * (chorusLpR - chorusLpR2);
            chorusWrite = (chorusWrite + 1) % kChorusBuf;

            const float dryAmt = 1.0f - 0.4f * chorusMix;
            const float wetAmt = 0.6f * chorusMix;
            float l = mono * dryAmt + chorusLpL2 * wetAmt;
            float r = mono * dryAmt + chorusLpR2 * wetAmt;

            outL[done + i] = fastTanh(l * volume * 1.2f);
            outR[done + i] = fastTanh(r * volume * 1.2f);
        }
        done += chunk;
    }
}

}  // namespace juno
