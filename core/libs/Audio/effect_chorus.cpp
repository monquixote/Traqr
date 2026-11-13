#include "effect_chorus.h"
#include "config.h"
#include <algorithm>
#include <cmath>
#include <cstring>

static int16_t chorusDelayBuffer[AudioEffectChorus::kBufferSize];

static constexpr float kCenterDelaySamples = 176.0f; // ~4.0 ms at 44.1 kHz
static constexpr float kMaxExcursionSamples = 140.0f; // +/- ~3.17 ms
static constexpr float kMinRateHz = 0.1f;
static constexpr float kRateRatio = 50.0f; // 0.1 Hz .. 5.0 Hz
static constexpr float kBbdLpCoeff = 0.68f;

static inline AUDIO_CORE_RAM float fastSin01(float phase01)
{
    float x = phase01 * 2.0f - 1.0f; // -1 .. +1
    float y = 4.0f * x * (1.0f - fabsf(x));
    return 0.225f * (y * fabsf(y) - y) + y;
}

static inline AUDIO_CORE_RAM int16_t clampInt16(float v)
{
    if (v > 32767.0f)
        return 32767;
    if (v < -32768.0f)
        return -32768;
    return static_cast<int16_t>(v);
}

AudioEffectChorus::AudioEffectChorus(void)
  : AudioStream(1, inputQueueArray)
{
    clear();
    setRate(25);
    setDepth(50);
    setWidth(50);
    blockUpdate();
}

AUDIO_CORE_FLASH void AudioEffectChorus::begin(uint8_t rate, uint8_t depth, uint8_t width)
{
    blockUpdate();
    clear();
    setRate(rate);
    setDepth(depth);
    setWidth(width);
    unblockUpdate();
}

AUDIO_CORE_FLASH void AudioEffectChorus::clear(void)
{
    std::memset(chorusDelayBuffer, 0, sizeof(chorusDelayBuffer));
    headIdx = 0;
    silentSamplesCount = kMaxActiveTailSamples;
    lfoPhase = 0.0f;
    lpLeft = 0.0f;
    lpRight = 0.0f;
}

void AudioEffectChorus::setRate(uint8_t rate)
{
    float norm = static_cast<float>(std::min<uint8_t>(rate, 100)) * 0.01f;
    float rateHz = kMinRateHz * std::pow(kRateRatio, norm);
    lfoPhaseInc = rateHz / static_cast<float>(AUDIO_SAMPLE_RATE_EXACT);
}

void AudioEffectChorus::setDepth(uint8_t depth)
{
    float norm = static_cast<float>(std::min<uint8_t>(depth, 100)) * 0.01f;
    modDepthSamples = norm * kMaxExcursionSamples;
}

void AudioEffectChorus::setWidth(uint8_t width)
{
    float norm = static_cast<float>(std::min<uint8_t>(width, 100)) * 0.01f;
    widthMain = 0.5f + 0.5f * norm;
    widthCross = 0.5f - 0.5f * norm;
}

AUDIO_CORE_RAM void AudioEffectChorus::update(void)
{
    if (isBlockedUpdate)
    {
        audio_block_t* discard = receiveReadOnly(0);
        if (discard)
            release(discard);
        return;
    }

    audio_block_t* inBlock = receiveReadOnly(0);

    bool blockHasSignal = false;
    if (inBlock != nullptr)
    {
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            if (inBlock->data[i] != 0)
            {
                blockHasSignal = true;
                break;
            }
        }
    }

    if (!blockHasSignal && isSilent())
    {
        if (inBlock != nullptr)
            release(inBlock);
        lfoPhase += lfoPhaseInc * static_cast<float>(AUDIO_BLOCK_SAMPLES);
        if (lfoPhase >= 1.0f)
            lfoPhase -= static_cast<int>(lfoPhase);
        return;
    }

    audio_block_t* outLeft = allocate();
    audio_block_t* outRight = allocate();
    if (outLeft == nullptr || outRight == nullptr)
    {
        if (outLeft != nullptr)
            release(outLeft);
        if (outRight != nullptr)
            release(outRight);
        if (inBlock != nullptr)
            release(inBlock);
        return;
    }

    if (blockHasSignal)
        silentSamplesCount = 0;
    else
        silentSamplesCount += AUDIO_BLOCK_SAMPLES;

    const int16_t* src = (inBlock != nullptr) ? inBlock->data : nullptr;
    int16_t* dstL = outLeft->data;
    int16_t* dstR = outRight->data;

    float phase = lfoPhase;
    const float phaseInc = lfoPhaseInc;
    const float depth = modDepthSamples;
    const float cMain = widthMain;
    const float cCross = widthCross;
    float fL = lpLeft;
    float fR = lpRight;
    uint32_t wIdx = headIdx;

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
    {
        int16_t inSample = (src != nullptr) ? src[i] : 0;
        chorusDelayBuffer[wIdx] = inSample;

        float lfo = fastSin01(phase);
        phase += phaseInc;
        if (phase >= 1.0f)
            phase -= 1.0f;

        float delayA = kCenterDelaySamples + depth * lfo;
        float delayB = kCenterDelaySamples - depth * lfo;

        uint32_t intA = static_cast<uint32_t>(delayA);
        float fracA = delayA - static_cast<float>(intA);
        uint32_t idxA0 = (wIdx - intA) & kBufferMask;
        uint32_t idxA1 = (wIdx - intA - 1) & kBufferMask;
        float tapA = static_cast<float>(chorusDelayBuffer[idxA0]) * (1.0f - fracA) +
                     static_cast<float>(chorusDelayBuffer[idxA1]) * fracA;

        uint32_t intB = static_cast<uint32_t>(delayB);
        float fracB = delayB - static_cast<float>(intB);
        uint32_t idxB0 = (wIdx - intB) & kBufferMask;
        uint32_t idxB1 = (wIdx - intB - 1) & kBufferMask;
        float tapB = static_cast<float>(chorusDelayBuffer[idxB0]) * (1.0f - fracB) +
                     static_cast<float>(chorusDelayBuffer[idxB1]) * fracB;

        fL += kBbdLpCoeff * (tapA - fL);
        fR += kBbdLpCoeff * (tapB - fR);

        dstL[i] = clampInt16(fL * cMain + fR * cCross);
        dstR[i] = clampInt16(fR * cMain + fL * cCross);

        wIdx = (wIdx + 1) & kBufferMask;
    }

    headIdx = wIdx;
    lfoPhase = phase;
    lpLeft = fL;
    lpRight = fR;

    if (inBlock != nullptr)
        release(inBlock);

    transmit(outLeft, 0);
    transmit(outRight, 1);
    release(outLeft);
    release(outRight);
}
