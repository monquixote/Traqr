#include "FT812.h"

#include "display.h"
#include "synthPerfControl.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static const char* const kPerfSynthModelNames[8] = {
    "ACD", "FAT", "VAP", "WTFM", "PERC", "DIRT", "CHIP", "TEAR"
};

cSynthPerfRow::cSynthPerfRow(strControlProperties* properties)
  : cDisplayControl(controlIdSynthPerfRow, 1)
{
    colorsCount = 5;
    colors = display.getTheme().activeBarColors;

    if (properties == nullptr)
    {
        posX = 0;
        posY = 0;
        text = nullptr;
        data = nullptr;
        value = 0;
        width = 0;
        height = 0;
        style = 0;
        return;
    }

    colors = (properties->colors == nullptr) ? colors : properties->colors;

    posX = properties->x;
    posY = properties->y;

    text = properties->text;
    value = properties->value;

    width = properties->w;
    height = properties->h;

    data = static_cast<strSynthPerfRowData*>(properties->data);

    setStyle(properties->style);
}

cSynthPerfRow::~cSynthPerfRow()
{
}

void cSynthPerfRow::setStyle(uint32_t style)
{
    this->style = style;
}

void cSynthPerfRow::setText(const char* text)
{
    this->text = text;
}

void cSynthPerfRow::setValue(int value)
{
    this->value = value;
}

void cSynthPerfRow::setColors(const uint32_t* colors)
{
    this->colors = colors;
}

void cSynthPerfRow::setData(void* data)
{
    this->data = static_cast<strSynthPerfRowData*>(data);
}

void cSynthPerfRow::update()
{
    if (data == nullptr || !(style & controlStyleShow))
    {
        return;
    }

    const Theme& theme = display.getTheme();

    const uint16_t headerH = 24;
    const uint16_t cellY = posY + headerH + 2;
    const uint16_t cellH = (height > headerH + 2) ? (height - headerH - 2) : 100;
    const uint16_t barTopOffset = 5;
    const uint16_t barH = (cellH > 42) ? (cellH - 42) : 56;

    const strFont* font12 = &fonts[4];      // 12 Regular
    const strFont* font14 = &fonts[0];      // 14 Regular
    const strFont* font14Bold = &fonts[2];  // 14 Bold
    const strFont* font17Bold = &fonts[3];  // 17 Bold

    API_SAVE_CONTEXT();
    API_LINE_WIDTH(16);

    // 1. Header background & 8 cell backgrounds + bar containers + bar fills in one RECTS pass
    API_BEGIN(RECTS);

    // Header background
    API_COLOR(data->isRowSelected ? theme.activeLabelsColors[1] : theme.topBarLabelColors[1]);
    API_VERTEX2F(posX + 1, posY);
    API_VERTEX2F(posX + width - 2, posY + headerH);

    // Left accent stripe on header if row is selected
    if (data->isRowSelected)
    {
        API_COLOR(theme.frameColors[0]);
        API_VERTEX2F(posX + 1, posY);
        API_VERTEX2F(posX + 5, posY + headerH);
    }

    // Stereo RMS meter background in header
    const uint16_t meterX = posX + 696;
    const uint16_t meterW = 94;
    API_COLOR(theme.activeBarColors[3]);
    API_VERTEX2F(meterX, posY + 5);
    API_VERTEX2F(meterX + meterW, posY + 11);
    API_VERTEX2F(meterX, posY + 13);
    API_VERTEX2F(meterX + meterW, posY + 19);

    // Stereo RMS meter fill (L and R)
    uint8_t rmsL = data->rmsLevelL > 100 ? 100 : data->rmsLevelL;
    uint8_t rmsR = data->rmsLevelR > 100 ? 100 : data->rmsLevelR;
    if (rmsL > 0)
    {
        uint16_t fillW = (meterW * rmsL) / 100;
        if (fillW < 1)
            fillW = 1;
        API_COLOR(rmsL >= 88 ? theme.red : theme.green);
        API_VERTEX2F(meterX, posY + 5);
        API_VERTEX2F(meterX + fillW, posY + 11);
    }
    if (rmsR > 0)
    {
        uint16_t fillW = (meterW * rmsR) / 100;
        if (fillW < 1)
            fillW = 1;
        API_COLOR(rmsR >= 88 ? theme.red : theme.green);
        API_VERTEX2F(meterX, posY + 13);
        API_VERTEX2F(meterX + fillW, posY + 19);
    }

    // 8 Parameter cells: backgrounds, bar containers, and bar fills
    for (uint8_t col = 0; col < 8; col++)
    {
        uint16_t cellX = posX + col * 100 + 1;
        uint16_t cellW = 97;

        // Cell background
        API_COLOR(theme.activeBarColors[4]);
        API_VERTEX2F(cellX, cellY);
        API_VERTEX2F(cellX + cellW, cellY + cellH);

        // Bar container
        uint16_t barX = cellX + 10;
        uint16_t barW = cellW - 20;
        uint16_t barY = cellY + barTopOffset;

        API_COLOR(theme.activeBarColors[3]);
        API_VERTEX2F(barX, barY);
        API_VERTEX2F(barX + barW, barY + barH);

        // Bar fill
        uint32_t fillColor =
          data->isRowSelected ? theme.activeBarColors[0] : theme.activeLabelsColors[0];
        API_COLOR(fillColor);

        int16_t val = data->paramBarValues[col];
        if (data->paramBipolar[col])
        {
            if (val < -100)
                val = -100;
            if (val > 100)
                val = 100;
            uint16_t halfH = barH / 2;
            uint16_t topY = barY + (val > 0 ? ((halfH * (100 - val)) / 100) : halfH);
            uint16_t botY = barY + barH - (val < 0 ? ((halfH * (100 + val)) / 100) : halfH);
            if (botY <= topY)
            {
                topY = barY + halfH - 1;
                botY = barY + halfH + 1;
            }
            API_VERTEX2F(barX + 1, topY);
            API_VERTEX2F(barX + barW - 1, botY);
        }
        else
        {
            if (val < 0)
                val = 0;
            if (val > 100)
                val = 100;
            if (val > 0)
            {
                uint16_t fillH = (barH * val) / 100;
                if (fillH < 1)
                    fillH = 1;
                API_VERTEX2F(barX + 1, barY + barH - fillH);
                API_VERTEX2F(barX + barW - 1, barY + barH);
            }
        }
    }

    API_END();

    // 2. Header Text
    char headerBuf[48];
    const char* engineName =
      (data->synthType < 8) ? kPerfSynthModelNames[data->synthType] : "SYN";
    if (data->voiceCount == 0)
    {
        snprintf(headerBuf, sizeof(headerBuf), "SYNTH %d [%s OFF]", data->synthIdx + 1, engineName);
    }
    else
    {
        snprintf(headerBuf, sizeof(headerBuf), "SYNTH %d [%s]", data->synthIdx + 1, engineName);
    }

    API_COLOR(data->isRowSelected ? theme.white : theme.activeLabelsColors[0]);
    API_CMD_TEXT(posX + 10, posY + headerH / 2, font14Bold->handle, OPT_CENTERY, headerBuf);

    // Patch Name
    if (data->patchName[0] != '\0')
    {
        API_COLOR(theme.white);
        uint16_t maxPatchLen =
          getMaxTextLengthThatFitSpecificWidth(font17Bold, data->patchName, 255);
        API_CMD_TEXT(posX + 162,
                     posY + headerH / 2,
                     font17Bold->handle,
                     OPT_CENTERY,
                     data->patchName,
                     maxPatchLen);
    }

    // MIDI Channel badge
    if (data->midiChannel > 0 && data->midiChannel <= 16)
    {
        snprintf(headerBuf, sizeof(headerBuf), "MIDI: CH%d", data->midiChannel);
        API_COLOR(theme.green);
    }
    else
    {
        snprintf(headerBuf, sizeof(headerBuf), "MIDI: Off");
        API_COLOR(theme.activeLabelsColors[0]);
    }
    API_CMD_TEXT(posX + 430, posY + headerH / 2, font14Bold->handle, OPT_CENTERY, headerBuf);

    // Pan & Gain summary
    int panInt = static_cast<int>(roundf(data->panning));
    char panStr[12];
    if (panInt == 0)
    {
        snprintf(panStr, sizeof(panStr), "C");
    }
    else if (panInt < 0)
    {
        snprintf(panStr, sizeof(panStr), "L%d", -panInt);
    }
    else
    {
        snprintf(panStr, sizeof(panStr), "R%d", panInt);
    }
    snprintf(headerBuf, sizeof(headerBuf), "Pan:%s  %0.1fdB", panStr, (double)data->gainDb);
    API_COLOR(theme.activeLabelsColors[0]);
    API_CMD_TEXT(posX + 540, posY + headerH / 2, font14->handle, OPT_CENTERY, headerBuf);

    // 3. Parameter Cell Value Texts and Short Names
    API_COLOR(theme.white);
    for (uint8_t col = 0; col < 8; col++)
    {
        uint16_t centerX = posX + col * 100 + 50;
        if (data->paramValueTexts[col][0] != '\0')
        {
            API_CMD_TEXT(centerX,
                         cellY + barTopOffset + barH + 9,
                         font14->handle,
                         OPT_CENTERX | OPT_CENTERY,
                         data->paramValueTexts[col]);
        }
    }

    API_COLOR(data->isRowSelected ? theme.activeLabelsColors[0] : theme.inactiveLabelsColors[3]);
    for (uint8_t col = 0; col < 8; col++)
    {
        uint16_t centerX = posX + col * 100 + 50;
        if (data->paramShortNames[col][0] != '\0')
        {
            uint16_t maxLen =
              getMaxTextLengthThatFitSpecificWidth(font12, data->paramShortNames[col], 90);
            API_CMD_TEXT(centerX,
                         cellY + cellH - 10,
                         font12->handle,
                         OPT_CENTERX | OPT_CENTERY,
                         data->paramShortNames[col],
                         maxLen);
        }
    }

    // 4. Selection Frame on Active Row & Active Column Cell
    if (data->isRowSelected && data->selectedCol < 8)
    {
        uint16_t selX = posX + data->selectedCol * 100 + 1;
        uint16_t selW = 97;

        API_COLOR(theme.frameColors[0]);
        API_LINE_WIDTH(kRedFrameWidth);
        API_BEGIN(LINE_STRIP);
        API_VERTEX2F(selX, cellY);
        API_VERTEX2F(selX + selW, cellY);
        API_VERTEX2F(selX + selW, cellY + cellH - 1);
        API_VERTEX2F(selX, cellY + cellH - 1);
        API_VERTEX2F(selX, cellY);
        API_END();
    }

    API_RESTORE_CONTEXT();
}
