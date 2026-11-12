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
    int32_t castPitchControl = (int32_t)pitchControl;
    float pitchFraction = pitchControl - (int32_t)pitchControl;

    int32_t currentFractionPitchCounter = fPitchCounter * MAX_16BIT;
    int32_t currentFractionPitchControl = pitchFraction * MAX_16BIT;

    const uint8_t subWave = mtProject.instrument[currentInstrIdx].wtSubWave;
    uint8_t subLevel = mtProject.instrument[currentInstrIdx].wtSubLevel;
    if (subLevel > 100)
        subLevel = 100;

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

            const uint32_t idx1 = (uint32_t)iPitchCounter;
            uint32_t idx2 = idx1 + 1;
            if (idx2 >= wavetableWindowSize)
                idx2 = 0;

            const int32_t currentSampleValue = in[idx1];
            const int32_t interpolationDif = in[idx2] - currentSampleValue;

            *out++ = (int16_t)(currentSampleValue +
                               (int32_t)(((currentFractionPitchCounter >> 2) * interpolationDif) >> 14));

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

            const uint32_t idx1 = (uint32_t)iPitchCounter;
            uint32_t idx2 = idx1 + 1;
            if (idx2 >= wavetableWindowSize)
                idx2 = 0;

            const int32_t currentSampleValue = in[idx1];
            const int32_t interpolationDif = in[idx2] - currentSampleValue;

            const int32_t wtSample = currentSampleValue +
                                     (int32_t)(((currentFractionPitchCounter >> 2) * interpolationDif) >> 14);

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

    next = currentStartAddress + pointsInSamples.start;
    fPitchCounter = (float)currentFractionPitchCounter / MAX_16BIT;

    return block;
}
