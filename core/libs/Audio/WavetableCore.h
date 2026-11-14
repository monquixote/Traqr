#pragma once

#include <stdint.h>

// Hardware-agnostic fixed-point Wavetable DSP core shared by Tracker firmware
// (AudioPlayMemory::updateWavetable) and the WASM web synth harness.

struct WtCoreVoiceState
{
    int32_t iPitchCounter = 0;
    int32_t fracPitchCounter = 0; // Q16
    uint32_t subPhase32 = 0;
    uint32_t prevWaveTablePosition = UINT32_MAX;
    uint16_t smoothedWarpQ8 = 0; // 0..25600 (0..100% in Q8)
    int32_t foldPrevRaw = 0;
    int32_t foldLpState = 0;
    uint8_t onsetFadeSamples = 32;
};

static inline int32_t wtClamp16(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return v;
}

// Fast parabolic sine approximation (~0.1% THD, zero SDRAM/table reads).
// phase32 covers [0, 2*pi) across [0, 0xFFFFFFFF], returning [-32767, 32767].
static inline int32_t wtFastSinQ15(uint32_t phase32)
{
    const int32_t x = ((int32_t)phase32) >> 16;
    const int32_t absX = (x < 0) ? -x : x;
    const int32_t y = (x * (32768 - absX)) >> 13;
    const int32_t absY = (y < 0) ? -y : y;
    const int32_t s = (y * (25395 + ((absY * 7373) >> 15))) >> 15;
    return wtClamp16(s);
}

// Musical 50/50 linear+quadratic taper over Q8 warp amount [0..25600] -> Q15 [0..32768].
static inline uint32_t wtWarpTaperQ15(int32_t curWarpQ8)
{
    if (curWarpQ8 <= 0)
        return 0U;
    if (curWarpQ8 >= 25600)
        return 32768U;
    // curWarpQ8 * 32 / 25 via 32-bit multiply-shift (25600 * 83886 >> 16 = 32767, zero division)
    const uint32_t uQ15 = ((uint32_t)curWarpQ8 * 83886U) >> 16;
    return (uQ15 + ((uQ15 * uQ15) >> 15)) >> 1;
}

// West-Coast sine wavefolder with soft fundamental retention and smooth onset.
static inline int32_t wtFoldSingleSample(int32_t sample, int32_t driveQ12, uint32_t taperQ15)
{
    const int32_t xClamp = (sample > 32767) ? 32767 : ((sample < -32767) ? -32767 : sample);
    const int32_t absX = (xClamp < 0) ? -xClamp : xClamp;
    // Soft-saturated fundamental: x * (1.5 - 0.5*|x|)
    const int32_t softFund = (xClamp * (49152 - ((absX * 16384) >> 15))) >> 15;

    const int32_t driven = (sample * driveQ12) >> 12;
    const uint32_t foldPhase32 = ((uint32_t)driven) << 15;
    const int32_t sineFolded = wtFastSinQ15(foldPhase32);

    // 80% sine folder + 20% soft fundamental
    const int32_t wet = ((sineFolded * 26214) + (softFund * 6554)) >> 15;

    // Smooth dry/wet crossfade over first 25% of taper so normalized tables ease in cleanly
    if (taperQ15 < 8192U)
    {
        const int32_t mixQ15 = (int32_t)(taperQ15 << 2);
        return sample + (((wet - sample) * mixQ15) >> 15);
    }
    return wet;
}

// Applies phase warping (Mode 1 = Smooth Asym, Mode 2 = Window-Softened Sync) to p16.
static inline uint32_t wtApplyPhaseWarp16(uint32_t p16,
                                          uint32_t wSize16,
                                          bool wIsPow2,
                                          uint8_t wShift,
                                          uint32_t wMask16,
                                          uint8_t warpMode,
                                          uint32_t taperQ15,
                                          int32_t& outSyncWinQ15)
{
    outSyncWinQ15 = 32768;
    if (taperQ15 == 0U || (warpMode != 1U && warpMode != 2U))
        return p16;

    const uint32_t normPhase32 = (wIsPow2 && wShift <= 16U)
                                   ? (p16 << (16U - wShift))
                                   : (uint32_t)(((uint64_t)p16 << 32) / (wSize16 ? wSize16 : 1U));

    if (warpMode == 1U)
    {
        // 2-stage strictly monotonic sine phase distortion (max compression ~3.6x, C^inf smooth)
        // Pure 32-bit saturating add: |s1 * k1| <= 32767 * 19000 = 622M < 2^31.
        const int32_t k1 = ((int32_t)taperQ15 * 19000) >> 15;
        const int32_t s1 = wtFastSinQ15(normPhase32);
        const int32_t d1 = s1 * k1;
        uint32_t pMid32 = normPhase32 + (uint32_t)d1;
        if (d1 >= 0)
        {
            if (pMid32 < normPhase32)
                pMid32 = 0xFFFFFFFFU;
        }
        else
        {
            if (pMid32 > normPhase32)
                pMid32 = 0U;
        }

        const int32_t s2 = wtFastSinQ15(pMid32);
        const int32_t d2 = s2 * (k1 >> 1);
        uint32_t pOut32 = pMid32 + (uint32_t)d2;
        if (d2 >= 0)
        {
            if (pOut32 < pMid32)
                pOut32 = 0xFFFFFFFFU;
        }
        else
        {
            if (pOut32 > pMid32)
                pOut32 = 0U;
        }

        uint32_t warped16 = (wIsPow2 && wShift <= 16U)
                              ? (pOut32 >> (16U - wShift))
                              : (uint32_t)(((uint64_t)pOut32 * (uint64_t)wSize16) >> 32);
        if (warped16 >= wSize16)
            warped16 = wSize16 - 1U;
        return warped16;
    }

    // Mode 2: Window-softened hard sync (1.0x .. 4.0x slave ratio)
    const uint32_t syncRatio12 = 4096U + (((uint32_t)taperQ15 * 12288U) >> 15);
    uint32_t warped16 = (uint32_t)(((uint64_t)p16 * (uint64_t)syncRatio12) >> 12);
    if (wIsPow2)
        warped16 &= wMask16;
    else if (wSize16 > 0U)
        warped16 %= wSize16;

    // Raised-parabolic window over the last 1/8th of the master cycle to tame non-integer wrap edge
    if (normPhase32 >= 0xE0000000U)
    {
        const uint32_t tailQ15 = (normPhase32 - 0xE0000000U) >> 14; // 0..32767
        const uint32_t inv = 32767U - tailQ15;
        const int32_t winQ15 = (int32_t)((inv * (65535U - inv)) >> 15);
        outSyncWinQ15 = 32768 - (((32768 - winQ15) * (int32_t)taperQ15) >> 15);
    }

    return warped16;
}

// Reads one unwarped wavetable sample with linear interpolation (+ optional 32-sample window xfade).
static inline int32_t wtReadUnwarpedSample(const int16_t* in,
                                           const int16_t* inPrev,
                                           int sampleIndexInBlock,
                                           bool doWindowXfade,
                                           uint32_t iPitchCounter,
                                           int32_t fracCounter,
                                           uint32_t wSize,
                                           int32_t& foldPrevRaw,
                                           int32_t& foldLpState)
{
    uint32_t idx1 = iPitchCounter;
    if (idx1 >= wSize)
        idx1 = wSize - 1U;
    uint32_t idx2 = idx1 + 1U;
    if (idx2 >= wSize)
        idx2 = 0U;

    const int32_t currentSampleValue = in[idx1];
    const int32_t interpolationDif = in[idx2] - currentSampleValue;
    int32_t wtSample =
      currentSampleValue + (int32_t)(((fracCounter >> 2) * interpolationDif) >> 14);

    if (doWindowXfade && inPrev != nullptr && sampleIndexInBlock < 32)
    {
        const int32_t prevSampleValue = inPrev[idx1];
        const int32_t prevInterpDif = inPrev[idx2] - prevSampleValue;
        const int32_t prevWtSample =
          prevSampleValue + (int32_t)(((fracCounter >> 2) * prevInterpDif) >> 14);
        wtSample = ((prevWtSample * (32 - sampleIndexInBlock)) + (wtSample * sampleIndexInBlock)) >> 5;
    }

    foldPrevRaw = wtSample;
    foldLpState = wtSample;
    return wtSample;
}

// Reads and warps one wavetable sample using a precomputed Q15 warp taper.
static inline int32_t wtReadWarpedSampleWithTaper(const int16_t* in,
                                                  const int16_t* inPrev,
                                                  int sampleIndexInBlock,
                                                  bool doWindowXfade,
                                                  uint32_t iPitchCounter,
                                                  int32_t fracCounter,
                                                  uint32_t wSize,
                                                  uint32_t wSize16,
                                                  bool wIsPow2,
                                                  uint8_t wShift,
                                                  uint32_t wMask16,
                                                  uint8_t warpMode,
                                                  uint32_t taperQ15,
                                                  int32_t& foldPrevRaw,
                                                  int32_t& foldLpState)
{
    if (warpMode == 0U || taperQ15 == 0U)
    {
        return wtReadUnwarpedSample(in,
                                    inPrev,
                                    sampleIndexInBlock,
                                    doWindowXfade,
                                    iPitchCounter,
                                    fracCounter,
                                    wSize,
                                    foldPrevRaw,
                                    foldLpState);
    }

    uint32_t idx1 = iPitchCounter;
    int32_t frac = fracCounter;
    int32_t syncWinQ15 = 32768;

    if ((warpMode == 1U || warpMode == 2U) && taperQ15 > 0U)
    {
        const uint32_t p16 = (idx1 << 16) + (uint32_t)fracCounter;
        const uint32_t warped16 = wtApplyPhaseWarp16(
          p16, wSize16, wIsPow2, wShift, wMask16, warpMode, taperQ15, syncWinQ15);
        idx1 = warped16 >> 16;
        frac = (int32_t)(warped16 & 0xFFFFU);
    }

    if (idx1 >= wSize)
        idx1 = wSize - 1U;
    uint32_t idx2 = idx1 + 1U;
    if (idx2 >= wSize)
        idx2 = 0U;

    const int32_t currentSampleValue = in[idx1];
    const int32_t interpolationDif = in[idx2] - currentSampleValue;
    int32_t wtSample =
      currentSampleValue + (int32_t)(((frac >> 2) * interpolationDif) >> 14);

    if (doWindowXfade && inPrev != nullptr && sampleIndexInBlock < 32)
    {
        const int32_t prevSampleValue = inPrev[idx1];
        const int32_t prevInterpDif = inPrev[idx2] - prevSampleValue;
        const int32_t prevWtSample =
          prevSampleValue + (int32_t)(((frac >> 2) * prevInterpDif) >> 14);
        wtSample = ((prevWtSample * (32 - sampleIndexInBlock)) + (wtSample * sampleIndexInBlock)) >> 5;
    }

    if (warpMode == 2U && syncWinQ15 < 32768)
    {
        wtSample = (wtSample * syncWinQ15) >> 15;
    }
    else if (warpMode == 3U && taperQ15 > 0U)
    {
        // 2x register-only midpoint oversampling + West-Coast sine wavefolder + gentle 1-pole LP
        const int32_t driveQ12 = 4096 + (int32_t)(((uint32_t)taperQ15 * 10240U) >> 15); // 1.0x .. 3.5x
        const int32_t midRaw = (foldPrevRaw + wtSample) >> 1;
        foldPrevRaw = wtSample;

        const int32_t fMid = wtFoldSingleSample(midRaw, driveQ12, taperQ15);
        const int32_t fCur = wtFoldSingleSample(wtSample, driveQ12, taperQ15);
        const int32_t fAvg = (fMid + fCur) >> 1;

        const int32_t lpAlphaQ15 = 32768 - (int32_t)(((uint32_t)taperQ15 * 11468U) >> 15);
        foldLpState += ((fAvg - foldLpState) * lpAlphaQ15) >> 15;
        wtSample = foldLpState;
    }
    else
    {
        foldPrevRaw = wtSample;
        foldLpState = wtSample;
    }

    return wtSample;
}

// Reads and warps one wavetable sample from a single window (plus optional 32-sample window xfade).
static inline int32_t wtReadWarpedSample(const int16_t* in,
                                         const int16_t* inPrev,
                                         int sampleIndexInBlock,
                                         bool doWindowXfade,
                                         uint32_t iPitchCounter,
                                         int32_t fracCounter,
                                         uint32_t wSize,
                                         uint32_t wSize16,
                                         bool wIsPow2,
                                         uint8_t wShift,
                                         uint32_t wMask16,
                                         uint8_t warpMode,
                                         int32_t curWarpQ8,
                                         int32_t& foldPrevRaw,
                                         int32_t& foldLpState)
{
    const uint32_t taperQ15 = (warpMode != 0U) ? wtWarpTaperQ15(curWarpQ8) : 0U;
    return wtReadWarpedSampleWithTaper(in,
                                       inPrev,
                                       sampleIndexInBlock,
                                       doWindowXfade,
                                       iPitchCounter,
                                       fracCounter,
                                       wSize,
                                       wSize16,
                                       wIsPow2,
                                       wShift,
                                       wMask16,
                                       warpMode,
                                       taperQ15,
                                       foldPrevRaw,
                                       foldLpState);
}

// Generates one sub-oscillator sample (-1 octave) matching Tracker's SubWave modes.
static inline int32_t wtEvalSubOsc(uint8_t subWave, uint32_t subPhase32)
{
    if (subWave == 1U)
        return wtFastSinQ15(subPhase32);
    if (subWave == 2U)
    {
        uint32_t t = subPhase32 >> 16;
        if (t >= 32768U)
            t = 65535U - t;
        return (int32_t)(t << 1) - 32767;
    }
    if (subWave == 3U)
        return (subPhase32 & 0x80000000U) ? -32767 : 32767;
    return 0;
}
