#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "../../core/libs/Audio/WavetableCore.h"

namespace wt {

constexpr int kVoices = 8;
constexpr int kBuiltinTableCount = 4;
constexpr int kBuiltinWindowSize = 2048;
constexpr int kBuiltinWindowCount = 64;

enum ParamId : int {
    P_TABLE = 0,          // 0..4 (0..3 built-in, 4 = custom loaded)
    P_POSITION,           // 0..1 normalized frame position
    P_WINDOW_SIZE,        // 0..6 -> {32, 64, 128, 256, 512, 1024, 2048}
    P_WARP_MODE,          // 0=Off, 1=Asym, 2=Sync, 3=Fold
    P_WARP_AMOUNT,        // 0..100 (%)
    P_SUB_WAVE,           // 0=Off, 1=Sine, 2=Triangle, 3=Square
    P_SUB_LEVEL,          // 0..100 (%)
    P_WT_ENV_MODE,        // 0=Off, 1=WT Env (One-Shot), 2=WT LFO (Loop)
    P_WT_ENV_SHAPE,       // 0..127 factory wavetable envelope shapes
    P_WT_ENV_WARP_X,      // -7..7
    P_WT_ENV_WARP_Y,      // -7..7
    P_WT_ENV_SPEED,       // 0.05..10.0 seconds
    P_WT_ENV_SUST,        // 0..99 (%) sustain freeze point
    P_WT_ENV_TO_POS,      // -1..1
    P_WT_ENV_TO_WARP,     // -1..1
    P_FILTER_ENABLE,      // 0..1
    P_FILTER_TYPE,        // 0=LP, 1=HP, 2=BP
    P_CUTOFF,             // 0..1
    P_RESONANCE,          // 0..1
    P_AENV_ATTACK,        // 0.001..5.0 seconds
    P_AENV_DECAY,         // 0.005..5.0 seconds
    P_AENV_SUSTAIN,       // 0..1
    P_AENV_RELEASE,       // 0.005..8.0 seconds
    P_TUNE,               // -24..24 semitones
    P_FINETUNE,           // -100..100 cents
    P_VOLUME,             // 0..1.5
    P_COUNT
};

struct ParamInfo {
    float minV, maxV, defV;
};

extern const ParamInfo kParamInfo[P_COUNT];

struct AmpAdsr {
    enum Stage { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };
    Stage stage = IDLE;
    float level = 0.0f;

    void gateOn() { stage = ATTACK; }
    void gateOff() {
        if (stage != IDLE) stage = RELEASE;
    }

    inline float process(float aCoef, float dCoef, float sustain, float rCoef) {
        switch (stage) {
            case ATTACK:
                level += (1.25f - level) * aCoef;
                if (level >= 1.0f) {
                    level = 1.0f;
                    stage = DECAY;
                }
                break;
            case DECAY:
                level += (sustain - level) * dCoef;
                if (std::fabs(level - sustain) < 0.0005f) {
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
                break;
        }
        return level;
    }
};

struct WtVoiceEnv {
    enum Phase { ENV_IDLE, ENV_ACTIVE, ENV_SUSTAIN, ENV_RELEASE };
    Phase phase = ENV_IDLE;
    float elapsedSec = 0.0f;
    float releaseStartPhase = 0.0f;
    float output = 0.0f;

    void trigger() {
        phase = ENV_ACTIVE;
        elapsedSec = 0.0f;
        releaseStartPhase = 0.0f;
    }
    void release(float durSec) {
        if (phase == ENV_ACTIVE || phase == ENV_SUSTAIN) {
            releaseStartPhase = (durSec > 0.0f) ? (elapsedSec / durSec) : 1.0f;
            if (releaseStartPhase > 1.0f) releaseStartPhase = 1.0f;
            phase = ENV_RELEASE;
            elapsedSec = 0.0f;
        }
    }
    void reset() {
        phase = ENV_IDLE;
        elapsedSec = 0.0f;
        releaseStartPhase = 0.0f;
        output = 0.0f;
    }
};

struct Voice {
    bool gate = false;
    bool active = false;
    int note = -1;
    float velocity = 1.0f;
    uint32_t age = 0;

    WtCoreVoiceState core;
    WtVoiceEnv wtEnv;
    AmpAdsr aenv;

    // 2-pole SVF state
    float ic1eq = 0.0f;
    float ic2eq = 0.0f;

    // Live modulation telemetry for UI visualizers
    float livePosNorm = 0.0f;
    float liveWarpPct = 0.0f;
    float liveEnvPhase01 = 0.0f;
};

class WavetableDSP {
public:
    WavetableDSP();
    void init(float sampleRate);
    void reset();
    void noteOn(int note, float velocity);
    void noteOff(int note);
    void allNotesOff();

    void setParam(int id, float value);
    float getParam(int id) const;
    int activeVoices() const;

    void processBlock(float* outL, float* outR, int numSamples);

    bool loadPcm16(const int16_t* samples, int numSamples, int windowSize);
    int getWindowSize() const;
    int getWindowCount() const;
    int getTotalTableSamples() const;

    void getDisplayWave(float* outBuf, int numPoints) const;
    void getEnvPreview(float* outBuf, int numPoints) const;
    int getModState(float* out3) const;

    int getExportPtiSize() const;
    int exportPti(uint8_t* outBuf, int maxLen, const char* name) const;
    bool importPti(const uint8_t* data, int byteLen);

    static int windowSizeFromIndex(int idx);
    static int indexFromWindowSize(int size);

private:
    const int16_t* activeTablePtr(uint32_t& outSamples, uint32_t& outWinSize, uint32_t& outWinCount) const;
    void generateBuiltinTables();
    void generateBuiltinEnvelopes();
    float evalWtEnv(float phase01, uint8_t shapeIdx, int8_t wx, int8_t wy) const;
    float stepVoiceWtEnv(Voice& v, float dtSec, bool gateHeld);

    float sr = 44100.0f;
    float params[P_COUNT];
    Voice voices[kVoices];
    uint32_t noteCounter = 0;

    std::vector<int16_t> builtinTables[kBuiltinTableCount];
    std::vector<int16_t> customTable;
    int customWinSize = 2048;

    static constexpr int kEnvCount = 128;
    static constexpr int kEnvLength = 128;
    int16_t envBank[kEnvCount][kEnvLength];
};

} // namespace wt
