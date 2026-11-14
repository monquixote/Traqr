#include "WavetableDSP.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace wt {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

constexpr int kWindowSizes[7] = { 32, 64, 128, 256, 512, 1024, 2048 };
constexpr int kChunkSize = 128;

// Exact Tracker tempoSyncRatesOthers[29] for mapping WT LFO speed <-> seconds at 120 BPM.
constexpr int kSyncRatesOtherCount = 29;
constexpr float kTempoSyncRatesOthers[kSyncRatesOtherCount] = {
    128.0f,   96.0f,     64.0f,    48.0f,     32.0f,    24.0f,     16.0f,    12.0f,
    8.0f,     6.0f,      4.0f,     3.0f,      2.0f,     1.5f,      1.0f,     0.75f,
    0.5f,     0.375f,    0.333333f,0.25f,     0.1875f,  0.166667f, 0.125f,   0.083333f,
    0.0625f,  0.041667f, 0.03125f, 0.020833f, 0.015625f
};

inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

inline float timeCoef(float seconds, float ratio, float sr)
{
    return 1.0f - std::exp(-ratio / (std::max(seconds, 0.0005f) * sr));
}

inline float softSat(float x)
{
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x * (1.0f - (4.0f / 27.0f) * x * x);
}

inline int8_t unpackWarpX(uint8_t wtBend)
{
    uint8_t nx = (wtBend >> 4) & 0x0F;
    if (nx <= 7) return (int8_t)nx;
    if (nx >= 9) return (int8_t)(nx - 16);
    return 0;
}

inline int8_t unpackWarpY(uint8_t wtBend)
{
    uint8_t ny = wtBend & 0x0F;
    if (ny <= 7) return (int8_t)ny;
    if (ny >= 9) return (int8_t)(ny - 16);
    return 0;
}

inline uint8_t packWarpXY(int8_t warpX, int8_t warpY)
{
    if (warpX < -7) warpX = -7; else if (warpX > 7) warpX = 7;
    if (warpY < -7) warpY = -7; else if (warpY > 7) warpY = 7;
    uint8_t nx = (warpX >= 0) ? (uint8_t)warpX : (uint8_t)(16 + warpX);
    uint8_t ny = (warpY >= 0) ? (uint8_t)warpY : (uint8_t)(16 + warpY);
    return (uint8_t)(((nx & 0x0F) << 4) | (ny & 0x0F));
}

uint32_t crc32Ieee(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
        {
            uint32_t mask = -(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

// Binary-exact Polyend Tracker .pti layout matching core/source/mtStructs.h and
// core/FileManager/fileManagerDefs.h (always 32-bit portable via uint32_t address placeholder).
#pragma pack(push, 4)
struct PtiHeader
{
    char id_file[2];
    uint16_t type;
    char fwVersion[4];
    char fileStructureVersion[4];
    uint16_t size;
};

struct PtiEnv
{
    float amount;
    uint16_t delay;
    uint16_t attack;
    uint16_t hold;
    uint16_t decay;
    float sustain;
    uint16_t release;
    uint8_t loop;
    uint8_t enable;
};

struct PtiLfo
{
    uint8_t shape;
    uint8_t speed;
    uint8_t wtBend;
    uint8_t wtSust;
    float amount;
};

struct PtiSampleSlot
{
    uint8_t type;
    char file_name[32];
    uint32_t address;
    uint32_t length;
    uint16_t wavetable_window_size;
    uint32_t wavetableWindowNumber;
};

struct PtiGranular
{
    uint16_t grainLength;
    uint16_t currentPosition;
    uint8_t shape;
    uint8_t type;
};

struct PtiInstrument
{
    uint8_t isActive;
    PtiSampleSlot sample;
    char name[4];
    uint8_t playMode;
    uint8_t chorusSend;
    uint16_t startPoint;
    uint16_t loopPoint1;
    uint16_t loopPoint2;
    uint16_t endPoint;
    uint16_t wavetableCurrentWindow;
    uint8_t wtSubWarpByte; // bits 0..1 = wtSubWave, bits 2..3 = wtWarpMode
    uint8_t wtSubLevel;
    PtiEnv envelope[6];
    PtiLfo lfo[6];
    float cutOff;
    float resonance;
    uint8_t filterType;
    uint8_t filterEnable;
    int8_t tune;
    int8_t fineTune;
    uint8_t volume;
    uint8_t wtWarpAmount;
    uint16_t glide;
    int16_t panning;
    uint8_t delaySend;
    uint8_t slicePlayThru;
    uint16_t slices[48];
    uint8_t sliceNumber;
    uint8_t selectedSlice;
    PtiGranular granular;
    uint8_t reverbSend;
    uint8_t overdrive;
    uint8_t bitDepth;
    int8_t tuneResample;
};

struct PtiFile
{
    struct DataAndHeader
    {
        PtiHeader instrHeader;
        PtiInstrument instrument;
    } instrumentDataAndHeader;
    uint32_t crc;
};
#pragma pack(pop)

static_assert(sizeof(PtiHeader) == 14, "PtiHeader must be 14 bytes");
static_assert(sizeof(PtiEnv) == 20, "PtiEnv must be 20 bytes");
static_assert(sizeof(PtiLfo) == 8, "PtiLfo must be 8 bytes");
static_assert(sizeof(PtiInstrument) == 372, "PtiInstrument must remain 372 bytes");
static_assert(sizeof(PtiFile) == 392, "PtiFile header+instrument+crc must be 392 bytes");
static_assert(offsetof(PtiFile, instrumentDataAndHeader) +
                  offsetof(PtiFile::DataAndHeader, instrument) == 16,
              "Instrument offset in .pti must be 16");

} // namespace

const ParamInfo kParamInfo[P_COUNT] = {
    /* P_TABLE          */ { 0.0f, 4.0f, 0.0f },
    /* P_POSITION       */ { 0.0f, 1.0f, 0.0f },
    /* P_WINDOW_SIZE    */ { 0.0f, 6.0f, 6.0f },
    /* P_WARP_MODE      */ { 0.0f, 3.0f, 0.0f },
    /* P_WARP_AMOUNT    */ { 0.0f, 100.0f, 0.0f },
    /* P_SUB_WAVE       */ { 0.0f, 3.0f, 0.0f },
    /* P_SUB_LEVEL      */ { 0.0f, 100.0f, 25.0f },
    /* P_WT_ENV_MODE    */ { 0.0f, 2.0f, 1.0f },
    /* P_WT_ENV_SHAPE   */ { 0.0f, 127.0f, 16.0f },
    /* P_WT_ENV_WARP_X  */ { -7.0f, 7.0f, 0.0f },
    /* P_WT_ENV_WARP_Y  */ { -7.0f, 7.0f, 0.0f },
    /* P_WT_ENV_SPEED   */ { 0.05f, 10.0f, 1.2f },
    /* P_WT_ENV_SUST    */ { 0.0f, 99.0f, 0.0f },
    /* P_WT_ENV_TO_POS  */ { -1.0f, 1.0f, 0.65f },
    /* P_WT_ENV_TO_WARP */ { -1.0f, 1.0f, 0.0f },
    /* P_FILTER_ENABLE  */ { 0.0f, 1.0f, 1.0f },
    /* P_FILTER_TYPE    */ { 0.0f, 2.0f, 0.0f },
    /* P_CUTOFF         */ { 0.0f, 1.0f, 0.85f },
    /* P_RESONANCE      */ { 0.0f, 1.0f, 0.15f },
    /* P_AENV_ATTACK    */ { 0.001f, 5.0f, 0.01f },
    /* P_AENV_DECAY     */ { 0.005f, 5.0f, 0.35f },
    /* P_AENV_SUSTAIN   */ { 0.0f, 1.0f, 0.8f },
    /* P_AENV_RELEASE   */ { 0.005f, 8.0f, 0.35f },
    /* P_TUNE           */ { -24.0f, 24.0f, 0.0f },
    /* P_FINETUNE       */ { -100.0f, 100.0f, 0.0f },
    /* P_VOLUME         */ { 0.0f, 1.5f, 0.85f },
};

int WavetableDSP::windowSizeFromIndex(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 6) idx = 6;
    return kWindowSizes[idx];
}

int WavetableDSP::indexFromWindowSize(int size)
{
    for (int i = 0; i < 7; ++i)
    {
        if (kWindowSizes[i] == size)
            return i;
    }
    return 6;
}

WavetableDSP::WavetableDSP()
{
    init(44100.0f);
}

void WavetableDSP::init(float sampleRate)
{
    sr = (sampleRate > 8000.0f) ? sampleRate : 44100.0f;
    for (int i = 0; i < P_COUNT; ++i)
        params[i] = kParamInfo[i].defV;

    generateBuiltinTables();
    generateBuiltinEnvelopes();
    reset();
}

void WavetableDSP::reset()
{
    for (auto& v : voices)
    {
        v = Voice();
    }
    noteCounter = 0;
}

void WavetableDSP::generateBuiltinTables()
{
    const int W = kBuiltinWindowSize;
    const int F = kBuiltinWindowCount;

    for (int t = 0; t < kBuiltinTableCount; ++t)
    {
        builtinTables[t].assign((size_t)W * (size_t)F, 0);
    }

    auto normalizeFrame = [W](std::vector<float>& buf) {
        float peak = 1e-6f;
        for (int i = 0; i < W; ++i)
            peak = std::max(peak, std::fabs(buf[i]));
        const float scale = 30000.0f / peak;
        return scale;
    };

    std::vector<float> frame(W);

    // Table 0: Basic Shapes (Sine -> Triangle -> Saw -> Narrow Pulse)
    for (int f = 0; f < F; ++f)
    {
        const float u = (float)f / (float)(F - 1);
        std::fill(frame.begin(), frame.end(), 0.0f);

        for (int i = 0; i < W; ++i)
        {
            const float ph = (float)i / (float)W;
            const float sine = std::sin(kTwoPi * ph);

            // Bandlimited triangle (harmonics up to 25)
            float tri = 0.0f;
            for (int h = 1; h <= 25; h += 2)
            {
                float sign = (((h - 1) / 2) & 1) ? -1.0f : 1.0f;
                tri += sign * std::sin(kTwoPi * h * ph) / (float)(h * h);
            }
            tri *= 8.0f / (kPi * kPi);

            // Bandlimited saw (harmonics up to 32)
            float saw = 0.0f;
            for (int h = 1; h <= 32; ++h)
            {
                float gibbs = std::cos((float)(h - 1) / 32.0f * (kPi * 0.45f));
                saw += gibbs * std::sin(kTwoPi * h * ph) / (float)h;
            }
            saw *= 2.0f / kPi;

            // Bandlimited pulse with varying width
            const float pw = 0.5f - 0.35f * std::max(0.0f, (u - 0.666f) * 3.0f);
            float pulse = 0.0f;
            for (int h = 1; h <= 32; ++h)
            {
                float gibbs = std::cos((float)(h - 1) / 32.0f * (kPi * 0.45f));
                pulse += gibbs * (std::sin(kTwoPi * h * ph) - std::sin(kTwoPi * h * (ph - pw))) /
                         (float)h;
            }
            pulse *= 2.0f / kPi;

            float y = 0.0f;
            if (u < 0.333f)
            {
                float a = u * 3.0f;
                y = sine * (1.0f - a) + tri * a;
            }
            else if (u < 0.666f)
            {
                float a = (u - 0.333f) * 3.0f;
                y = tri * (1.0f - a) + saw * a;
            }
            else
            {
                float a = (u - 0.666f) * 3.0f;
                y = saw * (1.0f - a) + pulse * a;
            }
            frame[i] = y;
        }

        const float sc = normalizeFrame(frame);
        int16_t* dst = &builtinTables[0][(size_t)f * (size_t)W];
        for (int i = 0; i < W; ++i)
            dst[i] = (int16_t)clampf(frame[i] * sc, -32767.0f, 32767.0f);
    }

    // Table 1: Vocal Formant (smooth harmonic formant sweep)
    for (int f = 0; f < F; ++f)
    {
        const float u = (float)f / (float)(F - 1);
        const float f1 = 3.0f + u * 6.0f;
        const float f2 = 9.0f + std::sin(u * kPi) * 10.0f;
        std::fill(frame.begin(), frame.end(), 0.0f);

        for (int h = 1; h <= 28; ++h)
        {
            const float d1 = ((float)h - f1) / 2.2f;
            const float d2 = ((float)h - f2) / 3.0f;
            const float amp = (1.0f / (1.0f + 0.15f * h)) *
                              (0.35f + 0.9f * std::exp(-d1 * d1) + 0.65f * std::exp(-d2 * d2));
            for (int i = 0; i < W; ++i)
            {
                const float ph = (float)i / (float)W;
                frame[i] += amp * std::sin(kTwoPi * h * ph);
            }
        }
        const float sc = normalizeFrame(frame);
        int16_t* dst = &builtinTables[1][(size_t)f * (size_t)W];
        for (int i = 0; i < W; ++i)
            dst[i] = (int16_t)clampf(frame[i] * sc, -32767.0f, 32767.0f);
    }

    // Table 2: FM Metallic (2-op FM index sweep)
    for (int f = 0; f < F; ++f)
    {
        const float u = (float)f / (float)(F - 1);
        const float index = 0.1f + u * 4.5f;
        for (int i = 0; i < W; ++i)
        {
            const float ph = (float)i / (float)W;
            const float mod = std::sin(kTwoPi * 2.0f * ph) + 0.35f * u * std::sin(kTwoPi * 3.0f * ph);
            frame[i] = std::sin(kTwoPi * ph + index * mod);
        }
        const float sc = normalizeFrame(frame);
        int16_t* dst = &builtinTables[2][(size_t)f * (size_t)W];
        for (int i = 0; i < W; ++i)
            dst[i] = (int16_t)clampf(frame[i] * sc, -32767.0f, 32767.0f);
    }

    // Table 3: Spectral Organ (additive odd/octave drawbar sweep)
    for (int f = 0; f < F; ++f)
    {
        const float u = (float)f / (float)(F - 1);
        static constexpr int kHarm[8] = { 1, 2, 3, 4, 6, 8, 12, 16 };
        std::fill(frame.begin(), frame.end(), 0.0f);
        for (int idx = 0; idx < 8; ++idx)
        {
            const float center = (float)idx / 7.0f;
            const float d = (u - center) / 0.28f;
            const float amp = (idx == 0 ? 0.65f : 0.2f) + 0.85f * std::exp(-d * d);
            const int h = kHarm[idx];
            for (int i = 0; i < W; ++i)
            {
                const float ph = (float)i / (float)W;
                frame[i] += amp * std::sin(kTwoPi * h * ph);
            }
        }
        const float sc = normalizeFrame(frame);
        int16_t* dst = &builtinTables[3][(size_t)f * (size_t)W];
        for (int i = 0; i < W; ++i)
            dst[i] = (int16_t)clampf(frame[i] * sc, -32767.0f, 32767.0f);
    }
}

void WavetableDSP::generateBuiltinEnvelopes()
{
    for (int shape = 0; shape < kEnvCount; ++shape)
    {
        const int bank = shape / 16;
        const int sub = shape % 16;

        for (int i = 0; i < kEnvLength; ++i)
        {
            const float t = (float)i / (float)(kEnvLength - 1);
            float y = 0.0f;

            switch (bank)
            {
                case 0: // Exponential & Logarithmic Decays
                    y = std::pow(1.0f - t, 0.5f + sub * 0.45f);
                    break;
                case 1: // Swells, S-Curves & Smooth Bells
                    if (sub < 8)
                    {
                        y = std::pow(std::sin(t * kPi), 0.5f + sub * 0.5f);
                    }
                    else
                    {
                        float center = 0.1f + (sub - 8) * 0.1f;
                        float diff = (t - center) / 0.22f;
                        y = std::exp(-0.5f * diff * diff);
                    }
                    break;
                case 2: // Double & Multi-Bounces
                {
                    int bounces = 2 + (sub % 5);
                    float scaledT = t * bounces;
                    int bIdx = std::min((int)scaledT, bounces - 1);
                    float localT = scaledT - bIdx;
                    y = (4.0f * localT * (1.0f - localT)) * std::pow(0.55f, (float)bIdx);
                    break;
                }
                case 3: // Rhythmic Gates & Steppers
                {
                    static const uint8_t pulseCounts[16] = {
                        2, 2, 3, 3, 4, 4, 6, 6, 8, 8, 12, 12, 16, 16, 8, 16
                    };
                    int pulses = pulseCounts[sub];
                    float duty = (sub % 2 == 0) ? 0.5f : 0.25f;
                    float cycle = t * pulses;
                    float frac = cycle - std::floor(cycle);
                    y = (frac < duty) ? 1.0f : 0.0f;
                    break;
                }
                case 4: // Sharks, Fins & Asymmetric Triangles
                {
                    float peak = 0.05f + sub * 0.06f;
                    y = (t < peak) ? (t / peak) : (1.0f - (t - peak) / (1.0f - peak));
                    break;
                }
                case 5: // Ripples & Damped Oscillations
                    y = std::exp(-3.5f * t) * (0.5f + 0.5f * std::cos(kTwoPi * (2.0f + sub) * t));
                    break;
                case 6: // Accelerating & Decelerating Trills
                {
                    float chirp = (sub < 8) ? (t * t) : std::sqrt(t);
                    float modT = chirp * (3 + (sub % 8));
                    float frac = modT - std::floor(modT);
                    y = (1.0f - t * 0.6f) * ((frac < 0.4f) ? 1.0f : 0.0f);
                    break;
                }
                case 7: // Staircases
                default:
                {
                    int steps = 2 + sub;
                    int curStep = std::min((int)(t * steps), steps - 1);
                    y = (sub < 8) ? ((float)curStep / (float)(steps - 1))
                                  : (1.0f - (float)curStep / (float)(steps - 1));
                    break;
                }
            }

            envBank[shape][i] = (int16_t)(clampf(y, 0.0f, 1.0f) * 32767.0f);
        }
    }
}

float WavetableDSP::evalWtEnv(float phase01, uint8_t shapeIdx, int8_t wx, int8_t wy) const
{
    if (shapeIdx >= kEnvCount) shapeIdx = kEnvCount - 1;
    float p = clampf(phase01, 0.0f, 1.0f);

    if (wx != 0)
    {
        float k = (float)wx * (1.0f / 9.0f);
        float denom = p * 2.0f * k + 1.0f - k;
        if (denom != 0.0f)
            p = (p * (1.0f + k)) / denom;
        p = clampf(p, 0.0f, 1.0f);
    }

    float idxF = p * (float)(kEnvLength - 1);
    int idx = std::clamp((int)idxF, 0, kEnvLength - 2);
    float frac = idxF - (float)idx;

    const int16_t* tbl = envBank[shapeIdx];
    float y = ((float)tbl[idx] + frac * (float)(tbl[idx + 1] - tbl[idx])) * (1.0f / 32767.0f);

    if (wy == -7)
    {
        y = 1.0f - y;
    }
    else if (wy != 0)
    {
        float ky = (float)wy * (1.0f / 9.0f);
        float denom = y * 2.0f * ky + 1.0f - ky;
        if (denom != 0.0f)
            y = (y * (1.0f + ky)) / denom;
    }

    return clampf(y, 0.0f, 1.0f);
}

float WavetableDSP::stepVoiceWtEnv(Voice& v, float dtSec, bool gateHeld)
{
    const int mode = (int)std::round(params[P_WT_ENV_MODE]);
    if (mode <= 0)
    {
        v.wtEnv.output = 0.0f;
        v.liveEnvPhase01 = 0.0f;
        return 0.0f;
    }

    const uint8_t shape = (uint8_t)std::clamp((int)std::round(params[P_WT_ENV_SHAPE]), 0, kEnvCount - 1);
    const int8_t wx = (int8_t)std::clamp((int)std::round(params[P_WT_ENV_WARP_X]), -7, 7);
    const int8_t wy = (int8_t)std::clamp((int)std::round(params[P_WT_ENV_WARP_Y]), -7, 7);
    const float dur = std::max(params[P_WT_ENV_SPEED], 0.05f);
    const float sustFrac = clampf(params[P_WT_ENV_SUST] * 0.01f, 0.0f, 0.99f);

    if (mode == 2) // WT LFO Loop (bipolar -1..+1 matching Tracker mtEnvelopeGenerator)
    {
        v.wtEnv.elapsedSec += dtSec;
        while (v.wtEnv.elapsedSec >= dur)
            v.wtEnv.elapsedSec -= dur;
        float p = v.wtEnv.elapsedSec / dur;
        v.liveEnvPhase01 = p;
        float y = evalWtEnv(p, shape, wx, wy);
        v.wtEnv.output = 2.0f * y - 1.0f;
        return v.wtEnv.output;
    }

    // Mode 1: WT Env One-Shot (unipolar 0..1)
    if (v.wtEnv.phase == WtVoiceEnv::ENV_ACTIVE)
    {
        v.wtEnv.elapsedSec += dtSec;
        if (!gateHeld && sustFrac > 0.0f)
        {
            v.wtEnv.release(dur);
        }
        else
        {
            float p = v.wtEnv.elapsedSec / dur;
            if (sustFrac > 0.0f && p >= sustFrac)
            {
                p = sustFrac;
                v.wtEnv.phase = WtVoiceEnv::ENV_SUSTAIN;
            }
            else if (p >= 1.0f)
            {
                p = 1.0f;
                v.wtEnv.phase = WtVoiceEnv::ENV_IDLE;
            }
            v.liveEnvPhase01 = p;
            v.wtEnv.output = evalWtEnv(p, shape, wx, wy);
            return v.wtEnv.output;
        }
    }

    if (v.wtEnv.phase == WtVoiceEnv::ENV_SUSTAIN)
    {
        if (!gateHeld)
        {
            v.wtEnv.releaseStartPhase = sustFrac;
            v.wtEnv.phase = WtVoiceEnv::ENV_RELEASE;
            v.wtEnv.elapsedSec = 0.0f;
        }
        else
        {
            v.liveEnvPhase01 = sustFrac;
            v.wtEnv.output = evalWtEnv(sustFrac, shape, wx, wy);
            return v.wtEnv.output;
        }
    }

    if (v.wtEnv.phase == WtVoiceEnv::ENV_RELEASE)
    {
        const float relDur = std::max(params[P_AENV_RELEASE], 0.05f);
        v.wtEnv.elapsedSec += dtSec;
        float relP = v.wtEnv.elapsedSec / relDur;
        if (relP >= 1.0f)
        {
            v.wtEnv.phase = WtVoiceEnv::ENV_IDLE;
            v.wtEnv.output = 0.0f;
            v.liveEnvPhase01 = 1.0f;
        }
        else
        {
            float p = v.wtEnv.releaseStartPhase + relP * (1.0f - v.wtEnv.releaseStartPhase);
            v.liveEnvPhase01 = p;
            v.wtEnv.output = evalWtEnv(p, shape, wx, wy) * (1.0f - relP);
        }
        return v.wtEnv.output;
    }

    return v.wtEnv.output;
}

const int16_t* WavetableDSP::activeTablePtr(uint32_t& outSamples,
                                            uint32_t& outWinSize,
                                            uint32_t& outWinCount) const
{
    int tblIdx = std::clamp((int)std::round(params[P_TABLE]), 0, kBuiltinTableCount);
    const int16_t* ptr = nullptr;
    uint32_t total = 0;

    if (tblIdx == kBuiltinTableCount && !customTable.empty())
    {
        ptr = customTable.data();
        total = (uint32_t)customTable.size();
    }
    else
    {
        if (tblIdx >= kBuiltinTableCount) tblIdx = 0;
        ptr = builtinTables[tblIdx].data();
        total = (uint32_t)builtinTables[tblIdx].size();
    }

    uint32_t wSize = (uint32_t)windowSizeFromIndex((int)std::round(params[P_WINDOW_SIZE]));
    if (wSize > total && total >= 32U)
    {
        wSize = 32U;
        while (wSize * 2U <= total && wSize < 2048U)
            wSize *= 2U;
    }
    if (wSize < 32U) wSize = 32U;

    uint32_t wCount = total / wSize;
    if (wCount < 1U) wCount = 1U;

    outSamples = total;
    outWinSize = wSize;
    outWinCount = wCount;
    return ptr;
}

bool WavetableDSP::loadPcm16(const int16_t* samples, int numSamples, int windowSize)
{
    if (!samples || numSamples < 32)
        return false;

    int winIdx = indexFromWindowSize(windowSize);
    int wSize = windowSizeFromIndex(winIdx);
    if (numSamples < wSize)
    {
        wSize = 32;
        while (wSize * 2 <= numSamples && wSize < 2048)
            wSize *= 2;
        winIdx = indexFromWindowSize(wSize);
    }

    int usable = (numSamples / wSize) * wSize;
    if (usable < wSize)
        return false;

    // Limit to 256 * 2048 = 524,288 samples (~1 MB) so WASM heap stays lean
    constexpr int kMaxSamples = 256 * 2048;
    if (usable > kMaxSamples)
        usable = (kMaxSamples / wSize) * wSize;

    customTable.assign(samples, samples + usable);
    customWinSize = wSize;
    params[P_WINDOW_SIZE] = (float)winIdx;
    params[P_TABLE] = (float)kBuiltinTableCount;

    for (auto& v : voices)
        v.core.prevWaveTablePosition = UINT32_MAX;

    return true;
}

int WavetableDSP::getWindowSize() const
{
    uint32_t total = 0, wSize = 2048, wCount = 1;
    activeTablePtr(total, wSize, wCount);
    return (int)wSize;
}

int WavetableDSP::getWindowCount() const
{
    uint32_t total = 0, wSize = 2048, wCount = 1;
    activeTablePtr(total, wSize, wCount);
    return (int)wCount;
}

int WavetableDSP::getTotalTableSamples() const
{
    uint32_t total = 0, wSize = 2048, wCount = 1;
    activeTablePtr(total, wSize, wCount);
    return (int)total;
}

void WavetableDSP::noteOn(int note, float velocity)
{
    if (note < 0 || note > 127) return;
    velocity = clampf(velocity, 0.05f, 1.0f);

    int slot = -1;
    for (int i = 0; i < kVoices; ++i)
    {
        if (voices[i].active && voices[i].note == note)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        for (int i = 0; i < kVoices; ++i)
        {
            if (!voices[i].active)
            {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0)
    {
        uint32_t oldestAge = UINT32_MAX;
        for (int i = 0; i < kVoices; ++i)
        {
            if (!voices[i].gate && voices[i].age < oldestAge)
            {
                oldestAge = voices[i].age;
                slot = i;
            }
        }
        if (slot < 0)
        {
            for (int i = 0; i < kVoices; ++i)
            {
                if (voices[i].age < oldestAge)
                {
                    oldestAge = voices[i].age;
                    slot = i;
                }
            }
        }
    }

    Voice& v = voices[slot];
    const bool retrigger = (v.active && v.note == note);
    if (!retrigger)
    {
        v.core = WtCoreVoiceState();
        v.ic1eq = 0.0f;
        v.ic2eq = 0.0f;
    }
    v.core.onsetFadeSamples = 32;
    v.core.smoothedWarpQ8 = (uint16_t)(clampf(params[P_WARP_AMOUNT], 0.0f, 100.0f) * 256.0f);
    v.gate = true;
    v.active = true;
    v.note = note;
    v.velocity = velocity;
    v.age = ++noteCounter;
    v.aenv.gateOn();
    v.wtEnv.trigger();
    v.livePosNorm = clampf(params[P_POSITION], 0.0f, 1.0f);
    v.liveWarpPct = clampf(params[P_WARP_AMOUNT], 0.0f, 100.0f);
    v.liveEnvPhase01 = 0.0f;
}

void WavetableDSP::noteOff(int note)
{
    const float dur = std::max(params[P_WT_ENV_SPEED], 0.05f);
    for (auto& v : voices)
    {
        if (v.active && v.gate && v.note == note)
        {
            v.gate = false;
            v.aenv.gateOff();
            if ((int)std::round(params[P_WT_ENV_MODE]) == 1)
                v.wtEnv.release(dur);
        }
    }
}

void WavetableDSP::allNotesOff()
{
    const float dur = std::max(params[P_WT_ENV_SPEED], 0.05f);
    for (auto& v : voices)
    {
        if (v.active && v.gate)
        {
            v.gate = false;
            v.aenv.gateOff();
            if ((int)std::round(params[P_WT_ENV_MODE]) == 1)
                v.wtEnv.release(dur);
        }
    }
}

void WavetableDSP::setParam(int id, float value)
{
    if (id < 0 || id >= P_COUNT) return;
    params[id] = clampf(value, kParamInfo[id].minV, kParamInfo[id].maxV);
}

float WavetableDSP::getParam(int id) const
{
    if (id < 0 || id >= P_COUNT) return 0.0f;
    return params[id];
}

int WavetableDSP::activeVoices() const
{
    int count = 0;
    for (const auto& v : voices)
        if (v.active) count++;
    return count;
}

void WavetableDSP::processBlock(float* outL, float* outR, int numSamples)
{
    if (!outL || !outR || numSamples <= 0) return;

    uint32_t totalSamples = 0, wSize = 2048, wCount = 1;
    const int16_t* tablePtr = activeTablePtr(totalSamples, wSize, wCount);
    const uint32_t wSize16 = wSize << 16;
    const bool wIsPow2 = (wSize & (wSize - 1U)) == 0U;
    uint8_t wShift = 0;
    if (wIsPow2)
    {
        while ((1U << wShift) < wSize && wShift < 16U)
            wShift++;
    }
    const uint32_t wMask16 = wSize16 - 1U;
    const uint32_t maxlength = (totalSamples >= wSize) ? (totalSamples - wSize) : 0U;

    const uint8_t warpMode = (uint8_t)std::clamp((int)std::round(params[P_WARP_MODE]), 0, 3);
    const uint8_t subWave = (uint8_t)std::clamp((int)std::round(params[P_SUB_WAVE]), 0, 3);
    const uint8_t subLevel = (uint8_t)std::clamp((int)std::round(params[P_SUB_LEVEL]), 0, 100);
    const int32_t subGain14 = ((int32_t)subLevel * 16384) / 100;
    const float subPhaseScale = 2147483648.0f / (float)wSize;

    const bool filterEn = params[P_FILTER_ENABLE] >= 0.5f;
    const int filterType = std::clamp((int)std::round(params[P_FILTER_TYPE]), 0, 2);
    const float fcHz = clampf(25.0f * std::pow(720.0f, params[P_CUTOFF]), 25.0f, sr * 0.44f);
    const float g = std::tan(kPi * fcHz / sr);
    const float kRes = 1.92f - clampf(params[P_RESONANCE], 0.0f, 1.0f) * 1.80f;
    const float a1 = 1.0f / (1.0f + g * (g + kRes));
    const float a2 = g * a1;
    const float a3 = g * a2;

    const float aCoef = timeCoef(params[P_AENV_ATTACK], 1.5f, sr);
    const float dCoef = timeCoef(params[P_AENV_DECAY], 6.9f, sr);
    const float sustain = params[P_AENV_SUSTAIN];
    const float rCoef = timeCoef(params[P_AENV_RELEASE], 6.9f, sr);
    const float masterVol = params[P_VOLUME] * 0.45f;

    int offset = 0;
    while (offset < numSamples)
    {
        const int chunk = std::min(kChunkSize, numSamples - offset);
        std::fill(outL + offset, outL + offset + chunk, 0.0f);
        std::fill(outR + offset, outR + offset + chunk, 0.0f);

        const float chunkDt = (float)chunk / sr;

        for (int vi = 0; vi < kVoices; ++vi)
        {
            Voice& v = voices[vi];
            if (!v.active) continue;

            const float envOut = stepVoiceWtEnv(v, chunkDt, v.gate);

            // Position modulation
            const float posNorm =
              clampf(params[P_POSITION] + envOut * params[P_WT_ENV_TO_POS], 0.0f, 1.0f);
            const uint32_t curWindow =
              (wCount > 1U) ? (uint32_t)std::round(posNorm * (float)(wCount - 1U)) : 0U;
            uint32_t waveTablePosition = curWindow * wSize;
            if (waveTablePosition > maxlength) waveTablePosition = maxlength;

            const int16_t* in = tablePtr + waveTablePosition;
            const bool doWindowXfade =
              (v.core.prevWaveTablePosition != UINT32_MAX &&
               v.core.prevWaveTablePosition != waveTablePosition &&
               v.core.prevWaveTablePosition <= maxlength);
            const int16_t* inPrev =
              doWindowXfade ? (tablePtr + v.core.prevWaveTablePosition) : nullptr;

            // Warp modulation
            const float warpModPct = clampf(
              params[P_WARP_AMOUNT] + envOut * params[P_WT_ENV_TO_WARP] * 100.0f, 0.0f, 100.0f);
            v.livePosNorm = posNorm;
            v.liveWarpPct = warpModPct;
            const int32_t targetWarpQ8 =
              (warpMode == 0U) ? 0 : (int32_t)(warpModPct * 256.0f + 0.5f);
            int32_t curWarpQ8 = (int32_t)v.core.smoothedWarpQ8;
            const int32_t warpStepQ8 = (targetWarpQ8 - curWarpQ8) / chunk;

            // Pitch step
            const float semi = (float)v.note + params[P_TUNE] + params[P_FINETUNE] * 0.01f;
            const float freq = 440.0f * std::pow(2.0f, (semi - 69.0f) / 12.0f);
            const float pitchControl = clampf(freq * (float)wSize / sr, 0.001f, (float)wSize * 0.45f);
            const int32_t castPitch = (int32_t)pitchControl;
            const int32_t fracPitchInc = (int32_t)((pitchControl - (float)castPitch) * 65536.0f);
            const uint32_t subPhaseInc = (uint32_t)(pitchControl * subPhaseScale);

            const float pan = 0.5f + (((float)vi - 3.5f) / 7.0f) * 0.14f; // subtle poly spread
            const float gainL = std::cos(pan * (kPi * 0.5f));
            const float gainR = std::sin(pan * (kPi * 0.5f));

            for (int i = 0; i < chunk; ++i)
            {
                if (v.core.iPitchCounter >= (int32_t)wSize)
                    v.core.iPitchCounter %= (int32_t)wSize;

                int32_t wtSample = wtReadWarpedSample(in,
                                                      inPrev,
                                                      i,
                                                      doWindowXfade,
                                                      (uint32_t)v.core.iPitchCounter,
                                                      v.core.fracPitchCounter,
                                                      wSize,
                                                      wSize16,
                                                      wIsPow2,
                                                      wShift,
                                                      wMask16,
                                                      warpMode,
                                                      curWarpQ8,
                                                      v.core.foldPrevRaw,
                                                      v.core.foldLpState);

                if (subWave != 0U && subLevel > 0U)
                {
                    const int32_t subSample = wtEvalSubOsc(subWave, v.core.subPhase32);
                    wtSample = wtClamp16(wtSample + ((subSample * subGain14) >> 14));
                    v.core.subPhase32 += subPhaseInc;
                }

                if (v.core.onsetFadeSamples > 0)
                {
                    const int32_t fadeMult = 32 - v.core.onsetFadeSamples;
                    wtSample = (wtSample * fadeMult) >> 5;
                    v.core.onsetFadeSamples--;
                }

                float sig = (float)wtSample * (1.0f / 32768.0f);

                if (filterEn)
                {
                    const float v3 = sig - v.ic2eq;
                    const float v1 = a1 * v.ic1eq + a2 * v3;
                    const float v2 = v.ic2eq + a2 * v.ic1eq + a3 * v3;
                    v.ic1eq = 2.0f * v1 - v.ic1eq;
                    v.ic2eq = 2.0f * v2 - v.ic2eq;

                    if (filterType == 0) sig = v2;
                    else if (filterType == 1) sig = sig - kRes * v1 - v2;
                    else sig = v1;
                }

                const float amp = v.aenv.process(aCoef, dCoef, sustain, rCoef) * v.velocity;
                const float outV = sig * amp;

                outL[offset + i] += outV * gainL;
                outR[offset + i] += outV * gainR;

                curWarpQ8 += warpStepQ8;
                v.core.iPitchCounter += castPitch;
                v.core.fracPitchCounter += fracPitchInc;
                if (v.core.fracPitchCounter >= 65536)
                {
                    v.core.fracPitchCounter -= 65536;
                    v.core.iPitchCounter++;
                }
                if (v.core.iPitchCounter >= (int32_t)wSize)
                    v.core.iPitchCounter -= (int32_t)wSize;
            }

            v.core.smoothedWarpQ8 = (uint16_t)targetWarpQ8;
            v.core.prevWaveTablePosition = waveTablePosition;

            if (v.aenv.stage == AmpAdsr::IDLE)
                v.active = false;
        }

        for (int i = 0; i < chunk; ++i)
        {
            outL[offset + i] = softSat(outL[offset + i] * masterVol);
            outR[offset + i] = softSat(outR[offset + i] * masterVol);
        }

        offset += chunk;
    }
}

int WavetableDSP::getModState(float* out3) const
{
    int bestIdx = -1;
    uint32_t bestAge = 0;
    for (int i = 0; i < kVoices; ++i)
    {
        if (voices[i].active && (bestIdx < 0 || voices[i].age >= bestAge))
        {
            bestIdx = i;
            bestAge = voices[i].age;
        }
    }
    if (bestIdx >= 0)
    {
        if (out3)
        {
            out3[0] = voices[bestIdx].livePosNorm;
            out3[1] = voices[bestIdx].liveWarpPct;
            out3[2] = voices[bestIdx].liveEnvPhase01;
        }
        return 1;
    }
    if (out3)
    {
        out3[0] = clampf(params[P_POSITION], 0.0f, 1.0f);
        out3[1] = clampf(params[P_WARP_AMOUNT], 0.0f, 100.0f);
        out3[2] = 0.0f;
    }
    return 0;
}

void WavetableDSP::getDisplayWave(float* outBuf, int numPoints) const
{
    if (!outBuf || numPoints <= 0) return;

    float mod[3] = { 0.0f, 0.0f, 0.0f };
    getModState(mod);

    uint32_t totalSamples = 0, wSize = 2048, wCount = 1;
    const int16_t* tablePtr = activeTablePtr(totalSamples, wSize, wCount);
    const uint32_t wSize16 = wSize << 16;
    const bool wIsPow2 = (wSize & (wSize - 1U)) == 0U;
    uint8_t wShift = 0;
    if (wIsPow2)
    {
        while ((1U << wShift) < wSize && wShift < 16U)
            wShift++;
    }
    const uint32_t wMask16 = wSize16 - 1U;
    const uint32_t maxlength = (totalSamples >= wSize) ? (totalSamples - wSize) : 0U;

    const uint32_t curWin =
      (wCount > 1U) ? (uint32_t)std::round(clampf(mod[0], 0.0f, 1.0f) * (float)(wCount - 1U))
                    : 0U;
    uint32_t pos = curWin * wSize;
    if (pos > maxlength) pos = maxlength;
    const int16_t* in = tablePtr + pos;

    const uint8_t warpMode = (uint8_t)std::clamp((int)std::round(params[P_WARP_MODE]), 0, 3);
    const int32_t warpQ8 = (warpMode == 0U) ? 0 : (int32_t)(clampf(mod[1], 0.0f, 100.0f) * 256.0f);
    const uint8_t subWave = (uint8_t)std::clamp((int)std::round(params[P_SUB_WAVE]), 0, 3);
    const uint8_t subLevel = (uint8_t)std::clamp((int)std::round(params[P_SUB_LEVEL]), 0, 100);
    const int32_t subGain14 = ((int32_t)subLevel * 16384) / 100;

    int32_t foldPrev = in[0];
    int32_t foldLp = in[0];

    for (int i = 0; i < numPoints; ++i)
    {
        const double phase01 = (double)i / (double)numPoints;
        const double samplePos = phase01 * (double)wSize;
        const uint32_t idx = (uint32_t)samplePos;
        const int32_t frac = (int32_t)((samplePos - (double)idx) * 65536.0);

        int32_t wtSample = wtReadWarpedSample(in,
                                              nullptr,
                                              i,
                                              false,
                                              idx,
                                              frac,
                                              wSize,
                                              wSize16,
                                              wIsPow2,
                                              wShift,
                                              wMask16,
                                              warpMode,
                                              warpQ8,
                                              foldPrev,
                                              foldLp);

        if (subWave != 0U && subLevel > 0U)
        {
            const uint32_t subPhase32 = (uint32_t)(phase01 * 0.5 * 4294967296.0);
            const int32_t subSample = wtEvalSubOsc(subWave, subPhase32);
            wtSample = wtClamp16(wtSample + ((subSample * subGain14) >> 14));
        }

        outBuf[i] = (float)wtSample * (1.0f / 32768.0f);
    }
}

void WavetableDSP::getEnvPreview(float* outBuf, int numPoints) const
{
    if (!outBuf || numPoints <= 0) return;
    const uint8_t shape = (uint8_t)std::clamp((int)std::round(params[P_WT_ENV_SHAPE]), 0, kEnvCount - 1);
    const int8_t wx = (int8_t)std::clamp((int)std::round(params[P_WT_ENV_WARP_X]), -7, 7);
    const int8_t wy = (int8_t)std::clamp((int)std::round(params[P_WT_ENV_WARP_Y]), -7, 7);

    for (int i = 0; i < numPoints; ++i)
    {
        const float p = (numPoints > 1) ? ((float)i / (float)(numPoints - 1)) : 0.0f;
        outBuf[i] = evalWtEnv(p, shape, wx, wy);
    }
}

int WavetableDSP::getExportPtiSize() const
{
    uint32_t totalSamples = 0, wSize = 2048, wCount = 1;
    activeTablePtr(totalSamples, wSize, wCount);
    const uint32_t usedSamples = wSize * wCount;
    return (int)(sizeof(PtiFile) + usedSamples * sizeof(int16_t));
}

int WavetableDSP::exportPti(uint8_t* outBuf, int maxLen, const char* name) const
{
    if (!outBuf) return 0;

    uint32_t totalSamples = 0, wSize = 2048, wCount = 1;
    const int16_t* tablePtr = activeTablePtr(totalSamples, wSize, wCount);
    const uint32_t usedSamples = wSize * wCount;
    const int neededBytes = (int)(sizeof(PtiFile) + usedSamples * sizeof(int16_t));
    if (maxLen < neededBytes) return 0;

    PtiFile pti;
    std::memset(&pti, 0, sizeof(pti));

    pti.instrumentDataAndHeader.instrHeader.id_file[0] = 'T';
    pti.instrumentDataAndHeader.instrHeader.id_file[1] = 'I';
    pti.instrumentDataAndHeader.instrHeader.type = 1; // fileTypeInstrument
    pti.instrumentDataAndHeader.instrHeader.fwVersion[0] = 1;
    pti.instrumentDataAndHeader.instrHeader.fwVersion[1] = 8;
    pti.instrumentDataAndHeader.instrHeader.fwVersion[2] = 0;
    pti.instrumentDataAndHeader.instrHeader.fwVersion[3] = 1;
    for (int i = 0; i < 4; ++i)
        pti.instrumentDataAndHeader.instrHeader.fileStructureVersion[i] = 9; // INSTRUMENT_FILE_VERSION
    pti.instrumentDataAndHeader.instrHeader.size = sizeof(PtiInstrument);

    PtiInstrument& inst = pti.instrumentDataAndHeader.instrument;
    inst.isActive = 1;
    inst.sample.type = 1; // mtSampleTypeWavetable
    if (name && name[0] != '\0')
    {
        std::snprintf(inst.sample.file_name, sizeof(inst.sample.file_name), "%s", name);
    }
    else
    {
        std::snprintf(inst.sample.file_name, sizeof(inst.sample.file_name), "WTPatch");
    }
    inst.sample.address = 0;
    inst.sample.length = usedSamples;
    inst.sample.wavetable_window_size = (uint16_t)wSize;
    inst.sample.wavetableWindowNumber = wCount;

    inst.playMode = 6; // playModeWavetable
    inst.startPoint = 0;
    inst.loopPoint1 = 1;
    inst.loopPoint2 = 65534;
    inst.endPoint = 65535;
    inst.wavetableCurrentWindow =
      (wCount > 1U)
        ? (uint16_t)std::round(clampf(params[P_POSITION], 0.0f, 1.0f) * (float)(wCount - 1U))
        : 0U;

    const uint8_t subW = (uint8_t)std::clamp((int)std::round(params[P_SUB_WAVE]), 0, 3);
    const uint8_t warpM = (uint8_t)std::clamp((int)std::round(params[P_WARP_MODE]), 0, 3);
    inst.wtSubWarpByte = (uint8_t)((subW & 0x03U) | ((warpM & 0x03U) << 2));
    inst.wtSubLevel = (uint8_t)std::clamp((int)std::round(params[P_SUB_LEVEL]), 0, 100);
    inst.wtWarpAmount = (uint8_t)std::clamp((int)std::round(params[P_WARP_AMOUNT]), 0, 100);

    // Default initialize all 6 envelope & LFO slots matching defaultInstrumentParams
    for (int e = 0; e < 6; ++e)
    {
        inst.envelope[e].amount = 1.0f;
        inst.envelope[e].attack = (e == 0) ? 10 : 3000;
        inst.envelope[e].sustain = 1.0f;
        inst.envelope[e].release = 1000;
        inst.envelope[e].enable = (e == 0) ? 1 : 0;
        inst.lfo[e].shape = 2; // lfoShapeTriangle
        inst.lfo[e].amount = 0.5f;
    }

    // Slot 0: Amp ADSR
    inst.envelope[0].enable = 1;
    inst.envelope[0].loop = 0;
    inst.envelope[0].amount = 1.0f;
    inst.envelope[0].attack = (uint16_t)std::clamp((int)std::round(params[P_AENV_ATTACK] * 1000.0f), 0, 10000);
    inst.envelope[0].decay = (uint16_t)std::clamp((int)std::round(params[P_AENV_DECAY] * 1000.0f), 0, 10000);
    inst.envelope[0].sustain = clampf(params[P_AENV_SUSTAIN], 0.0f, 1.0f);
    inst.envelope[0].release = (uint16_t)std::clamp((int)std::round(params[P_AENV_RELEASE] * 1000.0f), 0, 10000);

    // Map WT Env speed (seconds) to Tracker ms and closest tempoSyncRatesOthers index at 120 BPM
    const uint16_t wtDurMs =
      (uint16_t)std::clamp((int)std::round(params[P_WT_ENV_SPEED] * 1000.0f), 50, 10000);
    const float targetBeats = params[P_WT_ENV_SPEED] / 0.125f; // 120 BPM => 125ms per 1/16 step
    uint8_t bestSpeedIdx = 14;
    float bestDiff = 1e9f;
    for (int s = 0; s < kSyncRatesOtherCount; ++s)
    {
        float diff = std::fabs(kTempoSyncRatesOthers[s] - targetBeats);
        if (diff < bestDiff)
        {
            bestDiff = diff;
            bestSpeedIdx = (uint8_t)s;
        }
    }

    const int wtMode = (int)std::round(params[P_WT_ENV_MODE]);
    const uint8_t wtLoop = (wtMode == 2) ? 3 : 2;
    const uint8_t wtShape = (uint8_t)std::clamp((int)std::round(params[P_WT_ENV_SHAPE]), 0, 127);
    const uint8_t wtBend = packWarpXY((int8_t)std::round(params[P_WT_ENV_WARP_X]),
                                      (int8_t)std::round(params[P_WT_ENV_WARP_Y]));
    const uint8_t wtSust = (uint8_t)std::clamp((int)std::round(params[P_WT_ENV_SUST]), 0, 99);

    // Slot 3: envWtPos
    if (wtMode > 0 && std::fabs(params[P_WT_ENV_TO_POS]) > 0.001f)
    {
        inst.envelope[3].enable = 0x01; // kEnvEnableWtPosBit
        inst.envelope[3].loop = wtLoop;
        inst.envelope[3].amount = params[P_WT_ENV_TO_POS];
        inst.envelope[3].attack = wtDurMs;
        inst.envelope[3].decay = wtDurMs;
        inst.envelope[3].release = inst.envelope[0].release;
        inst.lfo[3].shape = wtShape;
        inst.lfo[3].speed = bestSpeedIdx;
        inst.lfo[3].wtBend = wtBend;
        inst.lfo[3].wtSust = wtSust;
        inst.lfo[3].amount = params[P_WT_ENV_TO_POS];
    }

    // Slot 4: envWtWarp (shares slot 4 with envGranPos via bit 0x02)
    if (wtMode > 0 && std::fabs(params[P_WT_ENV_TO_WARP]) > 0.001f)
    {
        inst.envelope[4].enable = 0x02; // kEnvEnableWtWarpBit
        inst.envelope[4].loop = wtLoop;
        inst.envelope[4].amount = params[P_WT_ENV_TO_WARP];
        inst.envelope[4].attack = wtDurMs;
        inst.envelope[4].decay = wtDurMs;
        inst.envelope[4].release = inst.envelope[0].release;
        inst.lfo[4].shape = wtShape;
        inst.lfo[4].speed = bestSpeedIdx;
        inst.lfo[4].wtBend = wtBend;
        inst.lfo[4].wtSust = wtSust;
        inst.lfo[4].amount = params[P_WT_ENV_TO_WARP];
    }

    inst.cutOff = clampf(params[P_CUTOFF], 0.0f, 1.0f);
    inst.resonance = clampf(params[P_RESONANCE], 0.0f, 1.0f) * 4.3f; // RESONANCE_MAX = 4.3
    inst.filterType = (uint8_t)std::clamp((int)std::round(params[P_FILTER_TYPE]), 0, 2);
    inst.filterEnable = (params[P_FILTER_ENABLE] >= 0.5f) ? 1 : 0;
    inst.tune = (int8_t)std::clamp((int)std::round(params[P_TUNE]), -24, 24);
    inst.fineTune = (int8_t)std::clamp((int)std::round(params[P_FINETUNE]), -100, 100);
    inst.volume = 50;
    inst.panning = 50;
    inst.granular.grainLength = 441;
    inst.bitDepth = 16;

    pti.crc = crc32Ieee(reinterpret_cast<const uint8_t*>(&pti.instrumentDataAndHeader),
                        sizeof(pti.instrumentDataAndHeader));

    std::memcpy(outBuf, &pti, sizeof(PtiFile));
    std::memcpy(outBuf + sizeof(PtiFile), tablePtr, usedSamples * sizeof(int16_t));
    return neededBytes;
}

bool WavetableDSP::importPti(const uint8_t* data, int byteLen)
{
    if (!data || byteLen < (int)sizeof(PtiFile))
        return false;

    PtiFile pti;
    std::memcpy(&pti, data, sizeof(PtiFile));

    const PtiHeader& hdr = pti.instrumentDataAndHeader.instrHeader;
    if (hdr.id_file[0] != 'T' || hdr.id_file[1] != 'I' || hdr.type != 1 ||
        hdr.size != sizeof(PtiInstrument))
    {
        return false;
    }

    const int pcmBytes = byteLen - (int)sizeof(PtiFile);
    if (pcmBytes < 64)
        return false;

    const PtiInstrument& inst = pti.instrumentDataAndHeader.instrument;
    int winSize = (inst.sample.wavetable_window_size >= 32 &&
                   inst.sample.wavetable_window_size <= 2048)
                    ? (int)inst.sample.wavetable_window_size
                    : 2048;

    const int numSamples = pcmBytes / (int)sizeof(int16_t);
    std::vector<int16_t> pcm(numSamples);
    std::memcpy(pcm.data(), data + sizeof(PtiFile), (size_t)numSamples * sizeof(int16_t));

    if (!loadPcm16(pcm.data(), numSamples, winSize))
        return false;

    const int wCount = getWindowCount();
    if (wCount > 1)
    {
        setParam(P_POSITION,
                 clampf((float)inst.wavetableCurrentWindow / (float)(wCount - 1), 0.0f, 1.0f));
    }
    else
    {
        setParam(P_POSITION, 0.0f);
    }

    setParam(P_SUB_WAVE, (float)(inst.wtSubWarpByte & 0x03U));
    setParam(P_WARP_MODE, (float)((inst.wtSubWarpByte >> 2) & 0x03U));
    setParam(P_SUB_LEVEL, (float)inst.wtSubLevel);
    setParam(P_WARP_AMOUNT, (float)inst.wtWarpAmount);

    setParam(P_FILTER_ENABLE, inst.filterEnable ? 1.0f : 0.0f);
    setParam(P_FILTER_TYPE, (float)inst.filterType);
    setParam(P_CUTOFF, inst.cutOff);
    setParam(P_RESONANCE, inst.resonance / 4.3f);

    setParam(P_TUNE, (float)inst.tune);
    setParam(P_FINETUNE, (float)inst.fineTune);

    if (inst.envelope[0].enable)
    {
        setParam(P_AENV_ATTACK, (float)inst.envelope[0].attack * 0.001f);
        setParam(P_AENV_DECAY, (float)inst.envelope[0].decay * 0.001f);
        setParam(P_AENV_SUSTAIN, inst.envelope[0].sustain);
        setParam(P_AENV_RELEASE, (float)inst.envelope[0].release * 0.001f);
    }

    const bool posEn = (inst.envelope[3].enable & 0x01U) != 0U &&
                       (inst.envelope[3].loop == 2 || inst.envelope[3].loop == 3);
    const bool warpEn = (inst.envelope[4].enable & 0x02U) != 0U &&
                        (inst.envelope[4].loop == 2 || inst.envelope[4].loop == 3);

    if (posEn || warpEn)
    {
        const int src = posEn ? 3 : 4;
        setParam(P_WT_ENV_MODE, (inst.envelope[src].loop == 3) ? 2.0f : 1.0f);
        setParam(P_WT_ENV_SHAPE, (float)inst.lfo[src].shape);
        setParam(P_WT_ENV_WARP_X, (float)unpackWarpX(inst.lfo[src].wtBend));
        setParam(P_WT_ENV_WARP_Y, (float)unpackWarpY(inst.lfo[src].wtBend));
        setParam(P_WT_ENV_SUST, (float)inst.lfo[src].wtSust);
        uint16_t durMs = (inst.envelope[src].loop == 3) ? inst.envelope[src].attack
                                                        : inst.envelope[src].decay;
        if (durMs >= 50)
            setParam(P_WT_ENV_SPEED, (float)durMs * 0.001f);
        setParam(P_WT_ENV_TO_POS, posEn ? inst.envelope[3].amount : 0.0f);
        setParam(P_WT_ENV_TO_WARP, warpEn ? inst.envelope[4].amount : 0.0f);
    }
    else
    {
        setParam(P_WT_ENV_MODE, 0.0f);
        setParam(P_WT_ENV_TO_POS, 0.0f);
        setParam(P_WT_ENV_TO_WARP, 0.0f);
    }

    return true;
}

} // namespace wt
