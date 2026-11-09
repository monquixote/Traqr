#ifndef LIBS_NEWDISPLAY_SYNTHPERFCONTROL_H_
#define LIBS_NEWDISPLAY_SYNTHPERFCONTROL_H_

#include "displayControls.h"

struct strSynthPerfRowData
{
    uint8_t synthIdx = 0;
    uint8_t synthType = 0;
    uint8_t midiChannel = 0;
    uint8_t voiceCount = 0;
    uint8_t maxVoiceCount = 0;
    bool isRowSelected = false;
    uint8_t selectedCol = 0;
    float gainDb = 0.0f;
    float panning = 0.0f;
    uint8_t rmsLevelL = 0;
    uint8_t rmsLevelR = 0;
    char patchName[40] = { 0 };
    char paramShortNames[8][14] = { { 0 } };
    char paramValueTexts[8][12] = { { 0 } };
    int16_t paramBarValues[8] = { 0 };
    bool paramBipolar[8] = { false };
};

class cSynthPerfRow : public cDisplayControl
{
  public:
    cSynthPerfRow(strControlProperties* properties = nullptr);
    virtual ~cSynthPerfRow();

    virtual void update();

    virtual void setStyle(uint32_t style);
    virtual void setText(const char* text);
    virtual void setValue(int value);
    virtual void setColors(const uint32_t* colors);
    virtual void setData(void* data);

    strSynthPerfRowData* data = nullptr;
};

#endif /* LIBS_NEWDISPLAY_SYNTHPERFCONTROL_H_ */
