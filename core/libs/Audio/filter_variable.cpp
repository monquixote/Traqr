/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop
 * open source software by purchasing Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "filter_variable.h"
#include "utility/dspinst.h"
#include <cmath>
#include <cr_section_macros.h>
#include <stdint.h>

// State Variable Filter (Chamberlin) with 2X oversampling
// http://www.musicdsp.org/showArchiveComment.php?ArchiveID=92

// The fast 32x32 with rshift32 discards 2 bits, which probably
// never matter.
//#define MULT(a, b) (int32_t)(((int64_t)(a) * (b)) >> 30)
#define MULT(a, b) (multiply_32x32_rshift32_rounded(a, b) << 2)

// It's very unlikely anyone could hear any difference, but if you
// care deeply about numerical precision in seldom-used cases,
// uncomment this to improve the control signal accuracy
//#define IMPROVE_HIGH_FREQUENCY_ACCURACY

// This increases the exponential approximation accuracy from
// about 0.341% error to only 0.012% error, which probably makes
// no audible difference.

bool AudioFilterStateVariable::isCutoffInitialize = false;
float AudioFilterStateVariable::filterFreq[kFrequencyLookupSize];
int32_t AudioFilterStateVariable::filterFmult[kFrequencyLookupSize];
uint8_t AudioFilterStateVariable::sFilterModel = AudioFilterStateVariable::kFilterModelCleanSVF;
uint8_t AudioFilterStateVariable::sFilterSlope = AudioFilterStateVariable::kFilterSlope12dB;
uint8_t AudioFilterStateVariable::sOverdriveCurve = AudioFilterStateVariable::kOverdriveCurveModernSoft;

static inline float fastTanh(float x)
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

AUDIO_CORE_FLASH void AudioFilterStateVariable::setFilterModel(uint8_t model)
{
    if (model >= kFilterModelCount)
    {
        model = kFilterModelCleanSVF;
    }
    sFilterModel = model;
}

AUDIO_CORE_FLASH void AudioFilterStateVariable::setFilterSlope(uint8_t slope)
{
    if (slope >= kFilterSlopeCount)
    {
        slope = kFilterSlope12dB;
    }
    sFilterSlope = slope;
}

AUDIO_CORE_FLASH void AudioFilterStateVariable::setOverdriveCurve(uint8_t curve)
{
    if (curve >= kOverdriveCurveCount)
    {
        curve = kOverdriveCurveModernSoft;
    }
    sOverdriveCurve = curve;
}

AUDIO_CORE_FLASH void AudioFilterStateVariable::initCutoffLookup()
{
    for (int idx = 0; idx < kFrequencyLookupSize; idx++)
    {
        float freq = 20.0f * powf(2.0f, idx / 50.0f);
        float clampedFreq = clamp<float>(freq, 20.0f, AUDIO_SAMPLE_RATE_EXACT / 2.5f);
        filterFreq[idx] = tanf(clampedFreq * (3.141592654f / AUDIO_SAMPLE_RATE_EXACT));
        filterFmult[idx] =
          2.0f *
          sinf(clampedFreq * (3.141592654f / (AUDIO_SAMPLE_RATE_EXACT * kOversamplingMult))) *
          kUnityValue;
    }

    isCutoffInitialize = true;
}

AUDIO_CORE_RAM void AudioFilterStateVariable::update_overdrive(const int16_t* in, int16_t* out)
{
    const int16_t* end = in + AUDIO_BLOCK_SAMPLES;

    if (overdriveGain != 0)
    {
        do
        {
            const int32_t in_val = *in++ * overdriveGain / kOverdriveGainMult;
            const int32_t in_abs = in_val >= 0 ? in_val : -in_val;
            const int32_t in_sign = in_val >= 0 ? INT16_MAX : INT16_MIN;
            int16_t out_val;

            if (in_abs > (int32_t)INT16_MAX / 2)
            {
                // y = 1
                out_val = in_sign;
            }
            else if (in_abs > INT16_MAX / 4)
            {
                // y = (4 - (2 - 4x)^2) / 4
                // exponent of 3 gives a curve better aligned to 3x, but at a cost of an additional
                // mult
                int32_t temp = 2 * (int32_t)INT16_MAX - 4 * (int32_t)in_abs;
                out_val = (4 * (int32_t)INT16_MAX - temp * temp / 0x8000) / 4;
                if (in_sign < 0)
                {
                    out_val = -out_val;
                }
            }
            else
            {
                // y = 3x
                out_val = 3 * in_val;
            }

            *out++ = out_val;
        } while (in < end);
    }
}

AUDIO_CORE_FLASH void AudioFilterStateVariable::stabilize()
{
    static constexpr int kStabilizeBufferSize = 8;
    static int16_t buffer[kStabilizeBufferSize];

    lastupdate_fmult = setting_fmult;
    lastupdate_damp = setting_damp;
    lastupdate_g = setting_g;
    lastupdate_k = setting_k;
    lastupdate_res01 = setting_res01;

    state_lowpass = 0;
    state_bandpass = 0;
    svf_s1 = 0.0f;
    svf_s2 = 0.0f;
    svf_s1_b = 0.0f;
    svf_s2_b = 0.0f;
    od_state1 = 0.0f;
    od_state2 = 0.0f;
    ladder_s[0] = 0.0f;
    ladder_s[1] = 0.0f;
    ladder_s[2] = 0.0f;
    ladder_s[3] = 0.0f;

    for (int sample = 0; sample < kStabilizeBufferSize; sample++)
    {
        buffer[sample] = state_inputprev >> kInputShift;
    }
    if (sFilterModel == kFilterModelLegacy)
    {
        update_fixed(buffer, buffer, kStabilizeBufferSize);
    }
    else
    {
        update_float(buffer, buffer, kStabilizeBufferSize);
    }
}

AUDIO_CORE_RAM void AudioFilterStateVariable::update_float(const int16_t* in,
                                                           int16_t* out,
                                                           int samples)
{
    const int16_t* end = in + samples;
    const float invSamples = 1.0f / (float)samples;

    float g = lastupdate_g;
    const float delta_g = (setting_g - g) * invSamples;
    float k = lastupdate_k;
    const float delta_k = (setting_k - k) * invSamples;
    float res01 = lastupdate_res01;
    const float delta_res01 = (setting_res01 - res01) * invSamples;

    const bool hasDrive = (overdriveGain != 0);
    const uint8_t odCurve = sOverdriveCurve;
    const float drive = hasDrive ? (1.0f + (float)overdriveGain * (7.0f / 928.0f)) : 1.0f;
    const float driveMakeup = hasDrive ? (0.65f / fastTanh(0.65f * drive)) : 1.0f;
    const float classicDriveMult = (float)overdriveGain * (1.0f / (float)kOverdriveGainMult);

    float od1 = od_state1;
    float od2 = od_state2;

    auto applyOverdrive = [&](float in_val) -> float {
        if (!hasDrive)
            return in_val;
        if (odCurve == kOverdriveCurveModernSoft)
        {
            return fastTanh(in_val * drive) * driveMakeup;
        }
        else if (odCurve == kOverdriveCurveClassicHard)
        {
            float v = in_val * classicDriveMult;
            float abs_v = fabsf(v);
            float sgn = (v >= 0.0f) ? 1.0f : -1.0f;
            if (abs_v > 0.5f)
            {
                return sgn;
            }
            else if (abs_v > 0.25f)
            {
                float temp = 2.0f - 4.0f * abs_v;
                return sgn * (1.0f - 0.25f * temp * temp);
            }
            else
            {
                return 3.0f * v;
            }
        }
        else if (odCurve == kOverdriveCurveTubeValve)
        {
            // Asymmetric triode saturation with rich 2nd-order even harmonics
            float v = in_val * drive;
            float sat;
            if (v >= 0.0f)
                sat = fastTanh(v * 1.25f) * (1.0f / 1.25f);
            else
                sat = fastTanh(v * 0.85f) * (1.0f / 0.85f);
            // 1-pole high-pass DC bias removal
            od1 += 0.005f * (sat - od1);
            return (sat - od1) * driveMakeup;
        }
        else if (odCurve == kOverdriveCurveTape)
        {
            // Symmetric tape saturation with dynamic high-frequency tape head loss
            float v = in_val * drive;
            float sat = fastTanh(v);
            float alpha = (drive > 7.0f) ? 0.35f : (0.05f * drive);
            od1 += alpha * (sat - od1);
            return (0.70f * sat + 0.30f * od1) * driveMakeup;
        }
        else // kOverdriveCurveTransformer
        {
            // Low-frequency emphasized iron-core saturation with magnetic hysteresis
            od1 += 0.035f * (in_val - od1);
            float v = (in_val + 1.2f * od1) * drive - 0.18f * od2;
            float sat = fastTanh(v);
            od2 = sat;
            float outTrans = sat - 0.30f * od1;
            return outTrans * driveMakeup;
        }
    };

    const uint8_t type = filterType;
    const uint8_t model = sFilterModel;
    const uint8_t slope = sFilterSlope;

    if (!isEnabled)
    {
        do
        {
            float x = (float)(*in++) * (1.0f / 32768.0f);
            x = applyOverdrive(x);
            int32_t outVal = (int32_t)(x * 32767.0f);
            if (outVal > 32767)
                outVal = 32767;
            else if (outVal < -32768)
                outVal = -32768;
            *out++ = (int16_t)outVal;
        } while (in < end);

        od_state1 = od1;
        od_state2 = od2;
        lastupdate_g = setting_g;
        lastupdate_k = setting_k;
        lastupdate_res01 = setting_res01;
        return;
    }

    if (model == kFilterModelLadder24)
    {
        float l0 = ladder_s[0];
        float l1 = ladder_s[1];
        float l2 = ladder_s[2];
        float l3 = ladder_s[3];

        do
        {
            float x = (float)(*in++) * (1.0f / 32768.0f);
            x = applyOverdrive(x);

            const float G = g / (1.0f + g);
            const float G2 = G * G;
            const float G4 = G2 * G2;
            // Smooth musical resonance taper: warm at 0-50%, singing at 50-80%, squelch at 80-100%
            const float kLad = 3.82f * (0.35f * res01 + 0.65f * res01 * res01);

            // Exact ZDF 4-pole ladder state calculation: S = (1 - G) * (l3 + G * (l2 + G * (l1 + G * l0)))
            const float S = (1.0f - G) * (l3 + G * (l2 + G * (l1 + G * l0)));
            // True zero-delay closed-loop feedback with analog differential saturation:
            const float u = fastTanh((x - kLad * S) / (1.0f + kLad * G4));

            const float v0 = G * (u - l0);
            const float y0 = v0 + l0;
            l0 = clamp<float>(y0 + v0, -4.0f, 4.0f);

            const float v1 = G * (y0 - l1);
            const float y1 = v1 + l1;
            l1 = clamp<float>(y1 + v1, -4.0f, 4.0f);

            const float v2 = G * (y1 - l2);
            const float y2 = v2 + l2;
            l2 = clamp<float>(y2 + v2, -4.0f, 4.0f);

            const float v3 = G * (y2 - l3);
            const float y3 = v3 + l3;
            l3 = clamp<float>(y3 + v3, -4.0f, 4.0f);

            float y;
            if (slope == kFilterSlope12dB)
            {
                if (type == kFilterTypeLowPass)
                {
                    y = y1 * (1.0f + 0.35f * res01);
                }
                else if (type == kFilterTypeBandPass)
                {
                    y = 2.0f * (y0 - y1);
                }
                else if (type == kFilterTypeHighPass)
                {
                    y = u - 2.0f * y0 + y1;
                }
                else
                {
                    y = 0.0f;
                }
            }
            else // kFilterSlope24dB
            {
                if (type == kFilterTypeLowPass)
                {
                    y = y3 * (1.0f + 0.5f * res01);
                }
                else if (type == kFilterTypeBandPass)
                {
                    y = 2.0f * (y1 - y2);
                }
                else if (type == kFilterTypeHighPass)
                {
                    y = u - 4.0f * y0 + 6.0f * y1 - 4.0f * y2 + y3;
                }
                else
                {
                    y = 0.0f;
                }
            }

            int32_t outVal = (int32_t)(y * 32767.0f);
            if (outVal > 32767)
                outVal = 32767;
            else if (outVal < -32768)
                outVal = -32768;
            *out++ = (int16_t)outVal;

            g += delta_g;
            res01 += delta_res01;
        } while (in < end);

        ladder_s[0] = l0;
        ladder_s[1] = l1;
        ladder_s[2] = l2;
        ladder_s[3] = l3;
    }
    else if (model == kFilterModelWarmSVF)
    {
        float s1 = svf_s1;
        float s2 = svf_s2;
        float s1_b = svf_s1_b;
        float s2_b = svf_s2_b;

        do
        {
            float x = (float)(*in++) * (1.0f / 32768.0f);
            x = applyOverdrive(x);

            float y;
            if (slope == kFilterSlope12dB)
            {
                const float xWarm = fastTanh(x * 1.35f);
                const float s1Sat = fastTanh(s1);

                const float a1 = 1.0f / (1.0f + g * (g + k));
                const float a2 = g * a1;
                const float a3 = g * a2;

                const float v3 = xWarm - s2;
                const float v1 = a1 * s1Sat + a2 * v3;
                const float v2 = s2 + a2 * s1Sat + a3 * v3;
                s1 = 2.0f * v1 - s1Sat;
                s2 = 2.0f * v2 - s2;

                if (type == kFilterTypeLowPass)
                {
                    y = v2;
                }
                else if (type == kFilterTypeBandPass)
                {
                    y = v1;
                }
                else if (type == kFilterTypeHighPass)
                {
                    y = xWarm - k * v1 - v2;
                }
                else
                {
                    y = 0.0f;
                }

                y *= (0.85f + 0.25f * res01);
            }
            else // 24dB cascade
            {
                const float k_casc = sqrtf(k * 1.414f);
                const float a1 = 1.0f / (1.0f + g * (g + k_casc));
                const float a2 = g * a1;
                const float a3 = g * a2;

                // Stage 1
                const float xWarm = fastTanh(x * 1.35f);
                const float s1Sat = fastTanh(s1);
                const float v3_1 = xWarm - s2;
                const float v1_1 = a1 * s1Sat + a2 * v3_1;
                const float v2_1 = s2 + a2 * s1Sat + a3 * v3_1;
                s1 = 2.0f * v1_1 - s1Sat;
                s2 = 2.0f * v2_1 - s2;

                float y1;
                if (type == kFilterTypeLowPass)
                    y1 = v2_1;
                else if (type == kFilterTypeBandPass)
                    y1 = v1_1;
                else
                    y1 = xWarm - k_casc * v1_1 - v2_1;

                // Stage 2
                const float y1Warm = fastTanh(y1 * 1.25f);
                const float s1Sat_b = fastTanh(s1_b);
                const float v3_2 = y1Warm - s2_b;
                const float v1_2 = a1 * s1Sat_b + a2 * v3_2;
                const float v2_2 = s2_b + a2 * s1Sat_b + a3 * v3_2;
                s1_b = 2.0f * v1_2 - s1Sat_b;
                s2_b = 2.0f * v2_2 - s2_b;

                if (type == kFilterTypeLowPass)
                    y = v2_2;
                else if (type == kFilterTypeBandPass)
                    y = v1_2;
                else
                    y = y1Warm - k_casc * v1_2 - v2_2;

                y *= (0.90f + 0.20f * res01);
            }

            int32_t outVal = (int32_t)(y * 32767.0f);
            if (outVal > 32767)
                outVal = 32767;
            else if (outVal < -32768)
                outVal = -32768;
            *out++ = (int16_t)outVal;

            g += delta_g;
            k += delta_k;
            res01 += delta_res01;
        } while (in < end);

        svf_s1 = s1;
        svf_s2 = s2;
        svf_s1_b = s1_b;
        svf_s2_b = s2_b;
    }
    else // kFilterModelCleanSVF
    {
        float s1 = svf_s1;
        float s2 = svf_s2;
        float s1_b = svf_s1_b;
        float s2_b = svf_s2_b;

        do
        {
            float x = (float)(*in++) * (1.0f / 32768.0f);
            x = applyOverdrive(x);

            float y;
            if (slope == kFilterSlope12dB)
            {
                const float a1 = 1.0f / (1.0f + g * (g + k));
                const float a2 = g * a1;
                const float a3 = g * a2;

                const float v3 = x - s2;
                const float v1 = a1 * s1 + a2 * v3;
                const float v2 = s2 + a2 * s1 + a3 * v3;
                s1 = 2.0f * v1 - s1;
                s2 = 2.0f * v2 - s2;

                if (type == kFilterTypeLowPass)
                {
                    y = v2;
                }
                else if (type == kFilterTypeBandPass)
                {
                    y = v1;
                }
                else if (type == kFilterTypeHighPass)
                {
                    y = x - k * v1 - v2;
                }
                else
                {
                    y = 0.0f;
                }
            }
            else // 24dB cascade
            {
                const float k_casc = sqrtf(k * 1.414f);
                const float a1 = 1.0f / (1.0f + g * (g + k_casc));
                const float a2 = g * a1;
                const float a3 = g * a2;

                // Stage 1
                const float v3_1 = x - s2;
                const float v1_1 = a1 * s1 + a2 * v3_1;
                const float v2_1 = s2 + a2 * s1 + a3 * v3_1;
                s1 = 2.0f * v1_1 - s1;
                s2 = 2.0f * v2_1 - s2;

                float y1;
                if (type == kFilterTypeLowPass)
                    y1 = v2_1;
                else if (type == kFilterTypeBandPass)
                    y1 = v1_1;
                else
                    y1 = x - k_casc * v1_1 - v2_1;

                // Stage 2
                const float v3_2 = y1 - s2_b;
                const float v1_2 = a1 * s1_b + a2 * v3_2;
                const float v2_2 = s2_b + a2 * s1_b + a3 * v3_2;
                s1_b = 2.0f * v1_2 - s1_b;
                s2_b = 2.0f * v2_2 - s2_b;

                if (type == kFilterTypeLowPass)
                    y = v2_2;
                else if (type == kFilterTypeBandPass)
                    y = v1_2;
                else
                    y = y1 - k_casc * v1_2 - v2_2;
            }

            int32_t outVal = (int32_t)(y * 32767.0f);
            if (outVal > 32767)
                outVal = 32767;
            else if (outVal < -32768)
                outVal = -32768;
            *out++ = (int16_t)outVal;

            g += delta_g;
            k += delta_k;
            res01 += delta_res01;
        } while (in < end);

        svf_s1 = s1;
        svf_s2 = s2;
        svf_s1_b = s1_b;
        svf_s2_b = s2_b;
    }

    od_state1 = od1;
    od_state2 = od2;
    lastupdate_g = setting_g;
    lastupdate_k = setting_k;
    lastupdate_res01 = setting_res01;
    state_inputprev = (int32_t)out[samples - 1] << kInputShift;
}

AUDIO_CORE_RAM void AudioFilterStateVariable::update_fixed(const int16_t* in, int16_t* out, int samples)
{
    const int16_t* end = in + samples;
    int32_t input, inputprev;
    int32_t lowpass, bandpass, highpass;
    int32_t lowpasstmp, bandpasstmp, highpasstmp;
    int32_t fmult, damp;
    int32_t delta_fmult, delta_damp;

    fmult = lastupdate_fmult;
    damp = lastupdate_damp;
    delta_fmult = (setting_fmult - fmult) / samples;
    delta_damp = (setting_damp - damp) / samples;
    inputprev = state_inputprev;
    lowpass = state_lowpass;
    bandpass = state_bandpass;

    const uint8_t type = filterType;

    do
    {
        input = (*in++) << kInputShift;
        lowpass = lowpass + MULT(fmult, bandpass);
        highpass = ((input + inputprev) >> 1) - lowpass - MULT(damp, bandpass);
        inputprev = input;
        bandpass = bandpass + MULT(fmult, highpass);
        lowpasstmp = lowpass;
        bandpasstmp = bandpass;
        highpasstmp = highpass;
        lowpass = lowpass + MULT(fmult, bandpass);
        highpass = input - lowpass - MULT(damp, bandpass);
        bandpass = bandpass + MULT(fmult, highpass);

        int32_t passSum;
        if (type == kFilterTypeLowPass)
        {
            passSum = lowpass + lowpasstmp;
        }
        else if (type == kFilterTypeBandPass)
        {
            passSum = bandpass + bandpasstmp;
        }
        else if (type == kFilterTypeHighPass)
        {
            passSum = highpass + highpasstmp;
        }
        else
        {
            passSum = 0;
        }

        *out++ = signed_saturate_rshift(passSum, 16, kInputShift + 1);

        fmult += delta_fmult;
        damp += delta_damp;
    } while (in < end);
    state_inputprev = inputprev;
    state_lowpass = lowpass;
    state_bandpass = bandpass;
    lastupdate_fmult = setting_fmult;
    lastupdate_damp = setting_damp;
}

AUDIO_CORE_RAM void AudioFilterStateVariable::update(void)
{
    audio_block_t* input_block = nullptr;
    audio_block_t* out_block = nullptr;
    audio_block_t* filter_block = nullptr;

    input_block = receiveReadOnly(0);
    if (nullptr == input_block)
    {
        return;
    }

    if (!isEnabled && overdriveGain == 0 && isEnabled == lastEnabled && filterType == lastFilterType)
    {
        int32_t lastSample = (int32_t)input_block->data[AUDIO_BLOCK_SAMPLES - 1];
        state_inputprev = lastSample << kInputShift;
        state_outputprev = lastSample;
        lastupdate_g = setting_g;
        lastupdate_k = setting_k;
        lastupdate_res01 = setting_res01;
        svf_s1 = 0.0f;
        svf_s2 = 0.0f;
        svf_s1_b = 0.0f;
        svf_s2_b = 0.0f;
        od_state1 = 0.0f;
        od_state2 = 0.0f;
        ladder_s[0] = 0.0f;
        ladder_s[1] = 0.0f;
        ladder_s[2] = 0.0f;
        ladder_s[3] = 0.0f;
        transmit(input_block, 0);
        release(input_block);
        return;
    }

    out_block = allocate();
    if (nullptr == out_block)
    {
        release(input_block);
        return;
    }

    if (sFilterModel != lastModel)
    {
        stabilize();
    }

    if (sFilterModel == kFilterModelLegacy)
    {
        if (overdriveGain != 0)
        {
            update_overdrive(input_block->data, out_block->data);
            filter_block = out_block;
        }
        else
        {
            filter_block = input_block;
        }

        if (isEnabled)
        {
            update_fixed(filter_block->data, out_block->data, AUDIO_BLOCK_SAMPLES);
        }
        else if (filter_block == input_block)
        {
            memcpy(out_block->data, input_block->data, sizeof(out_block->data));
            state_inputprev = (int32_t)out_block->data[AUDIO_BLOCK_SAMPLES - 1] << kInputShift;
        }
    }
    else
    {
        if (isEnabled || overdriveGain != 0)
        {
            update_float(input_block->data, out_block->data, AUDIO_BLOCK_SAMPLES);
        }
        else
        {
            memcpy(out_block->data, input_block->data, sizeof(out_block->data));
            state_inputprev = (int32_t)out_block->data[AUDIO_BLOCK_SAMPLES - 1] << kInputShift;
            lastupdate_g = setting_g;
            lastupdate_k = setting_k;
            lastupdate_res01 = setting_res01;
        }
    }

    if ((isEnabled != lastEnabled) || (filterType != lastFilterType) || (sFilterModel != lastModel) ||
        (sFilterSlope != lastSlope))
    {
        const int32_t kBlockSamplesShift = log2f(AUDIO_BLOCK_SAMPLES);
        for (int32_t sampleIdx = 0; sampleIdx < AUDIO_BLOCK_SAMPLES; sampleIdx++)
        {
            out_block->data[sampleIdx] =
              (int16_t)(((int32_t)out_block->data[sampleIdx] * (sampleIdx + 1) +
                         state_outputprev * (AUDIO_BLOCK_SAMPLES - (sampleIdx + 1))) >>
                        kBlockSamplesShift);
        }

        lastEnabled = isEnabled;
        lastFilterType = filterType;
        lastModel = sFilterModel;
        lastSlope = sFilterSlope;
    }

    state_outputprev = (int32_t)out_block->data[AUDIO_BLOCK_SAMPLES - 1];

    release(input_block);
    transmit(out_block, 0);
    release(out_block);
    return;
}
