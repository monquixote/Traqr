#include "synthPerformance/synthPerformance.h"

#include "Platform.h"
#if GUI_HANDLE_SYNTH
#include "PolySynth/Patch.h"
#endif
#include "ampLogarythmicValues.h"
#include "config.h"
#include "core/interface.h"
#include "core/interfaceMenuPopup.h"
#include "core/interfacePopups.h"
#include "core/titleBar.h"
#include "fileManager.h"
#include "instrumentEditor/instrumentEditor.h"
#include "mtAudioEngine.h"
#include "mtConfig.h"
#include "mtPadBoard.h"
#include "mtPadsBacklight.h"
#include "mtSequencer.h"
#include "performanceMode/performanceMode.h"
#include "util_math.h"
#include "util_string.h"

#include <cmath>
#include <cstdio>
#include <cstring>

cSynthPerformance synthPerformance;
static cSynthPerformance* SP = &synthPerformance;

static void functSwitchModule(uint8_t button);
static void functMenu(uint8_t state);
static void functPlayAction();
static void functScreenButton(uint8_t button);
static void functEncoder(int16_t value);
static void functUp();
static void functDown();
static void functLeft();
static void functRight();
static void functDelete();
static void functInstr(uint8_t state);
static void functMiniMiddle(uint8_t state);
static void functPads(uint8_t pad, uint8_t state, int16_t velo);

#if GUI_HANDLE_SYNTH
static void updateSingleRms(cSynthPerformance::RmsTracker& tracker, float sample)
{
    if (sample >= 0.0f)
    {
        tracker.sum += sample;
        tracker.count++;
        if (tracker.count >= 6)
        {
            float avg = tracker.sum / static_cast<float>(tracker.count);
            tracker.sum = 0.0f;
            tracker.count = 0;

            constexpr float kRms0dB = 0.71f;
            constexpr int kMinDb = -36;
            constexpr int kMaxDb = 6;
            int db = (avg > 0.0001f) ? static_cast<int>(10.0f * log10f(avg / kRms0dB)) : kMinDb;
            db = clamp<int>(db, kMinDb, kMaxDb);

            uint8_t targetLevel = (db <= 0) ? mapClamped<int>(db, kMinDb, 0, 0, 90)
                                            : mapClamped<int>(db, 0, kMaxDb, 90, 100);
            if (targetLevel > tracker.level)
            {
                tracker.level = targetLevel;
                tracker.decayTimer = 0;
            }
        }
    }

    if (tracker.level > 0 && tracker.decayTimer > 45)
    {
        tracker.decayTimer = 0;
        tracker.level = (tracker.level > 4) ? (tracker.level - 4) : 0;
    }
}
#endif

void cSynthPerformance::initDisplayControls()
{
    strControlProperties prop;

    for (int s = 0; s < kSynthParts; s++)
    {
        prop.x = 0;
        prop.y = 30 + s * 131;
        prop.w = 800;
        prop.h = 128;
        prop.value = s;
        prop.style = controlStyleShow;
        prop.colors = display.getTheme().activeBarColors;
        prop.data = &rowData[s];
        prop.text = nullptr;
        rowControl[s] = display.createControl<cSynthPerfRow>(&prop);
    }

    strControlProperties propLabel;
    for (uint8_t i = 0; i < 8; i++)
    {
        propLabel.colors = display.getTheme().activeLabelsColors;
        propLabel.style =
          controlStyleCenterX | controlStyleCenterY | controlStyleFont3 | controlStyleLimitTextWidth;
        propLabel.x = (800 / 8) * i + (800 / 16);
        propLabel.w = 800 / 8 - 6;
        propLabel.y = 424;
        propLabel.h = 55;
        propLabel.value = 1;
        propLabel.data = nullptr;
        propLabel.text = nullptr;
        label[i] = display.createControl<cLabel>(&propLabel);
    }

    strControlProperties propBg;
    propBg.text = nullptr;
    propBg.colors = nullptr;
    propBg.value = 255;
    propBg.style = controlStyleNoTransparency | controlStyleShow;
    propBg.x = 0;
    propBg.w = 800;
    propBg.y = 425;
    propBg.h = 54;
    propBg.data = nullptr;
    bgLabel = display.createControl<cBgLabel>(&propBg);
}

void cSynthPerformance::start(uint32_t options)
{
    (void)options;
    moduleRefresh = 1;

    if (isSynthInstrument(mtProject.values.lastUsedInstrument))
    {
        selectedSynth = instrumentIdxToSynthIdx(mtProject.values.lastUsedInstrument);
    }
    else
    {
        if (selectedSynth >= kSynthParts)
        {
            selectedSynth = 0;
        }
        mtProject.values.lastUsedInstrument = SYNTH_INSTRUMENTS_START_IDX + selectedSynth;
    }

    for (int s = 0; s < kSynthParts; s++)
    {
        dirtyRow[s] = false;
        for (int ch = 0; ch < 2; ch++)
        {
            rmsTrackers[s][ch] = RmsTracker{};
        }
    }
    dirtyBottomLabels = false;
    uiRefreshTimer = 0;

    setScreenFunctions();
    lightUpPadBoard();
    showFullScreen();
}

void cSynthPerformance::stop()
{
    mtPopups.hideInfoPopup();
    sequencer.stopManualNotes();
    moduleRefresh = 0;
    mtPadBoard.releaseInstrumentsTakenByPads();
    padsBacklight.clearGlobalPads(1, 1, 1);
}

void cSynthPerformance::hideControlsUnderMiniMenu()
{
    for (int s = 0; s < kSynthParts; s++)
    {
        display.setControlHide(rowControl[s]);
        display.refreshControl(rowControl[s]);
    }
}

void cSynthPerformance::update()
{
#if GUI_HANDLE_SYNTH
    for (int s = 0; s < kSynthParts; s++)
    {
        float rmsL = engine.getSynthVoiceRMS(0, s);
        float rmsR = engine.getSynthVoiceRMS(1, s);
        updateSingleRms(rmsTrackers[s][0], rmsL);
        updateSingleRms(rmsTrackers[s][1], rmsR);
    }

    if (uiRefreshTimer < 45)
    {
        return;
    }
    uiRefreshTimer = 0;

    if (mtMenuPopup.isPopupVisible())
    {
        return;
    }

    bool anyRefreshed = false;

    for (int s = 0; s < kSynthParts; s++)
    {
        int16_t prevBarValues[8];
        memcpy(prevBarValues, rowData[s].paramBarValues, sizeof(prevBarValues));
        float prevGain = rowData[s].gainDb;
        float prevPan = rowData[s].panning;

        populateRowData(s);

        bool barChanged = (memcmp(prevBarValues, rowData[s].paramBarValues, sizeof(prevBarValues)) != 0);
        bool gainChanged = (prevGain != rowData[s].gainDb || prevPan != rowData[s].panning);
        bool rmsChanged = (rmsTrackers[s][0].level != rmsTrackers[s][0].lastShownLevel) ||
                          (rmsTrackers[s][1].level != rmsTrackers[s][1].lastShownLevel);

        if (dirtyRow[s] || barChanged || gainChanged || rmsChanged)
        {
            rmsTrackers[s][0].lastShownLevel = rmsTrackers[s][0].level;
            rmsTrackers[s][1].lastShownLevel = rmsTrackers[s][1].level;
            dirtyRow[s] = false;

            display.refreshControl(rowControl[s]);
            anyRefreshed = true;

            if (s == selectedSynth && (barChanged || gainChanged))
            {
                dirtyBottomLabels = true;
            }
        }
    }

    if (dirtyBottomLabels)
    {
        dirtyBottomLabels = false;
        refreshBottomLabels();
        anyRefreshed = true;
    }

    if (anyRefreshed)
    {
        display.synchronizeRefresh();
    }
#endif
}

void cSynthPerformance::onSynthParamChanged(int synthPartIdx)
{
    if (synthPartIdx < 0 || synthPartIdx >= kSynthParts)
    {
        return;
    }
    dirtyRow[synthPartIdx] = true;
    if (synthPartIdx == selectedSynth)
    {
        dirtyBottomLabels = true;
    }
}

void cSynthPerformance::populateRowData(int synthIdx)
{
#if GUI_HANDLE_SYNTH
    if (synthIdx < 0 || synthIdx >= kSynthParts)
    {
        return;
    }

    strSynthPerfRowData& d = rowData[synthIdx];
    const SynthPartSettings& part = mtProject.values.synthPart[synthIdx];

    d.synthIdx = static_cast<uint8_t>(synthIdx);
    d.synthType = static_cast<uint8_t>(part.synthType);
    d.midiChannel = mtConfig.midi.synthMidiChannel[synthIdx];
    d.voiceCount = static_cast<uint8_t>(part.voiceCount);
    d.maxVoiceCount = synth ? static_cast<uint8_t>(synth->getMaxVoiceCount(synthIdx)) : 8;
    d.isRowSelected = (synthIdx == selectedSynth);
    d.selectedCol = selectedCol;
    d.gainDb = engine.getLiveSynthParam(synthIdx, audioEngine::SynthParam::kVolume);
    d.panning = engine.getLiveSynthParam(synthIdx, audioEngine::SynthParam::kPanning);
    d.rmsLevelL = rmsTrackers[synthIdx][0].level;
    d.rmsLevelR = rmsTrackers[synthIdx][1].level;

    if (synth != nullptr)
    {
        polystrncpy(d.patchName, synth->getPatchName(synthIdx).c_str(), sizeof(d.patchName));

        auto& patch = synth->getPatch(synthIdx);
        const auto& metadata = patch.getMetadata();

        int macroId = 0;
        int col = 0;
        for (const auto& macro : metadata.macros)
        {
            if (macroId != *polysynth::Patch::SystemMacroId::kPressure)
            {
                if (macro.displayName.empty())
                {
                    snprintf(
                      d.paramShortNames[col], sizeof(d.paramShortNames[col]), "Macro %d", col + 1);
                }
                else
                {
                    polystrncpy(d.paramShortNames[col],
                                macro.displayName.c_str(),
                                sizeof(d.paramShortNames[col]));
                }

                bool bipolar = macro.bipolar;
                int maxVal = bipolar ? (kCCMidValue * 2) : kCCMaxValue;
                float liveVal = engine.getLiveSynthParam(
                  synthIdx,
                  static_cast<audioEngine::SynthParam>(audioEngine::SynthParam::kMacro1 + col));
                int rawVal = clamp<int>(static_cast<int>(roundf(liveVal)), 0, maxVal);

                d.paramBipolar[col] = bipolar;
                if (bipolar)
                {
                    int dispVal = mapClamped(rawVal, 0, maxVal, -63, 63);
                    snprintf(d.paramValueTexts[col],
                             sizeof(d.paramValueTexts[col]),
                             dispVal > 0 ? "+%d" : "%d",
                             dispVal);
                    d.paramBarValues[col] =
                      static_cast<int16_t>(mapClamped(rawVal, kCCMinValue, maxVal, -100, 100));
                }
                else
                {
                    snprintf(d.paramValueTexts[col], sizeof(d.paramValueTexts[col]), "%d", rawVal);
                    d.paramBarValues[col] =
                      static_cast<int16_t>(mapClamped(rawVal, kCCMinValue, maxVal, 0, 100));
                }

                col++;
                if (col >= kSynthMacrosCount)
                {
                    break;
                }
            }
            macroId++;
        }
    }

    // Column 6: Delay Send
    polystrncpy(d.paramShortNames[6], "Delay", sizeof(d.paramShortNames[6]));
    d.paramBipolar[6] = false;
    float liveDelay = engine.getLiveSynthParam(synthIdx, audioEngine::SynthParam::kDelaySend);
    int delayVal = clamp<int>(static_cast<int>(roundf(liveDelay)), 0, 100);
    d.paramBarValues[6] = static_cast<int16_t>(delayVal);
    snprintf(d.paramValueTexts[6], sizeof(d.paramValueTexts[6]), "%d", delayVal);

    // Column 7: Reverb Send
    polystrncpy(d.paramShortNames[7], "Reverb", sizeof(d.paramShortNames[7]));
    d.paramBipolar[7] = false;
    float liveReverb = engine.getLiveSynthParam(synthIdx, audioEngine::SynthParam::kReverbSend);
    int reverbVal = clamp<int>(static_cast<int>(roundf(liveReverb)), 0, 100);
    d.paramBarValues[7] = static_cast<int16_t>(reverbVal);
    snprintf(d.paramValueTexts[7], sizeof(d.paramValueTexts[7]), "%d", reverbVal);
#else
    (void)synthIdx;
#endif
}

void cSynthPerformance::refreshRow(int synthIdx)
{
    if (synthIdx < 0 || synthIdx >= kSynthParts)
    {
        return;
    }
    populateRowData(synthIdx);
    display.setControlShow(rowControl[synthIdx]);
    display.refreshControl(rowControl[synthIdx]);
}

void cSynthPerformance::refreshBottomLabels()
{
#if GUI_HANDLE_SYNTH
    const SynthPartSettings& part = mtProject.values.synthPart[selectedSynth];

    if (synth != nullptr)
    {
        auto& patch = synth->getPatch(selectedSynth);
        const auto& metadata = patch.getMetadata();

        int macroId = 0;
        int col = 0;
        for (const auto& macro : metadata.macros)
        {
            if (macroId != *polysynth::Patch::SystemMacroId::kPressure)
            {
                snprintf(bottomLabelNames[col],
                         sizeof(bottomLabelNames[col]),
                         "M%d %s",
                         col + 1,
                         macro.displayName.c_str());

                bool bipolar = macro.bipolar;
                int maxVal = bipolar ? (kCCMidValue * 2) : kCCMaxValue;
                float liveVal = engine.getLiveSynthParam(
                  selectedSynth,
                  static_cast<audioEngine::SynthParam>(audioEngine::SynthParam::kMacro1 + col));
                int rawVal = clamp<int>(static_cast<int>(roundf(liveVal)), 0, maxVal);
                int dispVal = bipolar ? mapClamped(rawVal, 0, maxVal, -63, 63) : rawVal;

                snprintf(bottomLabelValues[col], sizeof(bottomLabelValues[col]), "%d ", dispVal);

                col++;
                if (col >= kSynthMacrosCount)
                {
                    break;
                }
            }
            macroId++;
        }
    }

    // Column 6: Delay Send
    snprintf(bottomLabelNames[6], sizeof(bottomLabelNames[6]), "Delay send");
    float liveDelay = engine.getLiveSynthParam(selectedSynth, audioEngine::SynthParam::kDelaySend);
    float delayDb = instrumentEditor.calcInstrumentSendsDecybelValue(
      static_cast<uint8_t>(clamp<int>(static_cast<int>(roundf(liveDelay)), 0, 100)));
    snprintf(bottomLabelValues[6], sizeof(bottomLabelValues[6]), "%0.1f dB", (double)delayDb);

    // Column 7: Reverb Send
    snprintf(bottomLabelNames[7], sizeof(bottomLabelNames[7]), "Reverb send");
    float liveReverb = engine.getLiveSynthParam(selectedSynth, audioEngine::SynthParam::kReverbSend);
    float reverbDb = instrumentEditor.calcInstrumentSendsDecybelValue(
      static_cast<uint8_t>(clamp<int>(static_cast<int>(roundf(liveReverb)), 0, 100)));
    snprintf(bottomLabelValues[7], sizeof(bottomLabelValues[7]), "%0.1f dB", (double)reverbDb);

    for (uint8_t i = 0; i < 8; i++)
    {
        display.setControlColors(label[i], display.getTheme().activeLabelsColors);
        display.setControlValue(label[i], 1);
        display.setControlStyle2(label[i],
                                 controlStyleCenterX | controlStyleCenterY | controlStyleFont2);
        if (i == selectedCol)
        {
            display.setAddControlStyle(label[i], controlStyleBorder);
        }
        else
        {
            display.setRemoveControlStyle(label[i], controlStyleBorder);
        }
        display.setControlText(label[i], bottomLabelNames[i]);
        display.setControlText2(label[i], bottomLabelValues[i]);
        display.setControlShow(label[i]);
        display.refreshControl(label[i]);
    }

    display.setControlValue(bgLabel, 255);
    display.setControlShow(bgLabel);
    display.refreshControl(bgLabel);
#endif
}

void cSynthPerformance::showFullScreen()
{
    titleBar->setTitle("Synth Performance");
    titleBar->showCurrentInstrumentOnRightLabel();
    titleBar->show(true);

    for (int s = 0; s < kSynthParts; s++)
    {
        refreshRow(s);
    }

    refreshBottomLabels();
    display.synchronizeRefresh();
}

void cSynthPerformance::selectSynthRow(int synthIdx)
{
    if (kSynthParts <= 0)
    {
        return;
    }

    int clamped = clamp<int>(synthIdx, 0, kSynthParts - 1);
    if (clamped == selectedSynth)
    {
        return;
    }

    int prevSynth = selectedSynth;
    selectedSynth = static_cast<uint8_t>(clamped);
    mtProject.values.lastUsedInstrument = SYNTH_INSTRUMENTS_START_IDX + selectedSynth;

    titleBar->showCurrentInstrumentOnRightLabel();
    lightUpPadBoard();

    refreshRow(prevSynth);
    refreshRow(selectedSynth);
    refreshBottomLabels();
    display.synchronizeRefresh();
}

void cSynthPerformance::selectColumn(int colIdx)
{
    int clamped = clamp<int>(colIdx, 0, 7);
    if (clamped == selectedCol)
    {
        return;
    }

    selectedCol = static_cast<uint8_t>(clamped);
    refreshRow(selectedSynth);
    refreshBottomLabels();
    display.synchronizeRefresh();
}

void cSynthPerformance::changeSelectedParam(int16_t delta)
{
#if GUI_HANDLE_SYNTH
    SynthPartSettings& part = mtProject.values.synthPart[selectedSynth];

    if (selectedCol < kSynthMacrosCount)
    {
        int maxVal = instrumentEditor.getMacroMax(selectedSynth, selectedCol);
        part.macroValue[selectedCol] =
          clamp<int>(static_cast<int>(part.macroValue[selectedCol]) + delta, kCCMinValue, maxVal);
        instrumentEditor.setSynthMacro(
          selectedSynth, selectedCol, static_cast<int>(part.macroValue[selectedCol]), false);
    }
    else if (selectedCol == 6)
    {
        part.delaySend = clamp<float>(part.delaySend + delta, 0.0f, 100.0f);
        instrumentEditor.setSynthDelaySend(selectedSynth, part.delaySend, false);
    }
    else if (selectedCol == 7)
    {
        part.reverbSend = clamp<float>(part.reverbSend + delta, 0.0f, 100.0f);
        instrumentEditor.setSynthReverbSend(selectedSynth, part.reverbSend, false);
    }

    refreshRow(selectedSynth);
    refreshBottomLabels();
    display.synchronizeRefresh();
#else
    (void)delta;
#endif
}

void cSynthPerformance::changeSelectedSynthGain(int16_t delta)
{
#if GUI_HANDLE_SYNTH
    SynthPartSettings& part = mtProject.values.synthPart[selectedSynth];
    part.gainDb =
      clamp<float>(part.gainDb + (delta * 0.5f), Synth::kGainSilence, Synth::kGainMax);
    instrumentEditor.setSynthGain(selectedSynth, part.gainDb, false);

    refreshRow(selectedSynth);
    display.synchronizeRefresh();
#else
    (void)delta;
#endif
}

void cSynthPerformance::resetSelectedParamToDefault()
{
#if GUI_HANDLE_SYNTH
    SynthPartSettings& part = mtProject.values.synthPart[selectedSynth];

    if (selectedCol < kSynthMacrosCount)
    {
        part.macroValue[selectedCol] =
          instrumentEditor.isMacroBipolar(selectedSynth, selectedCol) ? kCCMidValue : kCCMinValue;
        instrumentEditor.setSynthMacro(
          selectedSynth, selectedCol, static_cast<int>(part.macroValue[selectedCol]), false);
    }
    else if (selectedCol == 6)
    {
        part.delaySend = Synth::kSendDefaultValue;
        instrumentEditor.setSynthDelaySend(selectedSynth, part.delaySend, false);
    }
    else if (selectedCol == 7)
    {
        part.reverbSend = Synth::kSendDefaultValue;
        instrumentEditor.setSynthReverbSend(selectedSynth, part.reverbSend, false);
    }

    refreshRow(selectedSynth);
    refreshBottomLabels();
    display.synchronizeRefresh();
#endif
}

void cSynthPerformance::lightUpPadBoard()
{
    padsBacklight.clearGlobalPads(0, 1, 1);
#if GUI_HANDLE_SYNTH
    if (Synth::kSynthTypes.at(mtProject.values.synthPart[selectedSynth].synthType) ==
        polysynth::InstrumentTypeId::kDrumSynth)
    {
        for (int padIdx = 0; padIdx < 48; padIdx++)
        {
            padsBacklight.setBackLayer(
              instrumentEditor.isSynthPercSectionBegin(mtPadBoard.getNoteFromPad(padIdx)) ? 1 : 0,
              mtConfig.values.padsLightBack,
              padIdx);
        }
    }
#endif
}

void cSynthPerformance::setScreenFunctions()
{
    ip->clearAllButtons();
    ip->clearAllPots();

    // Module switching buttons
    ip->setButtonObj(interfaceButtonParams, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonPerformance, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonFile, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonConfig, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonMaster, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSamplePlay, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSampleEdit, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSampleRec, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSampleLoad, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSong, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonPattern, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonGame, buttonPress, functSwitchModule);

    ip->setButtonObj(interfaceButtonMiniMenu, functMenu);
    ip->setButtonObj(interfaceButtonPlay, buttonPress, functPlayAction);

    // 8 Screen buttons
    for (uint8_t i = interfaceButton0; i <= interfaceButton7; i++)
    {
        ip->setButtonObj(i, buttonPress, functScreenButton);
    }

    // Navigation & editing
    ip->setPotObj(interfacePot0, functEncoder);
    ip->setButtonObj(interfaceButtonUp, buttonPress, functUp);
    ip->setButtonObj(interfaceButtonDown, buttonPress, functDown);
    ip->setButtonObj(interfaceButtonLeft, buttonPress, functLeft);
    ip->setButtonObj(interfaceButtonRight, buttonPress, functRight);
    ip->setButtonObj(interfaceButtonDelete, buttonPress, functDelete);
    ip->setButtonObj(interfaceButtonInstr, functInstr);

    // Note preview
    ip->setButtonObj(interfaceButtonMiniMiddle, functMiniMiddle);
    ip->setPadsGlobal(functPads);
}

static void functSwitchModule(uint8_t button)
{
    if (!performanceMode.canPerformanceModeContinueAfterSwitchingTo(button))
    {
        performanceMode.turnOffAtEndOfPattern();
    }

    SP->eventFunct(eventSwitchModule, SP, &button, 0);
}

static void functMenu(uint8_t state)
{
    if (state == buttonPress)
    {
        mtMenuPopup.showModulePopup(SP);
    }
    else if (state == buttonRelease)
    {
        mtMenuPopup.hideModulePopup(SP);
        SP->showFullScreen();
    }
}

static void functPlayAction()
{
    if (sequencer.getSeqState() == Sequencer::SEQ_STATE_STOP)
    {
        if (SP->buttons->isButtonPressed(interfaceButtonShift))
        {
            sequencer.playSong();
        }
        else
        {
            sequencer.playPattern();
        }
    }
    else
    {
        sequencer.stop();
    }
}

static void functScreenButton(uint8_t button)
{
    if (button > 7)
    {
        return;
    }

    if (SP->selectedCol == button)
    {
        // Pressing the already-selected column button cycles through Synth 1 -> 2 -> 3
#if GUI_HANDLE_SYNTH
        if (kSynthParts > 0)
        {
            SP->selectSynthRow((SP->selectedSynth + 1) % (kSynthParts > 0 ? kSynthParts : 1));
        }
#endif
    }
    else
    {
        SP->selectColumn(button);
    }
}

static void functEncoder(int16_t value)
{
    if (SP->buttons->isButtonPressed(interfaceButtonShift))
    {
        SP->changeSelectedSynthGain(value);
    }
    else
    {
        SP->changeSelectedParam(value);
    }
}

static void functUp()
{
    if (SP->buttons->isButtonPressed(interfaceButtonShift))
    {
        SP->changeSelectedParam(1);
    }
    else
    {
        SP->selectSynthRow(static_cast<int>(SP->selectedSynth) - 1);
    }
}

static void functDown()
{
    if (SP->buttons->isButtonPressed(interfaceButtonShift))
    {
        SP->changeSelectedParam(-1);
    }
    else
    {
        SP->selectSynthRow(static_cast<int>(SP->selectedSynth) + 1);
    }
}

static void functLeft()
{
    SP->selectColumn(static_cast<int>(SP->selectedCol) - 1);
}

static void functRight()
{
    SP->selectColumn(static_cast<int>(SP->selectedCol) + 1);
}

static void functDelete()
{
    SP->resetSelectedParamToDefault();
}

static void functInstr(uint8_t state)
{
    if (state == buttonPress)
    {
#if GUI_HANDLE_SYNTH
        if (kSynthParts > 0)
        {
            SP->selectSynthRow((SP->selectedSynth + 1) % (kSynthParts > 0 ? kSynthParts : 1));
        }
#endif
    }
}

static void functMiniMiddle(uint8_t state)
{
#if GUI_HANDLE_SYNTH
    instrumentEditor.synthCommonPreviewButtonFunct(state);
#else
    (void)state;
#endif
}

static void functPads(uint8_t pad, uint8_t state, int16_t velo)
{
    (void)velo;
#if GUI_HANDLE_SYNTH
    instrumentEditor.playSynthNoteByPad(pad, state);
#else
    (void)pad;
    (void)state;
#endif
}
