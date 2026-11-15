#include "Audio.h"
#include "WavetableCore.h"
#include "play_memory.h"
#include "utility/dspinst.h"

AUDIO_CORE_FLASH void AudioPlayMemory::playWavetable(uint8_t instrIdx, int8_t note)
{
    AudioNoInterrupts();

    if (instrIdx > kSampleInstrumentsCount - 1)
        instrIdx = kSampleInstrumentsCount - 1;
    if (note > MAX_NOTE)
        note = MAX_NOTE;

    lastInstrIdxInPlay = currentInstrIdxInPlay;
    currentInstrIdxInPlay = instrIdx;
    currentInstrIdx = instrIdx;

    switch (mtProject.instrument[instrIdx].sample.wavetable_window_size)
    {
        case 32: wt_notes = wt32Note; break;
        case 64: wt_notes = wt64Note; break;
        case 128: wt_notes = wt128Note; break;
        case 256: wt_notes = wt256Note; break;
        case 512: wt_notes = wt512Note; break;
        case 1024: wt_notes = wt1024Note; break;
        case 2048: wt_notes = wt2048Note; break;
        default: break;
    }

    refreshStartParameters();

    constrainCurrentTune(note);

    setStartPitch(note);

    applyFinetuneOnPitch(note);

    calculateGlidePitch(note);

    lastNote = note;

    wavetableWindowSize = mtProject.instrument[instrIdx].sample.wavetable_window_size;
    if (wavetableWindowForceFlag)
        currentWindow = forcedWavetableWindow;
    else
        currentWindow = mtProject.instrument[instrIdx].wavetableCurrentWindow;

    onsetFadeSamples = 32;

    constrainsInSamples.endPoint =
      wavetableWindowSize * mtProject.instrument[instrIdx].sample.wavetableWindowNumber;
    constrainsInSamples.loopPoint1 = 0;
    constrainsInSamples.loopPoint2 = wavetableWindowSize;
    constrainsInSamples.loopLength = wavetableWindowSize;

    pointsInSamples.start = 0;

    next = currentStartAddress;
    length = currentSampleLength;
    iPitchCounter = 0;
    fPitchCounter = 0.0f;
    subPhase32 = 0;
    prevWaveTablePosition = UINT32_MAX;
    currentWarpAmount = mtProject.instrument[instrIdx].wtWarpAmount;
    if (currentWarpAmount > 100)
        currentWarpAmount = 100;
    smoothedWarpQ8 = (uint16_t)currentWarpAmount << 8;
    foldPrevRaw = 0;
    foldLpState = 0;

    playing = 1;
    AudioInterrupts();
}

AUDIO_CORE_RAM audio_block_t* AudioPlayMemory::updateWavetable()
{
    audio_block_t* block = allocate();
    if (!block)
        return nullptr;

    if (playing != 1 || wavetableWindowSize == 0)
    {
        release(block);
        return nullptr;
    }

    int16_t* out = block->data;

    if (wavetableWindowForceFlag)
        waveTablePosition = wavetableWindowSize * forcedWavetableWindow;
    else
        waveTablePosition = wavetableWindowSize * currentWindow;

    const uint32_t maxlength =
      (length >= wavetableWindowSize)
        ? (length - (length % wavetableWindowSize) - wavetableWindowSize)
        : 0;

    if (waveTablePosition > maxlength)
    {
        waveTablePosition = maxlength;
    }

    const int16_t* in = (const int16_t*)next + waveTablePosition;
    const bool doWindowXfade = (prevWaveTablePosition != UINT32_MAX &&
                                prevWaveTablePosition != waveTablePosition &&
                                prevWaveTablePosition <= maxlength);
    const int16_t* inPrev = doWindowXfade ? ((const int16_t*)next + prevWaveTablePosition) : nullptr;
    int32_t castPitchControl = (int32_t)pitchControl;
    float pitchFraction = pitchControl - (int32_t)pitchControl;

    int32_t currentFractionPitchCounter = fPitchCounter * MAX_16BIT;
    int32_t currentFractionPitchControl = pitchFraction * MAX_16BIT;

    const uint8_t subWave = mtProject.instrument[currentInstrIdx].wtSubWave;
    uint8_t subLevel = mtProject.instrument[currentInstrIdx].wtSubLevel;
    if (subLevel > 100)
        subLevel = 100;

    uint8_t warpMode = mtProject.instrument[currentInstrIdx].wtWarpMode;
    if (warpMode > 3)
        warpMode = 0;
    uint8_t warpAmt = currentWarpAmount;
    if (warpAmt > 100)
        warpAmt = 100;
    if (warpAmt == 0)
        warpMode = 0;

    const uint32_t wSize = wavetableWindowSize;
    const uint32_t wSize16 = wSize << 16;
    const bool wIsPow2 = (wSize & (wSize - 1U)) == 0;
    const uint8_t wShift = wIsPow2 ? (uint8_t)__builtin_ctz(wSize) : 0U;
    const uint32_t wMask16 = wSize16 - 1U;

    const int32_t targetWarpQ8 = (warpMode == 0) ? 0 : ((int32_t)warpAmt << 8);
    int32_t curWarpQ8 = (int32_t)smoothedWarpQ8;
    const int32_t warpStepQ8 = (targetWarpQ8 - curWarpQ8) >> 7;
    const uint32_t blockTaperQ15 = (warpMode != 0U && warpStepQ8 == 0) ? wtWarpTaperQ15(curWarpQ8) : 0U;
    const bool unwarpedBlock = (warpMode == 0U || (warpStepQ8 == 0 && blockTaperQ15 == 0U));

    auto renderBlock = [&]<bool HasSub, bool Unwarped>(uint32_t subPhaseInc, int32_t subGain14, float subPhaseScale) __attribute__((always_inline)) {
        // Keep playhead & filter state in local registers for the entire block so *out++ stores cannot alias `this`.
        int32_t lPitchCounter = iPitchCounter;
        int32_t lFracCounter = currentFractionPitchCounter;
        int32_t lCastPitch = castPitchControl;
        int32_t lFracPitch = currentFractionPitchControl;
        int32_t lFoldPrev = foldPrevRaw;
        int32_t lFoldLp = foldLpState;
        uint32_t lSubPhase32 = subPhase32;
        int32_t lCurWarpQ8 = curWarpQ8;
        const uint16_t lWinSize = wavetableWindowSize;
        const bool lHasSmoothing = (constrainsInSamples.pitchSmoothing != 0);

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            if (lHasSmoothing)
            {
                if (pitchSmoothingControlCounter <= constrainsInSamples.pitchSmoothing)
                {
                    pitchControl += pitchSmoothingControl;
                    if (pitchControl > wt_notes[MAX_NOTE - 1])
                    {
                        pitchControl = wt_notes[MAX_NOTE - 1];
                        pitchSmoothingControlCounter = constrainsInSamples.pitchSmoothing;
                    }
                    else if (pitchControl < wt_notes[MIN_NOTE])
                    {
                        pitchControl = wt_notes[MIN_NOTE];
                        pitchSmoothingControlCounter = constrainsInSamples.pitchSmoothing;
                    }
                    lCastPitch = (int32_t)pitchControl;
                    pitchFraction = pitchControl - lCastPitch;
                    lFracPitch = pitchFraction * MAX_16BIT;
                    if constexpr (HasSub)
                    {
                        subPhaseInc = (uint32_t)(pitchControl * subPhaseScale);
                    }
                    pitchSmoothingControlCounter++;
                }
            }

            int32_t wtSample;
            if constexpr (Unwarped)
            {
                wtSample = wtReadUnwarpedSample(in,
                                                inPrev,
                                                i,
                                                doWindowXfade,
                                                (uint32_t)lPitchCounter,
                                                lFracCounter,
                                                wSize,
                                                lFoldPrev,
                                                lFoldLp);
            }
            else
            {
                const uint32_t taperQ15 =
                  (warpStepQ8 == 0) ? blockTaperQ15 : wtWarpTaperQ15(lCurWarpQ8);
                wtSample = wtReadWarpedSampleWithTaper(in,
                                                       inPrev,
                                                       i,
                                                       doWindowXfade,
                                                       (uint32_t)lPitchCounter,
                                                       lFracCounter,
                                                       wSize,
                                                       wSize16,
                                                       wIsPow2,
                                                       wShift,
                                                       wMask16,
                                                       warpMode,
                                                       taperQ15,
                                                       lFoldPrev,
                                                       lFoldLp);
                lCurWarpQ8 += warpStepQ8;
            }

            if constexpr (HasSub)
            {
                const int32_t subSample = wtEvalSubOsc(subWave, lSubPhase32);
                const int32_t mixed = wtClamp16(wtSample + ((subSample * subGain14) >> 14));
                *out++ = (int16_t)mixed;
                lSubPhase32 += subPhaseInc;
            }
            else
            {
                *out++ = (int16_t)wtClamp16(wtSample);
            }

            lPitchCounter += lCastPitch;
            lFracCounter += lFracPitch;
            if (lFracCounter >= MAX_16BIT)
            {
                lFracCounter -= MAX_16BIT;
                lPitchCounter++;
            }

            if (lPitchCounter >= lWinSize)
            {
                lPitchCounter -= lWinSize;
            }
        }

        iPitchCounter = lPitchCounter;
        currentFractionPitchCounter = lFracCounter;
        castPitchControl = lCastPitch;
        currentFractionPitchControl = lFracPitch;
        foldPrevRaw = lFoldPrev;
        foldLpState = lFoldLp;
        subPhase32 = lSubPhase32;
        curWarpQ8 = lCurWarpQ8;
    };

    if (subWave == 0 || subLevel == 0)
    {
        if (unwarpedBlock)
        {
            renderBlock.template operator()<false, true>(0U, 0, 0.0f);
        }
        else
        {
            renderBlock.template operator()<false, false>(0U, 0, 0.0f);
        }
    }
    else
    {
        const float subPhaseScale = 2147483648.0f / (float)wavetableWindowSize;
        const uint32_t subPhaseInc = (uint32_t)(pitchControl * subPhaseScale);
        const int32_t subGain14 = ((int32_t)subLevel * 16384) / 100;
        if (unwarpedBlock)
        {
            renderBlock.template operator()<true, true>(subPhaseInc, subGain14, subPhaseScale);
        }
        else
        {
            renderBlock.template operator()<true, false>(subPhaseInc, subGain14, subPhaseScale);
        }
    }

    smoothedWarpQ8 = (uint16_t)targetWarpQ8;

    if (onsetFadeSamples > 0)
    {
        int16_t* fadePtr = block->data;
        const uint8_t fadeCount =
          (onsetFadeSamples < AUDIO_BLOCK_SAMPLES) ? onsetFadeSamples : AUDIO_BLOCK_SAMPLES;
        for (uint8_t k = 0; k < fadeCount; k++)
        {
            const int32_t fadeMult = 32 - onsetFadeSamples; // 0 to 31
            fadePtr[k] = (int16_t)(((int32_t)fadePtr[k] * fadeMult) >> 5);
            onsetFadeSamples--;
        }
    }

    prevWaveTablePosition = waveTablePosition;
    next = currentStartAddress + pointsInSamples.start;
    fPitchCounter = (float)currentFractionPitchCounter / MAX_16BIT;

    return block;
}
