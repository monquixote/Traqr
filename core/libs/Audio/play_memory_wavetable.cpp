#include "Audio.h"
#include "play_memory.h"
#include "utility/dspinst.h"

AUDIO_CORE_FLASH void AudioPlayMemory::playWavetable(uint8_t instrIdx, int8_t note)
{
    //	__disable_irq();
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
    targetWindowFloat = wavetableWindowForceFlag
                          ? forcedWavetableWindowFloat
                          : (float)mtProject.instrument[instrIdx].wavetableCurrentWindow;
    currentWindowFloat = targetWindowFloat;
    prevWindowFloat = currentWindowFloat;
    currentWindow = (uint32_t)currentWindowFloat;
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

    playing = 1;
    //	__enable_irq();
    AudioInterrupts();
}

AUDIO_CORE_RAM audio_block_t* AudioPlayMemory::updateWavetable()
{
    audio_block_t* block = nullptr;
    int16_t* out = nullptr;
    int32_t castPitchControl;
    float pitchFraction;

    block = allocate();
    if (!block)
        return nullptr;

    if (!playing)
    {
        release(block);
        return nullptr;
    }
    else if (playing == 1)
    {
        if (wavetableWindowSize < 4)
        {
            release(block);
            return nullptr;
        }

        out = block->data;

        const float targetWin =
          wavetableWindowForceFlag ? forcedWavetableWindowFloat : targetWindowFloat;
        const uint32_t totalWindows =
          mtProject.instrument[currentInstrIdx].sample.wavetableWindowNumber;
        const uint32_t maxWinIdx = (totalWindows > 0) ? (totalWindows - 1) : 0;
        const float maxWin = (float)maxWinIdx;

        currentWindowFloat = targetWin;
        if (currentWindowFloat > maxWin)
            currentWindowFloat = maxWin;
        else if (currentWindowFloat < 0.0f)
            currentWindowFloat = 0.0f;

        if (onsetFadeSamples == 32)
        {
            prevWindowFloat = currentWindowFloat;
        }

        const uint32_t maxlength = (length >= wavetableWindowSize)
                                     ? (length - (length % wavetableWindowSize) - wavetableWindowSize)
                                     : 0;

        const int32_t maxW14 = (int32_t)maxWinIdx << 14;
        int32_t w14 = (int32_t)(prevWindowFloat * 16384.0f);
        if (w14 < 0)
            w14 = 0;
        else if (w14 > maxW14)
            w14 = maxW14;

        int32_t targetW14 = (int32_t)(currentWindowFloat * 16384.0f);
        if (targetW14 < 0)
            targetW14 = 0;
        else if (targetW14 > maxW14)
            targetW14 = maxW14;

        const int32_t wStep14 = (totalWindows > 1) ? ((targetW14 - w14) >> 7) : 0;
        const bool zIsRamping = (wStep14 != 0);

        currentWindow = (uint32_t)currentWindowFloat;
        float steadyZFrac = currentWindowFloat - (float)currentWindow;
        if (steadyZFrac < 0.0005f)
        {
            steadyZFrac = 0.0f;
        }
        else if (steadyZFrac > 0.9995f)
        {
            steadyZFrac = 0.0f;
            if (currentWindow < maxWinIdx)
                currentWindow++;
        }

        uint32_t steadyPosA = wavetableWindowSize * currentWindow;
        if (steadyPosA > maxlength)
            steadyPosA = maxlength;

        int16_t* inA = (int16_t*)next + steadyPosA;
        int16_t* inB = nullptr;
        int32_t zFrac14 = (int32_t)(steadyZFrac * 16384.0f);

        if (zFrac14 > 0 && totalWindows > 1 && currentWindow < maxWinIdx)
        {
            uint32_t nextTablePos = wavetableWindowSize * (currentWindow + 1);
            if (nextTablePos > maxlength)
                nextTablePos = maxlength;
            if (nextTablePos != steadyPosA)
                inB = (int16_t*)next + nextTablePos;
            else
                zFrac14 = 0;
        }
        else
        {
            zFrac14 = 0;
        }

        waveTablePosition = steadyPosA;

        uint32_t cachedWinA = currentWindow;
        if (zIsRamping)
        {
            cachedWinA = (uint32_t)(w14 >> 14);
            zFrac14 = w14 & 0x3FFF;
            uint32_t posA = wavetableWindowSize * cachedWinA;
            if (posA > maxlength)
                posA = maxlength;
            inA = (int16_t*)next + posA;
            if (cachedWinA < maxWinIdx)
            {
                uint32_t posB = wavetableWindowSize * (cachedWinA + 1);
                if (posB > maxlength)
                    posB = maxlength;
                inB = (posB != posA) ? ((int16_t*)next + posB) : nullptr;
            }
            else
            {
                inB = nullptr;
            }
            if (inB == nullptr)
                zFrac14 = 0;
        }

        castPitchControl = (int32_t)pitchControl;
        pitchFraction = pitchControl - (int32_t)pitchControl;

        int32_t currentFractionPitchCounter = fPitchCounter * MAX_16BIT;
        int32_t currentFractionPitchControl = pitchFraction * MAX_16BIT;

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            //*********************************** GLIDE HANDLE
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

            //***************************************************************************************
            // Cyclic sample indices (seam-safe wrap: wraps to sample 0 of the current single-cycle)
            const uint32_t idx1 = (uint32_t)iPitchCounter;
            uint32_t idx2 = idx1 + 1;
            if (idx2 >= wavetableWindowSize)
                idx2 = 0;

            if (zIsRamping)
            {
                w14 += wStep14;
                if (w14 < 0)
                    w14 = 0;
                else if (w14 > maxW14)
                    w14 = maxW14;

                const uint32_t winA = (uint32_t)(w14 >> 14);
                zFrac14 = w14 & 0x3FFF;

                if (winA != cachedWinA)
                {
                    cachedWinA = winA;
                    uint32_t posA = wavetableWindowSize * winA;
                    if (posA > maxlength)
                        posA = maxlength;
                    inA = (int16_t*)next + posA;
                    if (winA < maxWinIdx)
                    {
                        uint32_t posB = wavetableWindowSize * (winA + 1);
                        if (posB > maxlength)
                            posB = maxlength;
                        inB = (posB != posA) ? ((int16_t*)next + posB) : nullptr;
                    }
                    else
                    {
                        inB = nullptr;
                    }
                }

                if (inB == nullptr)
                    zFrac14 = 0;
            }

            const int32_t xFrac14 = currentFractionPitchCounter >> 2;
            const int32_t sampleA = inA[idx1];
            int32_t outSample = sampleA + (((inA[idx2] - sampleA) * xFrac14) >> 14);

            if (zFrac14 > 0 && inB != nullptr)
            {
                const int32_t sampleB = inB[idx1];
                const int32_t sampB = sampleB + (((inB[idx2] - sampleB) * xFrac14) >> 14);
                outSample += ((sampB - outSample) * zFrac14) >> 14;
            }

            if (onsetFadeSamples > 0)
            {
                int32_t fadeMult = 32 - onsetFadeSamples; // 0 to 31
                outSample = (outSample * fadeMult) >> 5;
                onsetFadeSamples--;
            }

            *out++ = (int16_t)outSample;

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
        prevWindowFloat = currentWindowFloat;
        next = currentStartAddress + pointsInSamples.start;
        fPitchCounter = (float)currentFractionPitchCounter / MAX_16BIT;

        return block;
    }
}
