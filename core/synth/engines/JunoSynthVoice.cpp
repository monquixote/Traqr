#include "config.h"
#ifdef FEATURE_SYNTH

#include "JunoSynthVoice.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"
#include "cr_section_macros.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace polysynth
{

namespace
{

enum class JunoParamId : int
{
    // DCO
    kOctave = 0,
    kTune,
    kSawLevel,
    kPulseLevel,
    kPulseWidth,
    kPwmDepth,
    kPwmSource,
    kSubLevel,
    kSubOctave,
    kNoiseLevel,
    kDrift,

    // Filter
    kHpf,
    kCutoff,
    kResonance,
    kFilterEnv,
    kFilterLfo,
    kFilterKeytrack,
    kOtaDrive,

    // Filter Envelope
    kFenvAttack,
    kFenvDecay,
    kFenvSustain,
    kFenvRelease,

    // Amp Envelope
    kAenvAttack,
    kAenvDecay,
    kAenvSustain,
    kAenvRelease,
    kVcaMode,

    // LFO
    kLfoRate,
    kLfoDelay,
    kLfoPitch,

    // Chorus / Master
    kChorusMode,
    kVolume
};

constexpr int kCtl = 16;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr int kChorusBuf = 2048;

// Static buffer placed in external SDRAM (takes 0 bytes of precious SRAM_DTC heap)
__NOINIT(EXTERNAL_RAM) static float s_junoChorusBuf[kChorusBuf];

// Division-free PolyBLEP using precomputed invDt (1.0f / dt)
inline float polyBLEP(float t, float dt, float invDt)
{
    if (t < dt)
    {
        const float r = t * invDt;
        return r * (2.0f - r) - 1.0f;
    }
    else if (t > 1.0f - dt)
    {
        const float r = (t - 1.0f) * invDt;
        return r * (r + 2.0f) + 1.0f;
    }
    return 0.0f;
}

inline float fastTanh(float x)
{
    const float xc = std::clamp(x, -3.0f, 3.0f);
    const float x2 = xc * xc;
    return xc * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Fast rational approximation of tan(x) for x in [0, pi * 0.48]
inline float fastTan(float x)
{
    const float x2 = x * x;
    return x * (1.0f + x2 * (0.33333333f + x2 * (0.13333333f + x2 * 0.05396825f)));
}

inline float wrap01(float p)
{
    if (p >= 1.0f) p -= 1.0f;
    else if (p < 0.0f) p += 1.0f;
    return p;
}

inline float tri(float phase)
{
    float p = phase;
    if (p >= 1.0f) p -= 1.0f;
    else if (p < 0.0f) p += 1.0f;
    return 4.0f * std::fabs(p - 0.5f) - 1.0f;
}

inline float timeCoef(float seconds, float ratio, float sr)
{
    return 1.0f - std::exp(-ratio / (std::max(seconds, 0.0005f) * sr));
}

struct Adsr
{
    enum Stage : uint8_t { kIdle = 0, kAttack, kDecay, kSustain, kRelease };
    Stage stage = kIdle;
    float level = 0.0f;

    void gateOn() { stage = kAttack; }
    void gateOff()
    {
        if (stage != kIdle) stage = kRelease;
    }

    inline float process(float aCoef, float dCoef, float sustain, float rCoef)
    {
        switch (stage)
        {
            case kAttack:
                level += (1.3f - level) * aCoef;
                if (level >= 1.0f)
                {
                    level = 1.0f;
                    stage = kDecay;
                }
                break;
            case kDecay:
                level += (sustain - level) * dCoef;
                if (level - sustain < 0.0005f)
                {
                    level = sustain;
                    stage = kSustain;
                }
                break;
            case kSustain:
                level = sustain;
                break;
            case kRelease:
                level -= level * rCoef;
                if (level < 0.0001f)
                {
                    level = 0.0f;
                    stage = kIdle;
                }
                break;
            default:
                level = 0.0f;
                break;
        }
        return level;
    }
};

class JunoSynthVoice : public sdk::CustomSynthVoice<JunoSynthVoice, JunoParamId>
{
  public:
    int voiceIndex = 0;

    explicit JunoSynthVoice(int index = 0)
      : voiceIndex(index)
    {
        driftRnd = 0x9e3779b9u + voiceIndex * 0x1000193u;
        noiseRnd = 0x12345678u + voiceIndex * 0x0456123u;

        if (voiceIndex == 0)
        {
            std::fill_n(s_junoChorusBuf, kChorusBuf, 0.0f);
            chorusWrite = 0;
            chorusPhase = 0.0f;
            chorusLpL = 0.0f;
            chorusLpR = 0.0f;
            chorusLpL2 = 0.0f;
            chorusLpR2 = 0.0f;
            chorusDepthSm = 0.0f;
            chorusRateSm = 0.5f;
            chorusMix = 0.0f;
        }
    }

    bool isActive() override { return active; }

    void onNoteOn(const InstrumentVoiceContext& /*ctx*/,
                  note_t note,
                  float velocity,
                  bool /*isLegatoStep*/) override
    {
        currentNote = note;
        currentVelocity = velocity;
        gate = true;
        active = true;
        fenv.gateOn();
        aenv.gateOn();
        lfoFade = (lfoDelaySec <= 0.001f) ? 1.0f : 0.0f;
    }

    void onNoteOff(const InstrumentVoiceContext& /*ctx*/, note_t /*note*/) override
    {
        gate = false;
        fenv.gateOff();
        aenv.gateOff();
    }

    void renderVoiceAudio(const InstrumentVoiceContext& /*ctx*/,
                          [[maybe_unused]] std::optional<AudioBufferStereoRef> inputBuffer,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef> /*auxOutBuffer*/) override
    {
        auto outMono = outputBuffer[0];
        auto outRight = outputBuffer[1];

        if (!active)
        {
            std::fill(outMono.begin(), outMono.end(), 0.0f);
            std::fill(outRight.begin(), outRight.end(), 0.0f);
            return;
        }

        const float sr = sdk::kSampleRate;
        const float invSr = 1.0f / sr;
        const int n = static_cast<int>(outMono.size());

        const float subDiv = (subOctave > 0.5f) ? 0.25f : 0.5f;
        const float ladderK = resonance * 4.4f;
        const float inGain = 1.0f + otaDrive * 3.0f;
        const float driveComp = 1.0f / (1.0f + otaDrive * 1.2f);
        const float gateCoef = 1.0f - std::exp(-1.0f / (0.003f * sr));

        // HPF: 0 = Bass Boost (+3.5dB low shelf @ 70Hz), then 20Hz..1.2kHz
        const float boostMix = std::clamp(1.0f - hpfCutoff / 0.08f, 0.0f, 1.0f);
        const float shelfGainExtra = 0.496236f * boostMix; // std::pow(10.0f, 3.5f / 20.0f) - 1.0f
        const float shelfA = 1.0f - std::exp(-2.0f * kPi * 70.0f * invSr);
        const float hpHz = 20.0f * std::pow(60.0f, hpfCutoff);
        const float hpA = 1.0f - std::exp(-2.0f * kPi * hpHz * invSr);

        // Component tolerances for this voice
        const int vIdx = voiceIndex & 7;
        static constexpr float kTolPitch[8]   = { -0.72f, 0.45f, -0.28f, 0.81f, -0.63f, 0.36f, -0.15f, 0.58f };
        static constexpr float kTolCutoff[8]  = { -0.045f, 0.038f, -0.022f, 0.051f, -0.035f, 0.029f, -0.015f, 0.042f };
        static constexpr float kTolEnvTime[8] = { 0.035f, -0.042f, 0.021f, -0.031f, 0.048f, -0.025f, 0.018f, -0.039f };
        static constexpr float kTolPw[8]      = { -0.012f, 0.015f, -0.008f, 0.019f, -0.014f, 0.011f, -0.006f, 0.016f };

        // Precompute ADSR coefficients once per buffer (tf is constant for this voice/buffer)
        const float tf = 1.0f + voiceDrift * kTolEnvTime[vIdx] * 0.08f;
        const float aA = timeCoef(aenvAttackSec * tf, 1.466f, sr);
        const float aD = timeCoef(aenvDecaySec * tf, 4.6f, sr);
        const float aR = timeCoef(aenvReleaseSec * tf, 4.6f, sr);
        const float fA = timeCoef(fenvAttackSec * tf, 1.466f, sr);
        const float fD = timeCoef(fenvDecaySec * tf, 4.6f, sr);
        const float fR = timeCoef(fenvReleaseSec * tf, 4.6f, sr);
        const float velGain = 0.35f + 0.65f * currentVelocity;

        int done = 0;
        while (done < n)
        {
            if (!active)
            {
                std::fill(outMono.begin() + done, outMono.end(), 0.0f);
                std::fill(outRight.begin() + done, outRight.end(), 0.0f);
                return;
            }

            const int chunk = std::min(kCtl, n - done);

            // Control-rate updates
            lfoPhase = wrap01(lfoPhase + lfoRateHz * invSr * chunk);
            const float lfoVal = tri(lfoPhase);

            // Thermal wander leaky random walk
            const float wa = 0.0002f;
            driftRnd = driftRnd * 1664525u + 1013904223u;
            const float rnd1 = (static_cast<float>(driftRnd >> 8) / 8388608.0f) - 1.0f;
            driftPitch = std::clamp(driftPitch * (1.0f - wa) + rnd1 * wa * 90.0f, -1.0f, 1.0f);

            driftRnd = driftRnd * 1664525u + 1013904223u;
            const float rnd2 = (static_cast<float>(driftRnd >> 8) / 8388608.0f) - 1.0f;
            driftCutoff = std::clamp(driftCutoff * (1.0f - wa) + rnd2 * wa * 90.0f, -1.0f, 1.0f);

            if (lfoDelaySec > 0.001f && lfoFade < 1.0f)
            {
                lfoFade = std::min(1.0f, lfoFade + chunk * invSr / lfoDelaySec);
            }
            const float voiceLfo = lfoVal * lfoFade;

            // Pitch
            const float octShift = (octave - 1.0f) * 12.0f;
            float semis = static_cast<float>(currentNote) + octShift + tuneSemitones;
            semis += voiceDrift * (kTolPitch[vIdx] + driftPitch * 3.5f) * 0.01f;
            semis += voiceLfo * lfoPitchDepth * 1.5f;
            const float freq = std::clamp(440.0f * sdk::fastPow2((semis - 69.0f) * (1.0f / 12.0f)),
                                          8.0f, sr * 0.45f);
            const float dt = freq * invSr;
            const float invDt = (dt > 0.0f) ? (1.0f / dt) : 0.0f;
            const float subDt = dt * subDiv;
            const float invSubDt = (subDt > 0.0f) ? (1.0f / subDt) : 0.0f;

            // Pulse width
            float pwMod = 0.0f;
            if (pwmSource == 1) pwMod = lfoVal * pwmDepth * 0.45f;
            else if (pwmSource == 2) pwMod = fenv.level * pwmDepth * 0.45f;
            const float pw = std::clamp(pulseWidth + pwMod + voiceDrift * kTolPw[vIdx] * 0.015f, 0.05f, 0.95f);

            // Filter cutoff
            float cutHz = 20.0f * std::pow(900.0f, cutoffNorm);
            float oct = filterEnvDepth * fenv.level * 5.0f;
            oct += filterLfoDepth * voiceLfo * 1.5f;
            oct += filterKeytrack * (static_cast<float>(currentNote) - 60.0f) * (1.0f / 12.0f);
            oct += voiceDrift * (kTolCutoff[vIdx] * 0.08f + driftCutoff * 0.05f);
            cutHz = std::clamp(cutHz * sdk::fastPow2(oct), 20.0f, sr * 0.45f);
            const float g = fastTan(kPi * cutHz * invSr);
            const float G = g / (1.0f + g);
            const float oneMinusG = 1.0f - G;
            const float G4 = G * G * G * G;
            const float denomInv = 1.0f / (1.0f + ladderK * G4);

            // Audio-rate loop
            for (int i = 0; i < chunk; ++i)
            {
                fenv.process(fA, fD, fenvSustainLevel, fR);
                const float aenvLevel = aenv.process(aA, aD, aenvSustainLevel, aR);
                gateSlew += ((gate ? 1.0f : 0.0f) - gateSlew) * gateCoef;

                // DCO
                phase += dt;
                if (phase >= 1.0f) phase -= 1.0f;
                subPhase += subDt;
                if (subPhase >= 1.0f) subPhase -= 1.0f;

                float osc = 0.0f;
                float blepPhase = 0.0f;
                bool blepPhaseComputed = false;

                if (sawLevel > 0.0f)
                {
                    blepPhase = polyBLEP(phase, dt, invDt);
                    blepPhaseComputed = true;
                    float saw = 2.0f * phase - 1.0f - blepPhase;
                    osc += saw * sawLevel * 0.5f;
                }
                if (pulseLevel > 0.0f)
                {
                    if (!blepPhaseComputed)
                    {
                        blepPhase = polyBLEP(phase, dt, invDt);
                    }
                    float pShift = phase - pw + 1.0f;
                    if (pShift >= 1.0f) pShift -= 1.0f;
                    float p = (phase < pw) ? 1.0f : -1.0f;
                    p += blepPhase;
                    p -= polyBLEP(pShift, dt, invDt);
                    p -= (2.0f * pw - 1.0f);
                    osc += p * pulseLevel * 0.5f;
                }
                if (subLevel > 0.0f)
                {
                    float sq = (subPhase < 0.5f) ? 1.0f : -1.0f;
                    float subShift = subPhase + 0.5f;
                    if (subShift >= 1.0f) subShift -= 1.0f;
                    sq += polyBLEP(subPhase, subDt, invSubDt);
                    sq -= polyBLEP(subShift, subDt, invSubDt);
                    osc += sq * subLevel * 0.5f;
                }
                if (noiseLevel > 0.0f)
                {
                    noiseRnd = noiseRnd * 1664525u + 1013904223u;
                    const float nSamp = ((static_cast<float>(noiseRnd >> 8) / 8388608.0f) - 1.0f) * 0.4f;
                    osc += nSamp * noiseLevel;
                }

                // HPF
                shelfLp += shelfA * (osc - shelfLp);
                float x = osc + shelfGainExtra * shelfLp;
                hp1 += hpA * (x - hp1); x -= hp1;
                hp2 += hpA * (x - hp2); x -= hp2;

                // 4-pole OTA resonant ladder
                const float S = oneMinusG * (G * G * G * ladderS[0] + G * G * ladderS[1] + G * ladderS[2] + ladderS[3]);
                float u = (x * inGain - ladderK * S) * denomInv;
                u = fastTanh(u);
                float inSample = u;
                for (int k = 0; k < 4; ++k)
                {
                    const float vv = G * (inSample - ladderS[k]);
                    const float y = vv + ladderS[k];
                    ladderS[k] = y + vv;
                    inSample = y;
                }
                const float filtered = inSample * driveComp;

                // VCA
                const float amp = (vcaMode == 1) ? gateSlew : aenvLevel;
                const float voiceOut = filtered * amp * velGain * 0.8f;

                outMono[done + i] = voiceOut;
                outRight[done + i] = 0.0f;

                // Voice lifecycle
                if (vcaMode == 1)
                {
                    if (!gate && gateSlew < 0.0001f) active = false;
                }
                else
                {
                    if (!gate && aenv.stage == Adsr::kIdle) active = false;
                }

                if (!active)
                {
                    for (float& st : ladderS) st = 0.0f;
                }
            }

            done += chunk;
        }
    }

    void renderPostProcessAudio(const InstrumentVoiceContext& /*ctx*/,
                                AudioBufferStereoRef outputBuffer) override
    {
        if (voiceIndex != 0) return;

        auto outL = outputBuffer[0];
        auto outR = outputBuffer[1];
        const float sr = sdk::kSampleRate;
        const float invSr = 1.0f / sr;

        // Mode targets
        float chDepthTarget = 0.0f;
        float chRateTarget = 0.5f;
        if (chorusMode == 1)      { chDepthTarget = 0.0015f;  chRateTarget = 0.513f; }
        else if (chorusMode == 2) { chDepthTarget = 0.0021f;  chRateTarget = 0.863f; }
        else if (chorusMode == 3) { chDepthTarget = 0.00025f; chRateTarget = 9.75f;  }
        const float chMixTarget = (chorusMode > 0) ? 1.0f : 0.0f;

        const float chCenter = 0.0033f * sr;
        const float chLpA = 1.0f - std::exp(-2.0f * kPi * 8500.0f * invSr);

        // Chorus smoothing
        chorusDepthSm += (chDepthTarget - chorusDepthSm) * 0.05f;
        chorusRateSm += (chRateTarget - chorusRateSm) * 0.05f;
        chorusMix += (chMixTarget - chorusMix) * 0.05f;

        const float dryAmt = 1.0f - 0.4f * chorusMix;
        const float wetAmt = 0.6f * chorusMix;

        for (size_t i = 0; i < outL.size(); ++i)
        {
            const float mono = outL[i];

            if (chorusMix > 0.001f || chorusDepthSm > 0.00001f)
            {
                s_junoChorusBuf[chorusWrite] = mono;
                chorusPhase += chorusRateSm * invSr;
                if (chorusPhase >= 1.0f) chorusPhase -= 1.0f;

                const float modL = tri(chorusPhase);
                const float modR = -modL;

                const float dL = chCenter + modL * chorusDepthSm * sr;
                const float dR = chCenter + modR * chorusDepthSm * sr;

                auto readDelay = [&](float d) {
                    float rp = static_cast<float>(chorusWrite) - d;
                    while (rp < 0.0f) rp += kChorusBuf;
                    while (rp >= kChorusBuf) rp -= kChorusBuf;
                    const int i0 = static_cast<int>(rp);
                    const float fr = rp - i0;
                    const int i1 = (i0 + 1) & (kChorusBuf - 1);
                    return s_junoChorusBuf[i0] + fr * (s_junoChorusBuf[i1] - s_junoChorusBuf[i0]);
                };

                const float wetL = readDelay(dL);
                const float wetR = readDelay(dR);

                chorusLpL += chLpA * (wetL - chorusLpL);
                chorusLpL2 += chLpA * (chorusLpL - chorusLpL2);
                chorusLpR += chLpA * (wetR - chorusLpR);
                chorusLpR2 += chLpA * (chorusLpR - chorusLpR2);

                chorusWrite = (chorusWrite + 1) & (kChorusBuf - 1);

                const float l = mono * dryAmt + chorusLpL2 * wetAmt;
                const float r = mono * dryAmt + chorusLpR2 * wetAmt;

                outL[i] = fastTanh(l * volume * 1.2f);
                outR[i] = fastTanh(r * volume * 1.2f);
            }
            else
            {
                const float val = fastTanh(mono * volume * 1.2f);
                outL[i] = val;
                outR[i] = val;
            }
        }
    }

    static void createMetadata([[maybe_unused]] const Patch& patch, Patch::Metadata& metadata)
    {
        using namespace polysynth::sdk;

        metadata.params.resize(enumutil::EnumMax<JunoParamId>::value + 1);
        metadata.params.shrink_to_fit();

        if (paramHandlers.empty())
        {
            paramHandlers.resize(enumutil::EnumMax<JunoParamId>::value + 1);
        }

        ParamHelper<JunoSynthVoice> helper(metadata, paramHandlers);

        // --- 1. DCO SECTION ---
        helper.addParam(
          *JunoParamId::kOctave,
          {
            .displayName = "Range",
            .defaultValue = 1,
            .minValue = 0,
            .maxValue = 2,
            .isEnum = true,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t value) {
                constexpr std::array kOctNames = { "16'", "8'", "4'" };
                return std::string(kOctNames.at(std::clamp<int>(std::lround(value), 0, 2)));
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) {
              voice.octave = static_cast<float>(std::clamp<int>(std::lround(*val), 0, 2));
          });

        helper.addParam(
          *JunoParamId::kTune,
          {
            .displayName = "Tune",
            .defaultValue = 0.0f,
            .minValue = -12.0f,
            .maxValue = 12.0f,
            .valueFormatter = createSemitoneFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.tuneSemitones = *val; });

        helper.addParam(
          *JunoParamId::kSawLevel,
          {
            .displayName = "Saw Level",
            .defaultValue = 0.8f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.sawLevel = *val; });

        helper.addParam(
          *JunoParamId::kPulseLevel,
          {
            .displayName = "Pulse Level",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.pulseLevel = *val; });

        helper.addParam(
          *JunoParamId::kPulseWidth,
          {
            .displayName = "Pulse Width",
            .defaultValue = 0.5f,
            .minValue = 0.05f,
            .maxValue = 0.95f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.pulseWidth = *val; });

        helper.addParam(
          *JunoParamId::kPwmDepth,
          {
            .displayName = "PWM Depth",
            .defaultValue = 0.3f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.pwmDepth = *val; });

        helper.addParam(
          *JunoParamId::kPwmSource,
          {
            .displayName = "PWM Source",
            .defaultValue = 1,
            .minValue = 0,
            .maxValue = 2,
            .isEnum = true,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t value) {
                constexpr std::array kSrcNames = { "Manual", "LFO", "Env" };
                return std::string(kSrcNames.at(std::clamp<int>(std::lround(value), 0, 2)));
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) {
              voice.pwmSource = std::clamp<int>(std::lround(*val), 0, 2);
          });

        helper.addParam(
          *JunoParamId::kSubLevel,
          {
            .displayName = "Sub Level",
            .defaultValue = 0.2f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.subLevel = *val; });

        helper.addParam(
          *JunoParamId::kSubOctave,
          {
            .displayName = "Sub Octave",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = 1,
            .isEnum = true,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t value) {
                constexpr std::array kSubOctNames = { "-1 Oct", "-2 Oct" };
                return std::string(kSubOctNames.at(std::clamp<int>(std::lround(value), 0, 1)));
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) {
              voice.subOctave = static_cast<float>(std::clamp<int>(std::lround(*val), 0, 1));
          });

        helper.addParam(
          *JunoParamId::kNoiseLevel,
          {
            .displayName = "Noise",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.noiseLevel = *val; });

        helper.addParam(
          *JunoParamId::kDrift,
          {
            .displayName = "Voice Drift",
            .defaultValue = 0.35f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.voiceDrift = *val; });

        // --- 2. FILTER SECTION ---
        helper.addParam(
          *JunoParamId::kHpf,
          {
            .displayName = "HPF",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t value) {
                if (value <= 0.01f) return std::string("Bass Boost");
                const float hz = 20.0f * std::pow(60.0f, value);
                return fmt::format("{:.0f} Hz", hz);
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.hpfCutoff = *val; });

        helper.addParam(
          *JunoParamId::kCutoff,
          {
            .displayName = "Cutoff",
            .defaultValue = 0.62f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kFilterCutoff,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.cutoffNorm = *val; });

        helper.addParam(
          *JunoParamId::kResonance,
          {
            .displayName = "Resonance",
            .defaultValue = 0.15f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kFilterResonance,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.resonance = *val; });

        helper.addParam(
          *JunoParamId::kFilterEnv,
          {
            .displayName = "Env Amount",
            .defaultValue = 0.4f,
            .minValue = -1.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterEnvDepth = *val; });

        helper.addParam(
          *JunoParamId::kFilterLfo,
          {
            .displayName = "LFO Amount",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterLfoDepth = *val; });

        helper.addParam(
          *JunoParamId::kFilterKeytrack,
          {
            .displayName = "Key Track",
            .defaultValue = 0.5f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterKeytrack = *val; });

        helper.addParam(
          *JunoParamId::kOtaDrive,
          {
            .displayName = "OTA Drive",
            .defaultValue = 0.25f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.otaDrive = *val; });

        // --- 3. FILTER ENVELOPE SECTION ---
        helper.addParam(
          *JunoParamId::kFenvAttack,
          {
            .displayName = "Attack",
            .defaultValue = 0.005f,
            .minValue = 0.001f,
            .maxValue = 3.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kFilterEnvAttack,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.fenvAttackSec = *val; });

        helper.addParam(
          *JunoParamId::kFenvDecay,
          {
            .displayName = "Decay",
            .defaultValue = 0.6f,
            .minValue = 0.002f,
            .maxValue = 8.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kFilterEnvDecay,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.fenvDecaySec = *val; });

        helper.addParam(
          *JunoParamId::kFenvSustain,
          {
            .displayName = "Sustain",
            .defaultValue = 0.3f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kFilterEnvSustain,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.fenvSustainLevel = *val; });

        helper.addParam(
          *JunoParamId::kFenvRelease,
          {
            .displayName = "Release",
            .defaultValue = 0.4f,
            .minValue = 0.002f,
            .maxValue = 8.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kFilterEnvRelease,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.fenvReleaseSec = *val; });

        // --- 4. AMP ENVELOPE SECTION ---
        helper.addParam(
          *JunoParamId::kAenvAttack,
          {
            .displayName = "Attack",
            .defaultValue = 0.01f,
            .minValue = 0.001f,
            .maxValue = 3.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvAttack,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.aenvAttackSec = *val; });

        helper.addParam(
          *JunoParamId::kAenvDecay,
          {
            .displayName = "Decay",
            .defaultValue = 0.5f,
            .minValue = 0.002f,
            .maxValue = 8.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvDecay,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.aenvDecaySec = *val; });

        helper.addParam(
          *JunoParamId::kAenvSustain,
          {
            .displayName = "Sustain",
            .defaultValue = 0.8f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kAmpEnvSustain,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.aenvSustainLevel = *val; });

        helper.addParam(
          *JunoParamId::kAenvRelease,
          {
            .displayName = "Release",
            .defaultValue = 0.4f,
            .minValue = 0.002f,
            .maxValue = 8.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvRelease,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.aenvReleaseSec = *val; });

        helper.addParam(
          *JunoParamId::kVcaMode,
          {
            .displayName = "VCA Mode",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = 1,
            .isEnum = true,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t value) {
                constexpr std::array kVcaNames = { "Env", "Gate" };
                return std::string(kVcaNames.at(std::clamp<int>(std::lround(value), 0, 1)));
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) {
              voice.vcaMode = std::clamp<int>(std::lround(*val), 0, 1);
          });

        // --- 5. LFO SECTION ---
        helper.addParam(
          *JunoParamId::kLfoRate,
          {
            .displayName = "Rate",
            .defaultValue = 5.0f,
            .minValue = 0.1f,
            .maxValue = 30.0f,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t value) {
                return fmt::format("{:.1f} Hz", value);
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.lfoRateHz = *val; });

        helper.addParam(
          *JunoParamId::kLfoDelay,
          {
            .displayName = "Delay",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 3.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.lfoDelaySec = *val; });

        helper.addParam(
          *JunoParamId::kLfoPitch,
          {
            .displayName = "Pitch Mod",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.lfoPitchDepth = *val; });

        // --- 6. MASTER / FX SECTION ---
        helper.addParam(
          *JunoParamId::kChorusMode,
          {
            .displayName = "Chorus",
            .defaultValue = 1,
            .minValue = 0,
            .maxValue = 3,
            .isEnum = true,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t value) {
                constexpr std::array kChNames = { "Off", "I", "II", "I+II" };
                return std::string(kChNames.at(std::clamp<int>(std::lround(value), 0, 3)));
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) {
              voice.chorusMode = std::clamp<int>(std::lround(*val), 0, 3);
          });

        helper.addParam(
          *JunoParamId::kVolume,
          {
            .displayName = "Volume",
            .defaultValue = 0.9f,
            .minValue = 0.0f,
            .maxValue = 1.5f,
            .macroOperation = Patch::Metadata::Param::MacroOperation::kMult,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.volume = *val; });

        // --- UI LAYOUT SECTIONS (6 Sections for 2x6 Patch Editor) ---
        metadata.layoutFlat = {
            .sections = {
              {
                .name = "Oscillator",
                .shortName = "DCO",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kOctave },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kTune },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kSawLevel },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kPulseLevel },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kPulseWidth },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kPwmDepth },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kPwmSource },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kSubLevel },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kSubOctave },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kNoiseLevel },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kDrift },
                },
              },
              {
                .name = "Filter",
                .shortName = "Filter",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kHpf },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kCutoff },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kResonance },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kFilterEnv },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kFilterLfo },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kFilterKeytrack },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kOtaDrive },
                },
              },
              {
                .name = "Filter Env",
                .shortName = "F-Env",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kFenvAttack },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kFenvDecay },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kFenvSustain },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kFenvRelease },
                },
              },
              {
                .name = "Amp Env",
                .shortName = "A-Env",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kAenvAttack },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kAenvDecay },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kAenvSustain },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kAenvRelease },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kVcaMode },
                },
              },
              {
                .name = "LFO",
                .shortName = "LFO",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kLfoRate },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kLfoDelay },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kLfoPitch },
                },
              },
              {
                .name = "Master FX",
                .shortName = "Chorus",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kChorusMode },
                  { .type = EditorElement::Type::kParam, .id = *JunoParamId::kVolume },
                },
              },
            },
        };

        metadata.layout23 = metadata.layoutFlat;

        // --- 6 DEFAULT MACROS ---
        metadata.initDefaultMacros = [](auto& macros) {
            macros[0] = Patch::Metadata::Macro{
                .displayName = "Cutoff",
                .modifiers = {
                  { *JunoParamId::kCutoff, 0.5f },
                  { *JunoParamId::kResonance, 0.25f },
                },
                .bipolar = true,
            };
            macros[1] = Patch::Metadata::Macro{
                .displayName = "Resonance",
                .modifiers = {
                  { *JunoParamId::kResonance, 0.5f },
                },
                .bipolar = false,
            };
            macros[2] = Patch::Metadata::Macro{
                .displayName = "Env Mod",
                .modifiers = {
                  { *JunoParamId::kFilterEnv, 0.6f },
                },
                .bipolar = true,
            };
            macros[3] = Patch::Metadata::Macro{
                .displayName = "Sub / Bass",
                .modifiers = {
                  { *JunoParamId::kSubLevel, 0.5f },
                  { *JunoParamId::kOtaDrive, 0.3f },
                },
                .bipolar = false,
            };
            macros[4] = Patch::Metadata::Macro{
                .displayName = "PWM Depth",
                .modifiers = {
                  { *JunoParamId::kPwmDepth, 0.6f },
                },
                .bipolar = false,
            };
            macros[5] = Patch::Metadata::Macro{
                .displayName = "Release",
                .modifiers = {
                  { *JunoParamId::kFenvRelease, 1.5f },
                  { *JunoParamId::kAenvRelease, 1.5f },
                },
                .bipolar = true,
            };

            macros[*Patch::SystemMacroId::kVelocity].modifiers = {
                { *JunoParamId::kVolume, 0.6f },
                { *JunoParamId::kCutoff, 0.25f },
            };
        };

        metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kJunoSynth);
        metadata.outputChannels = Patch::Metadata::OutputChannels::kStereo;
    }

  private:
    // Real-time state
    note_t currentNote = 60;
    float currentVelocity = 1.0f;
    bool gate = false;
    bool active = false;

    float phase = 0.0f;
    float subPhase = 0.0f;

    Adsr fenv;
    Adsr aenv;
    float gateSlew = 0.0f;
    float lfoPhase = 0.0f;
    float lfoFade = 0.0f;

    // Filter state
    float ladderS[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float shelfLp = 0.0f;
    float hp1 = 0.0f;
    float hp2 = 0.0f;

    // Voice drift state
    uint32_t driftRnd = 0x9e3779b9u;
    uint32_t noiseRnd = 0x12345678u;
    float driftPitch = 0.0f;
    float driftCutoff = 0.0f;

    // Parameters
    float octave = 1.0f;
    float tuneSemitones = 0.0f;
    float sawLevel = 0.8f;
    float pulseLevel = 0.0f;
    float pulseWidth = 0.5f;
    float pwmDepth = 0.3f;
    int pwmSource = 1;
    float subLevel = 0.2f;
    float subOctave = 0.0f;
    float noiseLevel = 0.0f;
    float voiceDrift = 0.35f;

    float hpfCutoff = 0.0f;
    float cutoffNorm = 0.62f;
    float resonance = 0.15f;
    float filterEnvDepth = 0.4f;
    float filterLfoDepth = 0.0f;
    float filterKeytrack = 0.5f;
    float otaDrive = 0.25f;

    float fenvAttackSec = 0.005f;
    float fenvDecaySec = 0.6f;
    float fenvSustainLevel = 0.3f;
    float fenvReleaseSec = 0.4f;

    float aenvAttackSec = 0.01f;
    float aenvDecaySec = 0.5f;
    float aenvSustainLevel = 0.8f;
    float aenvReleaseSec = 0.4f;
    int vcaMode = 0;

    float lfoRateHz = 5.0f;
    float lfoDelaySec = 0.0f;
    float lfoPitchDepth = 0.0f;

    int chorusMode = 1;
    float volume = 0.9f;

    // Stereo BBD Chorus state (voice 0 only)
    int chorusWrite = 0;
    float chorusPhase = 0.0f;
    float chorusLpL = 0.0f, chorusLpR = 0.0f;
    float chorusLpL2 = 0.0f, chorusLpR2 = 0.0f;
    float chorusDepthSm = 0.0f;
    float chorusRateSm = 0.5f;
    float chorusMix = 0.0f;
};

} // namespace

std::unique_ptr<InstrumentType> createJunoSynth()
{
    return std::make_unique<InstrumentType>(
      InstrumentType{ .name = "JULYO",
                      .createMetadata = &JunoSynthVoice::createMetadata,
                      .createVoice = [](int voiceIdx) -> std::unique_ptr<InstrumentVoice>
                      { return std::make_unique<JunoSynthVoice>(voiceIdx); } });
}

} // namespace polysynth

#endif
