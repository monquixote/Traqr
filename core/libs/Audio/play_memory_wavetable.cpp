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
    currentWindow = (uint32_t)currentWindowFloat;

    constrainsInSamples.endPoint =
      wavetableWindowSize * mtProject.instrument[instrIdx].sample.wavetableWindowNumber;
    constrainsInSamples.loopPoint1 = 0;
    constrainsInSamples.loopPoint2 = wavetableWindowSize;
    constrainsInSamples.loopLength = wavetableWindowSize;

    pointsInSamples.start = 0;

    next = currentStartAddress;
    length = currentSampleLength;
    iPitchCounter = 0;

    playing = 1;
    //	__enable_irq();
    AudioInterrupts();
}

AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateWavetable()
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
        out = block->data;

        const float targetWin =
          wavetableWindowForceFlag ? forcedWavetableWindowFloat : targetWindowFloat;
        const uint32_t totalWindows =
          mtProject.instrument[currentInstrIdx].sample.wavetableWindowNumber;
        const float maxWin = (float)(totalWindows > 0 ? totalWindows - 1 : 0);

        // Smooth transition towards target window across audio blocks to eliminate stepping/zipper noise
        currentWindowFloat += (targetWin - currentWindowFloat) * 0.25f;
        if (currentWindowFloat > maxWin)
            currentWindowFloat = maxWin;
        else if (currentWindowFloat < 0.0f)
            currentWindowFloat = 0.0f;

        currentWindow = (uint32_t)currentWindowFloat;
        float zFrac = currentWindowFloat - (float)currentWindow;
        if (zFrac < 0.0005f)
        {
            zFrac = 0.0f;
        }
        else if (zFrac > 0.9995f)
        {
            zFrac = 0.0f;
            if (currentWindow < (uint32_t)maxWin)
                currentWindow++;
        }

        waveTablePosition = wavetableWindowSize * currentWindow;

        uint32_t maxlength = (length >= wavetableWindowSize)
                               ? (length - (length % wavetableWindowSize) - wavetableWindowSize)
                               : 0;

        if (waveTablePosition > maxlength)
        {
            waveTablePosition = maxlength;
        }

        int16_t* inA = (int16_t*)next + waveTablePosition;
        int16_t* inB = nullptr;

        if (zFrac > 0.0f && totalWindows > 1 && currentWindow < (uint32_t)maxWin)
        {
            uint32_t nextTablePos = wavetableWindowSize * (currentWindow + 1);
            if (nextTablePos > maxlength)
                nextTablePos = maxlength;
            if (nextTablePos != waveTablePosition)
                inB = (int16_t*)next + nextTablePos;
            else
                zFrac = 0.0f;
        }
        else
        {
            zFrac = 0.0f;
        }

        const int32_t zFrac14 = (int32_t)(zFrac * 16384.0f); // 14-bit fixed point lerp

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
            uint32_t nextSampleIdx = iPitchCounter + 1;
            if (nextSampleIdx >= wavetableWindowSize)
            {
                nextSampleIdx = 0;
            }

            int32_t currentSampleValue = *(inA + iPitchCounter);
            int32_t interpolationDifA = *(inA + nextSampleIdx) - currentSampleValue;

            int32_t sampA = currentSampleValue +
                            (int32_t)(((currentFractionPitchCounter >> 2) * interpolationDifA) >> 14);

            int32_t outSample = sampA;

            if (zFrac14 > 0 && inB != nullptr)
            {
                int32_t sampleBValue = *(inB + iPitchCounter);
                int32_t interpolationDifB = *(inB + nextSampleIdx) - sampleBValue;
                int32_t sampB = sampleBValue +
                                (int32_t)(((currentFractionPitchCounter >> 2) * interpolationDifB) >> 14);

                outSample = sampA + (((sampB - sampA) * zFrac14) >> 14);
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
        next = currentStartAddress + pointsInSamples.start;
        fPitchCounter = (float)currentFractionPitchCounter / MAX_16BIT;

        return block;
    }
}
