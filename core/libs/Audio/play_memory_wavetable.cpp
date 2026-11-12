#include "Audio.h"
#include "play_memory.h"
#include "utility/dspinst.h"

namespace
{
inline float cubicHermiteSample(const int16_t* table,
                                uint32_t idx0,
                                uint32_t idx1,
                                uint32_t idx2,
                                uint32_t idx3,
                                float t)
{
    const float ym1 = (float)table[idx0];
    const float y0 = (float)table[idx1];
    const float y1 = (float)table[idx2];
    const float y2 = (float)table[idx3];

    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

    return ((c3 * t + c2) * t + c1) * t + y0;
}
} // namespace

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

        float w = prevWindowFloat;
        const float wStep = (currentWindowFloat - prevWindowFloat) * (1.0f / AUDIO_BLOCK_SAMPLES);
        const bool zIsRamping = (wStep != 0.0f) && (totalWindows > 1);

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

        int16_t* steadyInA = (int16_t*)next + steadyPosA;
        int16_t* steadyInB = nullptr;

        if (steadyZFrac > 0.0f && totalWindows > 1 && currentWindow < maxWinIdx)
        {
            uint32_t nextTablePos = wavetableWindowSize * (currentWindow + 1);
            if (nextTablePos > maxlength)
                nextTablePos = maxlength;
            if (nextTablePos != steadyPosA)
                steadyInB = (int16_t*)next + nextTablePos;
            else
                steadyZFrac = 0.0f;
        }
        else
        {
            steadyZFrac = 0.0f;
        }

        waveTablePosition = steadyPosA;

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
            // Cyclic 4-point sample indices (seam-safe wrap within the current single-cycle)
            const uint32_t idx1 = (uint32_t)iPitchCounter;
            const uint32_t idx0 = (idx1 == 0) ? (wavetableWindowSize - 1) : (idx1 - 1);
            uint32_t idx2 = idx1 + 1;
            if (idx2 >= wavetableWindowSize)
                idx2 = 0;
            uint32_t idx3 = idx2 + 1;
            if (idx3 >= wavetableWindowSize)
                idx3 = 0;

            int16_t* inA = steadyInA;
            int16_t* inB = steadyInB;
            float zFrac = steadyZFrac;

            if (zIsRamping)
            {
                w += wStep;
                float wClamped = w;
                if (wClamped > maxWin)
                    wClamped = maxWin;
                else if (wClamped < 0.0f)
                    wClamped = 0.0f;

                const uint32_t winA = (uint32_t)wClamped;
                zFrac = wClamped - (float)winA;

                uint32_t posA = wavetableWindowSize * winA;
                if (posA > maxlength)
                    posA = maxlength;
                inA = (int16_t*)next + posA;
                inB = nullptr;

                if (zFrac > 0.0f && winA < maxWinIdx)
                {
                    uint32_t posB = wavetableWindowSize * (winA + 1);
                    if (posB > maxlength)
                        posB = maxlength;
                    if (posB != posA)
                        inB = (int16_t*)next + posB;
                    else
                        zFrac = 0.0f;
                }
                else
                {
                    zFrac = 0.0f;
                }
            }

            const float t = (float)currentFractionPitchCounter * (1.0f / 65536.0f);
            float outFloat = cubicHermiteSample(inA, idx0, idx1, idx2, idx3, t);

            if (zFrac > 0.0f && inB != nullptr)
            {
                const float sampB = cubicHermiteSample(inB, idx0, idx1, idx2, idx3, t);
                outFloat += (sampB - outFloat) * zFrac;
            }

            int32_t outSample = (int32_t)outFloat;
            if (outSample > 32767)
                outSample = 32767;
            else if (outSample < -32768)
                outSample = -32768;

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
