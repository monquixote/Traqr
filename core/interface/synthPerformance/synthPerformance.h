#ifndef INTERFACE_SYNTHPERFORMANCE_H_
#define INTERFACE_SYNTHPERFORMANCE_H_

#include "core/modulesBase.h"
#include "mtStructs.h"
#include "synthPerfControl.h"

#if GUI_HANDLE_SYNTH
#include "Synth.h"
#endif

class cSynthPerformance : public ModuleBase
{
  public:
    cSynthPerformance() = default;
    virtual ~cSynthPerformance() = default;

    virtual void update() override;
    virtual void start(uint32_t options) override;
    virtual void stop() override;

    virtual void initDisplayControls() override;
    virtual void destroyDisplayControls() override {}
    virtual void hideControlsUnderMiniMenu() override;
    virtual uint8_t getCurrentModeIdx() override { return 0; }

    void onSynthParamChanged(int synthPartIdx);

    void selectSynthRow(int synthIdx);
    void selectColumn(int colIdx);
    void changeSelectedParam(int16_t delta);
    void resetSelectedParamToDefault();
    void changeSelectedSynthGain(int16_t delta);

    void populateRowData(int synthIdx);
    void refreshRow(int synthIdx);
    void refreshBottomLabels();
    void showFullScreen();
    void setScreenFunctions();
    void lightUpPadBoard();

    struct RmsTracker
    {
        float sum = 0.0f;
        uint8_t count = 0;
        uint8_t level = 0;
        uint8_t lastShownLevel = 0;
        elapsedMillis decayTimer;
    };

    uint8_t selectedSynth = 0;
    uint8_t selectedCol = 0;

#if GUI_HANDLE_SYNTH
    hControl rowControl[kSynthParts] = { -1, -1, -1 };
    strSynthPerfRowData rowData[kSynthParts];
#else
    hControl rowControl[1] = { -1 };
    strSynthPerfRowData rowData[1];
#endif

    hControl label[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    hControl bgLabel = -1;

    char bottomLabelNames[8][24] = { { 0 } };
    char bottomLabelValues[8][16] = { { 0 } };

#if GUI_HANDLE_SYNTH
    bool dirtyRow[kSynthParts] = { false, false, false };
    bool dirtyBottomLabels = false;

    RmsTracker rmsTrackers[kSynthParts][2];
#else
    bool dirtyRow[1] = { false };
    bool dirtyBottomLabels = false;

    RmsTracker rmsTrackers[1][2];
#endif
    elapsedMillis uiRefreshTimer;
};

extern cSynthPerformance synthPerformance;

#endif /* INTERFACE_SYNTHPERFORMANCE_H_ */
