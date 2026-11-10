#include "config.h"
#ifdef FEATURE_SYNTH

#include "TearSynthVoice.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace polysynth
{

namespace
{

enum class TearParamId : int
{
    // Oscillator
    kSyncMode = 0,
    kSyncRatio,
    kSlaveShape,
    kSubLevel,
    kSubOctave,
    kTune,
    kFine,

    // Modulation
    kSyncEnvDecay,
    kSyncEnvDepth,
    kSyncLfoRate,
    kSyncLfoDepth,

    // Filter
    kFilterType,
    kCutoff,
    kResonance,
    kFilterEnvAmount,

    // Filter Envelope
    kFilterAttack,
    kFilterDecay,
    kFilterSustain,
    kFilterRelease,

    // Amplifier & Saturation
    kDrive,
    kSaturationType,
    kAmpAttack,
    kAmpDecay,
    kAmpSustain,
    kAmpRelease,

    // Voice
    kPlayMode,
    kGlideTime,
    kVolume,
};

enum class SyncMode : int
{
    kHardTear = 0,
    kSoftReverse = 1,
    kWindowedFormant = 2,
    kDualSuperTear = 3,
    kRingShred = 4,
};

enum class FilterType : int
{
    kLadder24 = 0,
    kWarmSVF12 = 1,
    kBypass = 2,
};

enum class SaturationType : int
{
    kModernSoft = 0,
    kClassicHard = 1,
    kTubeValve = 2,
    kTape = 3,
    kTransformer = 4,
};

static constexpr std::array<const char*, 5> kSyncModeNames = {
    "Hard Tear", "Soft Reverse", "Windowed Formant", "Dual SuperTear", "Ring Shred",
};

static constexpr std::array<const char*, 3> kFilterTypeNames = {
    "Ladder 24dB", "Warm SVF 12dB", "Bypass",
};

static constexpr std::array<const char*, 5> kSaturationNames = {
    "Modern Soft", "Classic Hard", "Tube / Valve", "Tape", "Transformer",
};

static constexpr std::array<const char*, 2> kSubOctaveNames = {
    "-1 Octave", "-2 Octaves",
};

// PolyBLEP anti-aliasing residual for step discontinuity (division-free using invDt)
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

// Fast tanh approximation with input clamping to prevent polynomial divergence
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

// Precomputed filter coefficients for block-rate evaluation
struct LadderCoeffs
{
    float G;
    float G2;
    float G3;
    float k;
    float denomInv;
};

struct SVFCoeffs
{
    float g;
    float gPlusDamping;
    float h;
};

// Evaluate raw waveform from phase with PolyBLEP
inline float evalShape(float phase, float shape, float dt, float invDt)
{
    if (phase >= 1.0f) phase -= static_cast<int>(phase);
    else if (phase < 0.0f)
    {
        phase += 1.0f;
        if (phase < 0.0f) phase -= static_cast<int>(phase) - 1.0f;
    }

    const float blep0 = polyBLEP(phase, dt, invDt);
    const float saw = 2.0f * phase - 1.0f - blep0;

    float pShift = phase + 0.5f;
    if (pShift >= 1.0f) pShift -= 1.0f;
    const float blepShift = polyBLEP(pShift, dt, invDt);
    const float sq = ((phase < 0.5f) ? 1.0f : -1.0f) + blep0 - blepShift;

    if (shape <= 0.5f)
    {
        const float morph = shape * 2.0f;
        return saw + morph * (sq - saw);
    }
    else
    {
        const float morph = (shape - 0.5f) * 2.0f;
        const float saw2 = 2.0f * pShift - 1.0f - blepShift;
        const float dualSaw = 0.5f * (saw + saw2);
        return sq + morph * (dualSaw - sq);
    }
}

class TearSynthVoice : public sdk::CustomSynthVoice<TearSynthVoice, TearParamId>
{
  public:
    TearSynthVoice() = default;

    bool isActive() final { return ampEnvStage != EnvStage::kIdle; }

    void onNoteOn(const InstrumentVoiceContext& /*ctx*/,
                  note_t note,
                  float /*velocity*/,
                  bool isLegatoStep) final
    {
        const float targetNote = static_cast<float>(note);
        if (!hasPlayedAnyNote || glideTimeSec <= 0.001f ||
            (playMode == PlayMode::kLegato && !isLegatoStep))
        {
            currentGlideNote = targetNote;
        }
        targetGlideNote = targetNote;
        hasPlayedAnyNote = true;

        if (!isLegatoStep || ampEnvStage == EnvStage::kIdle || ampEnvStage == EnvStage::kRelease)
        {
            if (ampEnvStage == EnvStage::kIdle) ampEnvLevel = 0.0f;
            if (filterEnvStage == EnvStage::kIdle) filterEnvLevel = 0.0f;
            ampEnvStage = EnvStage::kAttack;
            filterEnvStage = EnvStage::kAttack;

            // Sync Decay envelope: starts at 1.0 and decays to 0.0
            syncEnvVal = 1.0f;

            // Reset phase on new gate
            masterPhase = 0.0f;
            slavePhase1 = 0.0f;
            slavePhase2 = 0.0f;
            slaveDirection = 1.0f;
            subPhase = 0.0f;
        }
    }

    void onNoteOff(const InstrumentVoiceContext& /*ctx*/, note_t /*note*/) final
    {
        if (ampEnvStage != EnvStage::kIdle)
        {
            ampEnvStage = EnvStage::kRelease;
        }
        if (filterEnvStage != EnvStage::kIdle)
        {
            filterEnvStage = EnvStage::kRelease;
        }
    }

    void renderVoiceAudio(const InstrumentVoiceContext& /*ctx*/,
                          [[maybe_unused]] std::optional<AudioBufferStereoRef> inputBuffer,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef> /*auxOutBuffer*/) final
    {
        auto out = outputBuffer[0];
        if (ampEnvStage == EnvStage::kIdle)
        {
            std::fill(out.begin(), out.end(), 0.0f);
            return;
        }

        const float sr = sdk::kSampleRate;
        const float invSr = 1.0f / sr;

        // Envelope rates per sample
        const float ampAttackInc = (ampAttackSec > 0.001f) ? (1.0f / (ampAttackSec * sr)) : 1.0f;
        const float ampDecayDec = (ampDecaySec > 0.002f) ? ((1.0f - ampSustainLevel) / (ampDecaySec * sr)) : 1.0f;
        const float ampReleaseDec = (ampReleaseSec > 0.002f) ? (1.0f / (ampReleaseSec * sr)) : 1.0f;

        const float filterAttackInc = (filterAttackSec > 0.001f) ? (1.0f / (filterAttackSec * sr)) : 1.0f;
        const float filterDecayDec = (filterDecaySec > 0.002f) ? ((1.0f - filterSustainLevel) / (filterDecaySec * sr)) : 1.0f;
        const float filterReleaseDec = (filterReleaseSec > 0.002f) ? (1.0f / (filterReleaseSec * sr)) : 1.0f;

        const float syncEnvDec = (syncEnvDecaySec > 0.005f) ? (1.0f / (syncEnvDecaySec * sr)) : 1.0f;
        const float syncLfoPhaseInc = std::clamp(syncLfoRateHz, 0.05f, 30.0f) * invSr;

        // Portamento / glide coefficient
        const float glideStep = (glideTimeSec > 0.001f) ? (1.0f / (glideTimeSec * sr)) : 1.0f;

        const float baseNoteOffset = tuneSemitones + (fineCents * 0.01f) + pitchBendSemitones;

        const float subDiv = (subOctave == 1) ? 0.25f : 0.5f;
        const float subLevel = std::clamp(subMixLevel, 0.0f, 1.0f);
        const float mainOscGain = 1.0f - 0.5f * subLevel;

        const float res = std::clamp(filterResonance, 0.0f, 0.98f);
        const float driveGain = 1.0f + driveAmount * 3.0f;
        const float tapeCoeff = 0.45f + 0.35f * (1.0f - driveAmount);
        const float transCoeff = 0.28f + 0.15f * driveAmount;

        constexpr int kCtl = 16;
        int done = 0;
        const int totalSamples = static_cast<int>(out.size());

        LadderCoeffs ladderCoeffs{};
        SVFCoeffs svfCoeffs{};

        while (done < totalSamples)
        {
            if (ampEnvStage == EnvStage::kIdle)
            {
                std::fill(out.begin() + done, out.end(), 0.0f);
                return;
            }

            const int chunk = std::min(kCtl, totalSamples - done);

            // --- Control Rate Updates ---
            // 1. Process Glide
            if (std::abs(currentGlideNote - targetGlideNote) > 0.001f)
            {
                const float diff = targetGlideNote - currentGlideNote;
                const float step = std::min(std::abs(diff), glideStep * 24.0f * chunk);
                currentGlideNote += (diff > 0.0f) ? step : -step;
            }
            else
            {
                currentGlideNote = targetGlideNote;
            }

            // 2. Modulations
            if (syncEnvVal > 0.0f)
            {
                syncEnvVal = std::max(0.0f, syncEnvVal - syncEnvDec * chunk);
            }

            syncLfoPhase += syncLfoPhaseInc * chunk;
            if (syncLfoPhase >= 1.0f) syncLfoPhase -= static_cast<int>(syncLfoPhase);
            const float lfoVal = sdk::fastSine(syncLfoPhase);

            const float totalMasterNote = currentGlideNote + baseNoteOffset;
            const float fMaster = std::clamp(440.0f * sdk::fastPow2((totalMasterNote - 69.0f) * (1.0f / 12.0f)),
                                             10.0f, sr * 0.45f);
            const float dtMaster = fMaster * invSr;
            const float invDtMaster = (dtMaster > 0.0f) ? (1.0f / dtMaster) : 0.0f;

            float syncSemitones = syncRatioSemitones + (syncEnvVal * syncEnvDepthSemi) + (lfoVal * syncLfoDepthSemi);
            syncSemitones = std::clamp(syncSemitones, -24.0f, 60.0f);
            const float syncFreqMult = sdk::fastPow2(syncSemitones * (1.0f / 12.0f));

            const float fSlave1 = std::clamp(fMaster * syncFreqMult, 10.0f, sr * 0.48f);
            const float dtSlave1 = fSlave1 * invSr;
            const float invDtSlave1 = (dtSlave1 > 0.0f) ? (1.0f / dtSlave1) : 0.0f;

            const float fSlave2 = std::clamp(fSlave1 * 1.0081f, 10.0f, sr * 0.48f);
            const float dtSlave2 = fSlave2 * invSr;
            const float invDtSlave2 = (dtSlave2 > 0.0f) ? (1.0f / dtSlave2) : 0.0f;

            // 3. Filter Cutoff and Coefficients
            const float targetCutoff = std::clamp(
              filterCutoffHz * sdk::fastPow2(filterEnvAmount * filterEnvLevel * 4.0f),
              20.0f,
              sr * 0.45f);

            if (filterType == FilterType::kLadder24)
            {
                const float omega = std::numbers::pi_v<float> * targetCutoff * invSr;
                float g = fastTan(omega);
                if (g > 2.0f) g = 2.0f;

                const float G = g / (1.0f + g);
                const float k = 3.82f * (0.35f * res + 0.65f * res * res);
                const float G2 = G * G;
                const float G3 = G2 * G;
                const float G4 = G3 * G;
                const float denomInv = 1.0f / (1.0f + k * G4);
                ladderCoeffs = { G, G2, G3, k, denomInv };
            }
            else if (filterType == FilterType::kWarmSVF12)
            {
                const float omega = std::numbers::pi_v<float> * targetCutoff * invSr;
                float g = fastTan(omega);
                if (g > 2.0f) g = 2.0f;

                const float damping = 2.0f * (1.0f - res * 0.97f);
                const float gPlusDamping = g + damping;
                const float h = 1.0f / (1.0f + g * gPlusDamping);
                svfCoeffs = { g, gPlusDamping, h };
            }

            // --- Audio Rate Loop for this Chunk ---
            for (int i = 0; i < chunk; ++i)
            {
                // 1. Process Amp ADSR
                switch (ampEnvStage)
                {
                    case EnvStage::kAttack:
                        ampEnvLevel += ampAttackInc;
                        if (ampEnvLevel >= 1.0f)
                        {
                            ampEnvLevel = 1.0f;
                            ampEnvStage = EnvStage::kDecay;
                        }
                        break;
                    case EnvStage::kDecay:
                        ampEnvLevel -= ampDecayDec;
                        if (ampEnvLevel <= ampSustainLevel)
                        {
                            ampEnvLevel = ampSustainLevel;
                            ampEnvStage = (ampSustainLevel <= 0.0001f) ? EnvStage::kIdle : EnvStage::kSustain;
                        }
                        break;
                    case EnvStage::kSustain:
                        ampEnvLevel = ampSustainLevel;
                        break;
                    case EnvStage::kRelease:
                        ampEnvLevel -= ampReleaseDec;
                        if (ampEnvLevel <= 0.0001f)
                        {
                            ampEnvLevel = 0.0f;
                            ampEnvStage = EnvStage::kIdle;
                        }
                        break;
                    case EnvStage::kIdle:
                        out[done + i] = 0.0f;
                        continue;
                }

                // 2. Process Filter ADSR
                switch (filterEnvStage)
                {
                    case EnvStage::kAttack:
                        filterEnvLevel += filterAttackInc;
                        if (filterEnvLevel >= 1.0f)
                        {
                            filterEnvLevel = 1.0f;
                            filterEnvStage = EnvStage::kDecay;
                        }
                        break;
                    case EnvStage::kDecay:
                        filterEnvLevel -= filterDecayDec;
                        if (filterEnvLevel <= filterSustainLevel)
                        {
                            filterEnvLevel = filterSustainLevel;
                            filterEnvStage = (filterSustainLevel <= 0.0001f) ? EnvStage::kIdle : EnvStage::kSustain;
                        }
                        break;
                    case EnvStage::kSustain:
                        filterEnvLevel = filterSustainLevel;
                        break;
                    case EnvStage::kRelease:
                        filterEnvLevel -= filterReleaseDec;
                        if (filterEnvLevel <= 0.0001f)
                        {
                            filterEnvLevel = 0.0f;
                            filterEnvStage = EnvStage::kIdle;
                        }
                        break;
                    case EnvStage::kIdle:
                        break;
                }

                // 3. Advance Master Phase & detect wrap
                masterPhase += dtMaster;
                bool syncTriggered = false;
                float tFrac = 0.0f;

                if (masterPhase >= 1.0f)
                {
                    masterPhase -= 1.0f;
                    syncTriggered = true;
                    tFrac = std::clamp(masterPhase * invDtMaster, 0.0f, 0.999f);
                }

                // 4. Synthesize Slave Oscillator
                float slaveOut = 0.0f;
                if (syncMode == SyncMode::kSoftReverse)
                {
                    if (syncTriggered) slaveDirection = -slaveDirection;
                    slavePhase1 += dtSlave1 * slaveDirection;
                    if (slavePhase1 >= 1.0f) slavePhase1 -= 1.0f;
                    else if (slavePhase1 < 0.0f) slavePhase1 += 1.0f;
                    slaveOut = evalShape(slavePhase1, slaveShape, dtSlave1, invDtSlave1);
                }
                else if (syncMode == SyncMode::kWindowedFormant)
                {
                    if (syncTriggered) slavePhase1 = tFrac * dtSlave1;
                    else
                    {
                        slavePhase1 += dtSlave1;
                        if (slavePhase1 >= 1.0f) slavePhase1 -= 1.0f;
                    }
                    const float rawSlave = evalShape(slavePhase1, slaveShape, dtSlave1, invDtSlave1);
                    const float win = 0.5f * (1.0f - sdk::fastSine(masterPhase - 0.25f));
                    slaveOut = rawSlave * win;
                }
                else if (syncMode == SyncMode::kDualSuperTear)
                {
                    if (syncTriggered)
                    {
                        slavePhase1 = tFrac * dtSlave1;
                        slavePhase2 = tFrac * dtSlave2;
                    }
                    else
                    {
                        slavePhase1 += dtSlave1;
                        if (slavePhase1 >= 1.0f) slavePhase1 -= 1.0f;
                        slavePhase2 += dtSlave2;
                        if (slavePhase2 >= 1.0f) slavePhase2 -= 1.0f;
                    }
                    const float s1 = evalShape(slavePhase1, slaveShape, dtSlave1, invDtSlave1);
                    const float s2 = evalShape(slavePhase2, slaveShape, dtSlave2, invDtSlave2);
                    slaveOut = 0.5f * (s1 + s2);
                }
                else if (syncMode == SyncMode::kRingShred)
                {
                    if (syncTriggered) slavePhase1 = tFrac * dtSlave1;
                    else
                    {
                        slavePhase1 += dtSlave1;
                        if (slavePhase1 >= 1.0f) slavePhase1 -= 1.0f;
                    }
                    const float s = evalShape(slavePhase1, slaveShape, dtSlave1, invDtSlave1);
                    const float m = 2.0f * masterPhase - 1.0f - polyBLEP(masterPhase, dtMaster, invDtMaster);
                    slaveOut = s * m * 1.5f;
                }
                else
                {
                    // Mode 0: Hard Tear
                    if (syncTriggered)
                    {
                        float phaseBefore = slavePhase1 + dtSlave1 * (1.0f - tFrac);
                        if (phaseBefore >= 1.0f) phaseBefore -= static_cast<int>(phaseBefore);
                        const float valBefore = evalShape(phaseBefore, slaveShape, dtSlave1, invDtSlave1);
                        slavePhase1 = tFrac * dtSlave1;
                        if (slavePhase1 >= 1.0f) slavePhase1 -= static_cast<int>(slavePhase1);
                        const float valAfter = evalShape(slavePhase1, slaveShape, dtSlave1, invDtSlave1);
                        const float delta = valAfter - valBefore;
                        slaveOut = valAfter - delta * (1.0f - tFrac);
                    }
                    else
                    {
                        slavePhase1 += dtSlave1;
                        if (slavePhase1 >= 1.0f) slavePhase1 -= 1.0f;
                        slaveOut = evalShape(slavePhase1, slaveShape, dtSlave1, invDtSlave1);
                    }
                }

                // 5. Sub-Oscillator
                const float dtSub = dtMaster * subDiv;
                subPhase += dtSub;
                if (subPhase >= 1.0f) subPhase -= 1.0f;
                const float subTri = 4.0f * std::abs(subPhase - 0.5f) - 1.0f;

                const float oscMix = (slaveOut * mainOscGain) + (subTri * subLevel);

                // 6. Zero-Delay Feedback Filter
                float filtered = oscMix;
                if (filterType == FilterType::kLadder24)
                {
                    filtered = processLadder(oscMix, ladderCoeffs);
                }
                else if (filterType == FilterType::kWarmSVF12)
                {
                    filtered = processWarmSVF(oscMix, svfCoeffs);
                }

                // 7. Saturation Stage & Drive
                const float saturated = applySaturation(filtered, driveAmount, driveGain, saturationType, tapeCoeff, transCoeff);

                // 8. Amp Envelope & Master Gain
                out[done + i] = saturated * ampEnvLevel * volumeMult;
            }

            // State bounding per chunk
            if (filterType == FilterType::kLadder24)
            {
                for (int k = 0; k < 4; ++k)
                {
                    ladderS[k] = std::clamp(ladderS[k], -4.0f, 4.0f);
                }
            }
            else if (filterType == FilterType::kWarmSVF12)
            {
                svfIc1eq = std::clamp(svfIc1eq, -3.0f, 3.0f);
                svfIc2eq = std::clamp(svfIc2eq, -3.0f, 3.0f);
            }

            done += chunk;
        }
    }

    static void createMetadata(const Patch& /*patch*/, Patch::Metadata& metadata)
    {
        using namespace polysynth::sdk;

        metadata.params.resize(enumutil::EnumMax<TearParamId>::value + 1);
        metadata.params.shrink_to_fit();

        if (paramHandlers.empty())
        {
            paramHandlers.resize(enumutil::EnumMax<TearParamId>::value + 1);
        }

        ParamHelper<TearSynthVoice> helper(metadata, paramHandlers);

        // --- 1. OSCILLATOR SECTION ---
        helper.addParam(
          *TearParamId::kSyncMode,
          {
            .displayName = "Sync Mode",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = static_cast<Patch::paramvalue_short_t>(kSyncModeNames.size() - 1),
            .isEnum = true,
            .midiCC = *MidiCCs::kOscParam0,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t value) {
                const int idx = std::clamp<int>(static_cast<int>(std::lround(value)), 0, kSyncModeNames.size() - 1);
                return std::string(kSyncModeNames[idx]);
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) {
              voice.syncMode = static_cast<SyncMode>(std::clamp<int>(static_cast<int>(std::lround(*val)), 0, kSyncModeNames.size() - 1));
          });

        helper.addParam(
          *TearParamId::kSyncRatio,
          {
            .displayName = "Sync Ratio",
            .defaultValue = 12.0f,
            .minValue = 0.0f,
            .maxValue = 48.0f,
            .editCurve = Patch::Metadata::Param::Curve::kLinear,
            .macroOperation = Patch::Metadata::Param::MacroOperation::kAdd,
            .midiCC = *MidiCCs::kOscParam0 + 1,
            .valueFormatter = createSemitoneFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.syncRatioSemitones = *val; });

        helper.addParam(
          *TearParamId::kSlaveShape,
          {
            .displayName = "Shape Morph",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kOscParam0 + 2,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t val) {
                if (val < 0.25f) return std::string("Saw");
                if (val < 0.75f) return std::string("Pulse");
                return std::string("DualSaw");
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.slaveShape = *val; });

        helper.addParam(
          *TearParamId::kSubLevel,
          {
            .displayName = "Sub Level",
            .defaultValue = 0.25f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kOscParam0 + 3,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.subMixLevel = *val; });

        helper.addParam(
          *TearParamId::kSubOctave,
          {
            .displayName = "Sub Octave",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = static_cast<Patch::paramvalue_short_t>(kSubOctaveNames.size() - 1),
            .isEnum = true,
            .midiCC = *MidiCCs::kOscParam0 + 4,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t val) {
                const int idx = std::clamp<int>(static_cast<int>(std::lround(val)), 0, kSubOctaveNames.size() - 1);
                return std::string(kSubOctaveNames[idx]);
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.subOctave = static_cast<int>(std::lround(*val)); });

        helper.addParam(
          *TearParamId::kTune,
          {
            .displayName = "Tune",
            .defaultValue = 0.0f,
            .minValue = -24,
            .maxValue = 24,
            .midiCC = *MidiCCs::kOscParam0 + 5,
            .valueFormatter = createSemitoneFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.tuneSemitones = std::round(*val); });

        helper.addParam(
          *TearParamId::kFine,
          {
            .displayName = "Fine",
            .defaultValue = 0.0f,
            .minValue = -100,
            .maxValue = 100,
            .midiCC = *MidiCCs::kOscParam0 + 6,
            .valueFormatter = createCentsFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.fineCents = *val; });

        // --- 2. MODULATION SECTION ---
        helper.addParam(
          *TearParamId::kSyncEnvDecay,
          {
            .displayName = "Sync Decay",
            .defaultValue = 0.35f,
            .minValue = 0.005f,
            .maxValue = 3.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.syncEnvDecaySec = *val; });

        helper.addParam(
          *TearParamId::kSyncEnvDepth,
          {
            .displayName = "Sync Depth",
            .defaultValue = 24.0f,
            .minValue = -48.0f,
            .maxValue = 48.0f,
            .valueFormatter = createSemitoneFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.syncEnvDepthSemi = *val; });

        helper.addParam(
          *TearParamId::kSyncLfoRate,
          {
            .displayName = "Sync LFO Rate",
            .defaultValue = 1.5f,
            .minValue = 0.05f,
            .maxValue = 25.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .valueFormatter = createFrequencyFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.syncLfoRateHz = *val; });

        helper.addParam(
          *TearParamId::kSyncLfoDepth,
          {
            .displayName = "Sync LFO Depth",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 24.0f,
            .valueFormatter = createSemitoneFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.syncLfoDepthSemi = *val; });

        // --- 3. FILTER SECTION ---
        helper.addParam(
          *TearParamId::kFilterType,
          {
            .displayName = "Filter Model",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = static_cast<Patch::paramvalue_short_t>(kFilterTypeNames.size() - 1),
            .isEnum = true,
            .midiCC = *MidiCCs::kFilterType,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t val) {
                const int idx = std::clamp<int>(static_cast<int>(std::lround(val)), 0, kFilterTypeNames.size() - 1);
                return std::string(kFilterTypeNames[idx]);
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) {
              voice.filterType = static_cast<FilterType>(std::clamp<int>(static_cast<int>(std::lround(*val)), 0, kFilterTypeNames.size() - 1));
          });

        helper.addParam(
          *TearParamId::kCutoff,
          {
            .displayName = "Cutoff",
            .defaultValue = 12000.0f,
            .minValue = 20.0f,
            .maxValue = 18000.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kFilterCutoff,
            .valueFormatter = createFrequencyFormatter(18000.0f),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterCutoffHz = *val; });

        helper.addParam(
          *TearParamId::kResonance,
          {
            .displayName = "Resonance",
            .defaultValue = 0.15f,
            .minValue = 0.0f,
            .maxValue = 0.95f,
            .midiCC = *MidiCCs::kFilterResonance,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterResonance = *val; });

        helper.addParam(
          *TearParamId::kFilterEnvAmount,
          {
            .displayName = "Env Amount",
            .defaultValue = 0.0f,
            .minValue = -1.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterEnvAmount = *val; });

        // --- 4. FILTER ENVELOPE SECTION ---
        helper.addParam(
          *TearParamId::kFilterAttack,
          {
            .displayName = "Attack",
            .defaultValue = 0.01f,
            .minValue = 0.001f,
            .maxValue = 2.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterAttackSec = *val; });

        helper.addParam(
          *TearParamId::kFilterDecay,
          {
            .displayName = "Decay",
            .defaultValue = 0.5f,
            .minValue = 0.01f,
            .maxValue = 3.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterDecaySec = *val; });

        helper.addParam(
          *TearParamId::kFilterSustain,
          {
            .displayName = "Sustain",
            .defaultValue = 0.2f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterSustainLevel = *val; });

        helper.addParam(
          *TearParamId::kFilterRelease,
          {
            .displayName = "Release",
            .defaultValue = 0.3f,
            .minValue = 0.01f,
            .maxValue = 3.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterReleaseSec = *val; });

        // --- 5. AMPLIFIER & SATURATION SECTION ---
        helper.addParam(
          *TearParamId::kDrive,
          {
            .displayName = "Drive",
            .defaultValue = 0.2f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.driveAmount = *val; });

        helper.addParam(
          *TearParamId::kSaturationType,
          {
            .displayName = "Saturation",
            .defaultValue = 2, // Tube / Valve default
            .minValue = 0,
            .maxValue = static_cast<Patch::paramvalue_short_t>(kSaturationNames.size() - 1),
            .isEnum = true,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t val) {
                const int idx = std::clamp<int>(static_cast<int>(std::lround(val)), 0, kSaturationNames.size() - 1);
                return std::string(kSaturationNames[idx]);
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) {
              voice.saturationType = static_cast<SaturationType>(std::clamp<int>(static_cast<int>(std::lround(*val)), 0, kSaturationNames.size() - 1));
          });

        helper.addParam(
          *TearParamId::kAmpAttack,
          {
            .displayName = "Attack",
            .defaultValue = 0.005f,
            .minValue = 0.001f,
            .maxValue = 2.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvAttack,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.ampAttackSec = *val; });

        helper.addParam(
          *TearParamId::kAmpDecay,
          {
            .displayName = "Decay",
            .defaultValue = 0.4f,
            .minValue = 0.01f,
            .maxValue = 3.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvDecay,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.ampDecaySec = *val; });

        helper.addParam(
          *TearParamId::kAmpSustain,
          {
            .displayName = "Sustain",
            .defaultValue = 0.7f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kAmpEnvSustain,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.ampSustainLevel = *val; });

        helper.addParam(
          *TearParamId::kAmpRelease,
          {
            .displayName = "Release",
            .defaultValue = 0.25f,
            .minValue = 0.01f,
            .maxValue = 3.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvRelease,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.ampReleaseSec = *val; });

        // --- 6. VOICE SECTION ---
        helper.addParam(
          *TearParamId::kPlayMode,
          {
            .displayName = "Glide Mode",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = 2,
            .isEnum = true,
            .valueFormatter = [](const Patch& /*patch*/, Patch::paramvalue_t value) {
                constexpr std::array kModes = { "Always", "Overlap", "Legato" };
                return std::string(kModes.at(std::clamp<int>(std::lround(value), 0, 2)));
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) {
              voice.playMode = static_cast<PlayMode>(std::clamp<int>(std::lround(*val), 0, 2));
          });

        helper.addParam(
          *TearParamId::kGlideTime,
          {
            .displayName = "Glide Time",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 2.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.glideTimeSec = *val; });

        helper.addParam(
          *TearParamId::kVolume,
          {
            .displayName = "Volume",
            .defaultValue = 1.0f,
            .minValue = 0.0f,
            .maxValue = 2.0f,
            .macroOperation = Patch::Metadata::Param::MacroOperation::kMult,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.volumeMult = *val; });

        // --- UI LAYOUT SECTIONS (Max 6 for the 2x6 Patch Editor) ---
        metadata.layoutFlat = {
            .sections = {
              {
                .name = "Oscillator",
                .shortName = "Osc",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kSyncMode },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kSyncRatio },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kSlaveShape },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kSubLevel },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kSubOctave },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kTune },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kFine },
                },
              },
              {
                .name = "Modulation",
                .shortName = "Mod",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kSyncEnvDecay },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kSyncEnvDepth },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kSyncLfoRate },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kSyncLfoDepth },
                },
              },
              {
                .name = "Filter",
                .shortName = "Filter",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kFilterType },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kCutoff },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kResonance },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kFilterEnvAmount },
                },
              },
              {
                .name = "Envelope",
                .shortName = "Env 1",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kFilterAttack },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kFilterDecay },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kFilterSustain },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kFilterRelease },
                },
              },
              {
                .name = "Amplifier",
                .shortName = "Amp",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kDrive },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kSaturationType },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kAmpAttack },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kAmpDecay },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kAmpSustain },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kAmpRelease },
                },
              },
              {
                .name = "Voice",
                .shortName = "Voice",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kPlayMode },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kGlideTime },
                  { .type = EditorElement::Type::kParam, .id = *TearParamId::kVolume },
                },
              },
            },
        };

        metadata.layout23 = metadata.layoutFlat;

        // --- 6 DEFAULT MACROS ---
        metadata.initDefaultMacros = [](auto& macros) {
            macros[0] = Patch::Metadata::Macro{
                .displayName = "Tear Pitch",
                .modifiers = {
                  { *TearParamId::kSyncRatio, 24.0f },
                },
                .bipolar = false,
            };
            macros[1] = Patch::Metadata::Macro{
                .displayName = "Sync Env",
                .modifiers = {
                  { *TearParamId::kSyncEnvDepth, 24.0f },
                },
                .bipolar = true,
            };
            macros[2] = Patch::Metadata::Macro{
                .displayName = "Sync LFO",
                .modifiers = {
                  { *TearParamId::kSyncLfoDepth, 12.0f },
                },
                .bipolar = false,
            };
            macros[3] = Patch::Metadata::Macro{
                .displayName = "Cutoff",
                .modifiers = {
                  { *TearParamId::kCutoff, 1.0f },
                  { *TearParamId::kResonance, 0.35f },
                },
                .bipolar = true,
            };
            macros[4] = Patch::Metadata::Macro{
                .displayName = "Drive",
                .modifiers = {
                  { *TearParamId::kDrive, 0.7f },
                },
                .bipolar = false,
            };
            macros[5] = Patch::Metadata::Macro{
                .displayName = "Shape Morph",
                .modifiers = {
                  { *TearParamId::kSlaveShape, 0.6f },
                },
                .bipolar = true,
            };

            macros[*Patch::SystemMacroId::kVelocity].modifiers = {
                { *TearParamId::kVolume, 1.0f },
            };
        };

        metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kTearSynth);
        metadata.outputChannels = Patch::Metadata::OutputChannels::kMono;
    }

  private:
    enum class EnvStage : uint8_t
    {
        kIdle = 0,
        kAttack,
        kDecay,
        kSustain,
        kRelease,
    };

    // Filter render helpers
    inline float processLadder(float in, const LadderCoeffs& c)
    {
        const float S_equiv = c.G3 * ladderS[0] + c.G2 * ladderS[1] + c.G * ladderS[2] + ladderS[3];
        float u = (in - c.k * S_equiv) * c.denomInv;
        u = std::clamp(u, -3.0f, 3.0f);

        const float v0 = (u - ladderS[0]) * c.G;
        const float y0 = v0 + ladderS[0];
        ladderS[0] = y0 + v0;

        const float v1 = (y0 - ladderS[1]) * c.G;
        const float y1 = v1 + ladderS[1];
        ladderS[1] = y1 + v1;

        const float v2 = (y1 - ladderS[2]) * c.G;
        const float y2 = v2 + ladderS[2];
        ladderS[2] = y2 + v2;

        const float v3 = (y2 - ladderS[3]) * c.G;
        const float y3 = v3 + ladderS[3];
        ladderS[3] = y3 + v3;

        return y3;
    }

    inline float processWarmSVF(float in, const SVFCoeffs& c)
    {
        const float hp = (in - svfIc1eq * c.gPlusDamping - svfIc2eq) * c.h;
        const float v1 = c.g * hp;
        const float bp = v1 + svfIc1eq;
        svfIc1eq = bp + v1;

        const float v2 = c.g * bp;
        const float lp = v2 + svfIc2eq;
        svfIc2eq = lp + v2;

        return lp;
    }

    inline float applySaturation(float in, float drive, float gain, SaturationType type, float tapeCoeff, float transCoeff)
    {
        if (drive <= 0.001f) return in;

        const float x = in * gain;
        switch (type)
        {
            case SaturationType::kModernSoft:
                return fastTanh(x);
            case SaturationType::kClassicHard:
                return std::clamp(x * 1.2f, -1.0f, 1.0f);
            case SaturationType::kTubeValve:
            {
                const float asym = x + 0.22f * drive * x * x;
                return fastTanh(asym);
            }
            case SaturationType::kTape:
            {
                float sat = x - (x * x * x) * 0.16f;
                sat = fastTanh(sat);
                tapeHist += (sat - tapeHist) * tapeCoeff;
                return tapeHist;
            }
            case SaturationType::kTransformer:
            {
                const float diff = x - transMag;
                transMag += diff * transCoeff;
                const float hyst = 0.82f * x + 0.18f * transMag;
                return fastTanh(hyst);
            }
            default:
                return fastTanh(x);
        }
    }

    // Parameters
    SyncMode syncMode = SyncMode::kHardTear;
    float syncRatioSemitones = 12.0f;
    float slaveShape = 0.0f;
    float subMixLevel = 0.25f;
    int subOctave = 0;
    float tuneSemitones = 0.0f;
    float fineCents = 0.0f;

    float syncEnvDecaySec = 0.35f;
    float syncEnvDepthSemi = 24.0f;
    float syncLfoRateHz = 1.5f;
    float syncLfoDepthSemi = 0.0f;

    FilterType filterType = FilterType::kLadder24;
    float filterCutoffHz = 12000.0f;
    float filterResonance = 0.15f;
    float filterEnvAmount = 0.0f;

    float filterAttackSec = 0.01f;
    float filterDecaySec = 0.5f;
    float filterSustainLevel = 0.2f;
    float filterReleaseSec = 0.3f;

    float driveAmount = 0.2f;
    SaturationType saturationType = SaturationType::kTubeValve;
    float ampAttackSec = 0.005f;
    float ampDecaySec = 0.4f;
    float ampSustainLevel = 0.7f;
    float ampReleaseSec = 0.25f;

    float glideTimeSec = 0.0f;
    float volumeMult = 1.0f;

    // Real-time voice state
    EnvStage ampEnvStage = EnvStage::kIdle;
    float ampEnvLevel = 0.0f;
    EnvStage filterEnvStage = EnvStage::kIdle;
    float filterEnvLevel = 0.0f;

    bool hasPlayedAnyNote = false;
    float currentGlideNote = 60.0f;
    float targetGlideNote = 60.0f;

    float syncEnvVal = 0.0f;
    float syncLfoPhase = 0.0f;

    float masterPhase = 0.0f;
    float slavePhase1 = 0.0f;
    float slavePhase2 = 0.0f;
    float slaveDirection = 1.0f;
    float subPhase = 0.0f;

    float ladderS[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float svfIc1eq = 0.0f;
    float svfIc2eq = 0.0f;

    float tapeHist = 0.0f;
    float transMag = 0.0f;
};

} // namespace

std::unique_ptr<InstrumentType> createTearSynth()
{
    return std::make_unique<InstrumentType>(
      InstrumentType{ .name = "TEAR",
                      .createMetadata = &TearSynthVoice::createMetadata,
                      .createVoice = [](int /*voiceIdx*/) -> std::unique_ptr<InstrumentVoice>
                      { return std::make_unique<TearSynthVoice>(); } });
}

} // namespace polysynth

#endif
