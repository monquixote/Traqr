#include "Audio.h"
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
    uint8_t warpAmt = mtProject.instrument[currentInstrIdx].wtWarpAmount;
    if (warpAmt > 100)
        warpAmt = 100;
    if (warpAmt == 0)
        warpMode = 0;

    const uint32_t wSize = wavetableWindowSize;
    const uint32_t wSize16 = wSize << 16;
    const bool wIsPow2 = (wSize & (wSize - 1U)) == 0;
    const uint32_t wMask16 = wSize16 - 1U;

    uint32_t asymMid16 = 0;
    uint32_t asymHalfW16 = 0;
    uint32_t asymSlope1_12 = 4096U;
    uint32_t asymSlope2_12 = 4096U;
    uint32_t syncRatio12 = 4096U;
    int32_t foldGain12 = 4096;

    if (warpMode == 1)
    {
        asymHalfW16 = wSize << 15;
        asymMid16 = (uint32_t)(((uint64_t)asymHalfW16 * (100U - ((uint32_t)warpAmt * 9U) / 10U)) / 100U);
        if (asymMid16 < 1U)
            asymMid16 = 1U;
        asymSlope1_12 = (uint32_t)(((uint64_t)asymHalfW16 << 12) / asymMid16);
        const uint32_t rem16 = wSize16 - asymMid16;
        asymSlope2_12 = (uint32_t)(((uint64_t)asymHalfW16 << 12) / (rem16 ? rem16 : 1U));
    }
    else if (warpMode == 2)
    {
        syncRatio12 = 4096U + ((uint32_t)warpAmt * 12288U) / 100U;
    }
    else if (warpMode == 3)
    {
        foldGain12 = 4096 + ((int32_t)warpAmt * 12288) / 100;
    }

    auto readWarpedSample = [&](int i, int32_t fracCounter) -> int32_t {
        uint32_t idx1 = (uint32_t)iPitchCounter;
        int32_t frac = fracCounter;

        if (warpMode == 1)
        {
            const uint32_t p16 = (idx1 << 16) + (uint32_t)fracCounter;
            uint32_t warped16 = (p16 < asymMid16)
                                  ? (uint32_t)(((uint64_t)p16 * asymSlope1_12) >> 12)
                                  : (asymHalfW16 + (uint32_t)(((uint64_t)(p16 - asymMid16) * asymSlope2_12) >> 12));
            if (warped16 >= wSize16)
                warped16 = wSize16 - 1U;
            idx1 = warped16 >> 16;
            frac = (int32_t)(warped16 & 0xFFFFU);
        }
        else if (warpMode == 2)
        {
            const uint32_t p16 = (idx1 << 16) + (uint32_t)fracCounter;
            uint32_t warped16 = (uint32_t)(((uint64_t)p16 * syncRatio12) >> 12);
            if (wIsPow2)
                warped16 &= wMask16;
            else
                warped16 %= wSize16;
            idx1 = warped16 >> 16;
            frac = (int32_t)(warped16 & 0xFFFFU);
        }

        uint32_t idx2 = idx1 + 1U;
        if (idx2 >= wSize)
            idx2 = 0;

        const int32_t currentSampleValue = in[idx1];
        const int32_t interpolationDif = in[idx2] - currentSampleValue;

        int32_t wtSample = currentSampleValue +
                           (int32_t)(((frac >> 2) * interpolationDif) >> 14);

        if (doWindowXfade && i < 32)
        {
            const int32_t prevSampleValue = inPrev[idx1];
            const int32_t prevInterpDif = inPrev[idx2] - prevSampleValue;
            const int32_t prevWtSample = prevSampleValue +
                                         (int32_t)(((frac >> 2) * prevInterpDif) >> 14);
            wtSample = ((prevWtSample * (32 - i)) + (wtSample * i)) >> 5;
        }

        if (warpMode == 3)
        {
            wtSample = (wtSample * foldGain12) >> 12;
            while (wtSample > 32767 || wtSample < -32768)
            {
                if (wtSample > 32767)
                    wtSample = 65534 - wtSample;
                else
                    wtSample = -65536 - wtSample;
            }
        }

        return wtSample;
    };

    if (subWave == 0 || subLevel == 0)
    {
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            if (constrainsInSamples.pitchSmoothing)
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
                    castPitchControl = (int32_t)pitchControl;
                    pitchFraction = pitchControl - (int32_t)pitchControl;
                    currentFractionPitchControl = pitchFraction * MAX_16BIT;
                    pitchSmoothingControlCounter++;
                }
            }

            const int32_t wtSample = readWarpedSample(i, currentFractionPitchCounter);
            *out++ = (int16_t)wtSample;

            iPitchCounter += castPitchControl;
            currentFractionPitchCounter += currentFractionPitchControl;
            if (currentFractionPitchCounter >= MAX_16BIT)
            {
                currentFractionPitchCounter -= MAX_16BIT;
                iPitchCounter++;
            }

            if (iPitchCounter >= wavetableWindowSize)
            {
                iPitchCounter -= wavetableWindowSize;
            }
        }
    }
    else
    {
        const float subPhaseScale = 2147483648.0f / (float)wavetableWindowSize;
        uint32_t subPhaseInc = (uint32_t)(pitchControl * subPhaseScale);
        const int32_t subGain14 = ((int32_t)subLevel * 16384) / 100;

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            if (constrainsInSamples.pitchSmoothing)
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
                    castPitchControl = (int32_t)pitchControl;
                    pitchFraction = pitchControl - (int32_t)pitchControl;
                    currentFractionPitchControl = pitchFraction * MAX_16BIT;
                    subPhaseInc = (uint32_t)(pitchControl * subPhaseScale);
                    pitchSmoothingControlCounter++;
                }
            }

            const int32_t wtSample = readWarpedSample(i, currentFractionPitchCounter);

            int32_t subSample = 0;
            if (subWave == 1)
            {
                // Fast parabolic sine approximation (0.1% THD, zero SDRAM reads)
                const int32_t x = ((int32_t)subPhase32) >> 16;
                const int32_t absX = (x < 0) ? -x : x;
                const int32_t y = (x * (32768 - absX)) >> 13;
                const int32_t absY = (y < 0) ? -y : y;
                subSample = (y * (25395 + ((absY * 7373) >> 15))) >> 15;
            }
            else if (subWave == 2)
            {
                // Triangle wave
                uint32_t t = subPhase32 >> 16;
                if (t >= 32768U)
                    t = 65535U - t;
                subSample = (int32_t)(t << 1) - 32767;
            }
            else
            {
                // Square wave
                subSample = (subPhase32 & 0x80000000U) ? -32767 : 32767;
            }

            int32_t mixed = wtSample + ((subSample * subGain14) >> 14);
            if (mixed > 32767)
                mixed = 32767;
            else if (mixed < -32768)
                mixed = -32768;

            *out++ = (int16_t)mixed;

            subPhase32 += subPhaseInc;

            iPitchCounter += castPitchControl;
            currentFractionPitchCounter += currentFractionPitchControl;
            if (currentFractionPitchCounter >= MAX_16BIT)
            {
                currentFractionPitchCounter -= MAX_16BIT;
                iPitchCounter++;
            }

            if (iPitchCounter >= wavetableWindowSize)
            {
                iPitchCounter -= wavetableWindowSize;
            }
        }
    }

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
