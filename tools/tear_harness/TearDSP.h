#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace tear {

constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

// Fast tanh approximation
inline float fastTanh(float x) {
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Fast rational approximation of tan(x) for x in [0, pi/2)
inline float fastTan(float x) {
    const float x2 = x * x;
    return x * (1.0f - 0.08333333f * x2) / (1.0f - 0.41666667f * x2);
}

// PolyBLEP anti-aliasing residual for step discontinuity at t=0
// t in [0, 1) is time past discontinuity divided by phase increment dt
inline float polyBLEP(float t, float dt) {
    if (dt <= 0.0f) return 0.0f;
    if (t < dt) {
        float r = t / dt;
        return r + r - r * r - 1.0f;
    } else if (t > 1.0f - dt) {
        float r = (t - 1.0f) / dt;
        return r * r + r + r + 1.0f;
    }
    return 0.0f;
}

// PolyBLAMP anti-aliasing residual for slope discontinuity (triangle corners)
inline float polyBLAMP(float t, float dt) {
    if (dt <= 0.0f) return 0.0f;
    if (t < dt) {
        float r = t / dt - 1.0f;
        return -r * r * r * (dt / 3.0f);
    } else if (t > 1.0f - dt) {
        float r = (t - 1.0f) / dt + 1.0f;
        return r * r * r * (dt / 3.0f);
    }
    return 0.0f;
}

enum SyncMode : uint8_t {
    SYNC_HARD_TEAR = 0,
    SYNC_SOFT_REVERSE = 1,
    SYNC_WINDOWED_FORMANT = 2,
    SYNC_DUAL_SUPERTEAR = 3,
    SYNC_RING_SHRED = 4
};

enum FilterType : uint8_t {
    FILTER_LADDER_24 = 0,
    FILTER_WARM_SVF_12 = 1,
    FILTER_BYPASS = 2
};

enum SaturationType : uint8_t {
    SAT_MODERN_SOFT = 0,
    SAT_CLASSIC_HARD = 1,
    SAT_TUBE_VALVE = 2,
    SAT_TAPE = 3,
    SAT_TRANSFORMER = 4
};

enum ParamId : uint8_t {
    // 1. Oscillator (0..6)
    PARAM_SYNC_MODE = 0,     // 0 .. 4 (SyncMode)
    PARAM_SYNC_RATIO = 1,    // 0.0 .. 48.0 (semitones above master)
    PARAM_SLAVE_SHAPE = 2,   // 0.0 (Saw) .. 0.5 (Pulse) .. 1.0 (DualSaw)
    PARAM_SUB_LEVEL = 3,     // 0.0 .. 1.0
    PARAM_SUB_OCTAVE = 4,    // 0 (-1 oct), 1 (-2 oct)
    PARAM_TUNE = 5,          // -24 .. +24 semitones
    PARAM_FINE = 6,          // -100 .. +100 cents

    // 2. Modulation (7..10)
    PARAM_SYNC_ENV_DECAY = 7,// 0.005 .. 3.0 seconds
    PARAM_SYNC_ENV_DEPTH = 8,// -48.0 .. +48.0 semitones
    PARAM_SYNC_LFO_RATE = 9, // 0.05 .. 25.0 Hz
    PARAM_SYNC_LFO_DEPTH = 10,// 0.0 .. 24.0 semitones

    // 3. Filter (11..14)
    PARAM_FILTER_TYPE = 11,  // 0 (Ladder 24), 1 (SVF 12), 2 (Bypass)
    PARAM_CUTOFF = 12,       // 20.0 .. 18000.0 Hz
    PARAM_RESONANCE = 13,    // 0.0 .. 0.95
    PARAM_FILTER_ENV_AMOUNT = 14, // -1.0 .. 1.0

    // 4. Filter Envelope (15..18)
    PARAM_FILTER_ATTACK = 15,// 0.001 .. 2.0 s
    PARAM_FILTER_DECAY = 16, // 0.01 .. 3.0 s
    PARAM_FILTER_SUSTAIN = 17,// 0.0 .. 1.0
    PARAM_FILTER_RELEASE = 18,// 0.01 .. 3.0 s

    // 5. Amplifier & Saturation (19..24)
    PARAM_DRIVE = 19,        // 0.0 .. 1.0
    PARAM_SATURATION_TYPE = 20,// 0 .. 4
    PARAM_AMP_ATTACK = 21,   // 0.001 .. 2.0 s
    PARAM_AMP_DECAY = 22,    // 0.01 .. 3.0 s
    PARAM_AMP_SUSTAIN = 23,  // 0.0 .. 1.0
    PARAM_AMP_RELEASE = 24,  // 0.01 .. 3.0 s

    // 6. Voice (25..27)
    PARAM_PLAY_MODE = 25,    // 0: Always, 1: Overlap, 2: Legato
    PARAM_GLIDE_TIME = 26,   // 0.0 .. 2.0 s
    PARAM_VOLUME = 27,       // 0.0 .. 2.0 (1.0 = 100%)

    PARAM_COUNT = 28
};

struct ADSR {
    enum State { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };
    State state = IDLE;
    float value = 0.0f;
    float attackRate = 0.01f;
    float decayRate = 0.001f;
    float sustainLevel = 0.7f;
    float releaseRate = 0.001f;

    void setAttack(float seconds, float sampleRate) {
        float samples = std::max(1.0f, seconds * sampleRate);
        attackRate = 1.0f / samples;
    }
    void setDecay(float seconds, float sampleRate) {
        float samples = std::max(1.0f, seconds * sampleRate);
        decayRate = 1.0f / samples;
    }
    void setSustain(float level) {
        sustainLevel = std::clamp(level, 0.0f, 1.0f);
    }
    void setRelease(float seconds, float sampleRate) {
        float samples = std::max(1.0f, seconds * sampleRate);
        releaseRate = 1.0f / samples;
    }

    void gate(bool on) {
        if (on) {
            state = ATTACK;
        } else if (state != IDLE) {
            state = RELEASE;
        }
    }

    inline float process() {
        switch (state) {
            case ATTACK:
                value += attackRate;
                if (value >= 1.0f) {
                    value = 1.0f;
                    state = DECAY;
                }
                break;
            case DECAY:
                value -= decayRate;
                if (value <= sustainLevel) {
                    value = sustainLevel;
                    state = SUSTAIN;
                }
                break;
            case SUSTAIN:
                value = sustainLevel;
                break;
            case RELEASE:
                value -= releaseRate;
                if (value <= 0.00001f) {
                    value = 0.0f;
                    state = IDLE;
                }
                break;
            case IDLE:
            default:
                value = 0.0f;
                break;
        }
        return value;
    }

    bool isActive() const { return state != IDLE; }
};

class TearDSP {
public:
    TearDSP();
    void init(float sampleRate);
    void reset();

    void noteOn(uint8_t note, float velocity);
    void noteOff();
    void setParam(uint8_t paramId, float value);
    float getParam(uint8_t paramId) const;

    void processBlock(float* output, int numSamples);
    bool isPlaying() const { return ampEnv.isActive(); }

private:
    float sampleRate = 44100.0f;
    float invSampleRate = 1.0f / 44100.0f;

    // Pitch & Voice state
    uint8_t currentNote = 60;
    float currentVelocity = 1.0f;
    float baseFrequency = 261.63f;

    // Parameters
    float params[PARAM_COUNT];

    // Envelopes
    ADSR ampEnv;
    ADSR filterEnv;
    
    // Sync Decay Envelope
    float syncEnvVal = 0.0f;
    float syncEnvRate = 0.001f;

    // Sync LFO
    float syncLfoPhase = 0.0f;

    // Master Oscillator
    float masterPhase = 0.0f;

    // Slave Oscillators
    float slavePhase1 = 0.0f;
    float slavePhase2 = 0.0f;     // For Dual SuperTear
    float slaveDirection = 1.0f;  // For Soft Reverse mode

    // Sub Oscillator
    float subPhase = 0.0f;

    // Ladder ZDF Filter States (4 stages)
    float ladderS[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // SVF ZDF Filter States (2 integrators)
    float svfIc1eq = 0.0f;
    float svfIc2eq = 0.0f;

    // Saturation memory (for Tape & Transformer)
    float tapeHist = 0.0f;
    float transMag = 0.0f;

    // Internal helpers
    float processSample();
    float applySaturation(float in, float drive, SaturationType type);
    float renderLadder(float in, float cutoffHz, float res);
    float renderWarmSVF(float in, float cutoffHz, float res);
};

} // namespace tear
