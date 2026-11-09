#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// JUNO-style 8-voice polyphonic synth engine (WASM test harness).
// Architecture: DCO (saw/pulse/sub/noise) -> HPF (continuous, bass boost at 0)
//   -> 24dB OTA-style resonant LPF -> VCA -> mono sum -> stereo BBD chorus.
// Two independent ADSR envelopes (filter + amp), delayed triangle LFO,
// and a "Drift" control modelling analog component tolerance + thermal wander.

namespace juno {

constexpr int kVoices = 8;
constexpr float kPi = 3.14159265358979323846f;

enum ParamId : int {
    // DCO
    P_OCTAVE = 0,       // 0=16' 1=8' 2=4'
    P_TUNE,             // -12 .. +12 semitones
    P_SAW_LEVEL,        // 0..1
    P_PULSE_LEVEL,      // 0..1
    P_PULSE_WIDTH,      // 0.05..0.95 (manual width)
    P_PWM_DEPTH,        // 0..1
    P_PWM_SOURCE,       // 0=manual 1=LFO 2=ENV (filter env)
    P_SUB_LEVEL,        // 0..1
    P_SUB_OCTAVE,       // 0=-1 oct 1=-2 oct
    P_NOISE_LEVEL,      // 0..1
    P_DRIFT,            // 0..1
    // Filters
    P_HPF,              // 0..1 (0 = bass boost, then rising cutoff)
    P_CUTOFF,           // 0..1 (exponential 20Hz..18kHz)
    P_RESONANCE,        // 0..1
    P_FILTER_ENV,       // -1..1
    P_FILTER_LFO,       // 0..1
    P_FILTER_KEYTRACK,  // 0..1
    P_OTA_DRIVE,        // 0..1
    // Filter envelope
    P_FENV_ATTACK,      // seconds
    P_FENV_DECAY,
    P_FENV_SUSTAIN,
    P_FENV_RELEASE,
    // Amp envelope
    P_AENV_ATTACK,
    P_AENV_DECAY,
    P_AENV_SUSTAIN,
    P_AENV_RELEASE,
    P_VCA_MODE,         // 0=ENV 1=GATE
    // LFO
    P_LFO_RATE,         // Hz
    P_LFO_DELAY,        // seconds
    P_LFO_PITCH,        // 0..1 (vibrato depth)
    // Chorus / output
    P_CHORUS_MODE,      // 0=off 1=I 2=II 3=I+II
    P_VOLUME,           // 0..1.5
    P_COUNT
};

struct ParamInfo {
    float minV, maxV, defV;
};
extern const ParamInfo kParamInfo[P_COUNT];

struct Adsr {
    enum Stage { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };
    Stage stage = IDLE;
    float level = 0.0f;

    void gateOn() { stage = ATTACK; }
    void gateOff() {
        if (stage != IDLE) stage = RELEASE;
    }
    // coefficients are per-sample one-pole factors
    inline float process(float aCoef, float dCoef, float sustain, float rCoef) {
        switch (stage) {
            case ATTACK:
                level += (1.3f - level) * aCoef;  // RC charge toward overshoot target
                if (level >= 1.0f) {
                    level = 1.0f;
                    stage = DECAY;
                }
                break;
            case DECAY:
                level += (sustain - level) * dCoef;
                if (level - sustain < 0.0005f) {
                    level = sustain;
                    stage = SUSTAIN;
                }
                break;
            case SUSTAIN:
                level = sustain;
                break;
            case RELEASE:
                level -= level * rCoef;
                if (level < 0.0001f) {
                    level = 0.0f;
                    stage = IDLE;
                }
                break;
            default:
                level = 0.0f;
        }
        return level;
    }
};

struct Voice {
    bool gate = false;
    bool active = false;
    int note = -1;
    float velocity = 1.0f;
    uint32_t age = 0;

    float phase = 0.0f;
    float subPhase = 0.0f;

    Adsr fenv, aenv;
    float gateSlew = 0.0f;
    float lfoFade = 0.0f;

    // 4-pole TPT ladder states
    float s[4] = {0, 0, 0, 0};
    // HPF states: shelf lowpass + 2 one-pole highpass
    float shelfLp = 0.0f;
    float hp1 = 0.0f, hp2 = 0.0f;

    // static per-voice component tolerances (fixed at init)
    float tolPitchCents = 0.0f;
    float tolCutoff = 0.0f;   // fractional
    float tolEnvTime = 0.0f;  // fractional
    float tolPw = 0.0f;
    // dynamic thermal wander
    float driftPitch = 0.0f;
    float driftCutoff = 0.0f;
};

class JunoDSP {
public:
    JunoDSP();
    void init(float sampleRate);
    void reset();
    void noteOn(int note, float velocity);
    void noteOff(int note);
    void allNotesOff();
    void setParam(int id, float value);
    float getParam(int id) const;
    int activeVoices() const;
    void processBlock(float* outL, float* outR, int n);

private:
    float sr = 44100.0f;
    float params[P_COUNT];
    uint32_t noiseState = 0x12345678u;
    uint32_t driftState = 0x9e3779b9u;
    uint32_t ageCounter = 0;
    Voice voices[kVoices];

    // LFO
    float lfoPhase = 0.0f;

    // Chorus
    static constexpr int kChorusBuf = 4096;
    float chorusBuf[kChorusBuf];
    int chorusWrite = 0;
    float chorusPhase = 0.0f;
    float chorusLpL = 0.0f, chorusLpR = 0.0f, chorusLpL2 = 0.0f, chorusLpR2 = 0.0f;
    float chorusDepthSm = 0.0f, chorusRateSm = 0.5f, chorusMix = 0.0f;

    float noise();
    float driftNoise();
    int allocateVoice(int note);
};

}  // namespace juno
