#ifndef LIBS_AUDIO_EFFECT_CHORUS_H_
#define LIBS_AUDIO_EFFECT_CHORUS_H_

#include "AudioStream.h"

class AudioEffectChorus : public AudioStream
{
  public:
    static constexpr uint32_t kBufferSize = 2048;
    static constexpr uint32_t kBufferMask = kBufferSize - 1;
    static constexpr uint32_t kMaxActiveTailSamples = 512;

    AudioEffectChorus(void);

    void begin(uint8_t rate, uint8_t depth, uint8_t width);
    void setRate(uint8_t rate);
    void setDepth(uint8_t depth);
    void setWidth(uint8_t width);
    void clear(void);
    void blockUpdate(void) { isBlockedUpdate = true; }
    void unblockUpdate(void) { isBlockedUpdate = false; }
    bool isSilent(void) const { return silentSamplesCount >= kMaxActiveTailSamples; }

    virtual void update(void);

  private:
    audio_block_t* inputQueueArray[1];
    uint32_t headIdx = 0;
    uint32_t silentSamplesCount = kMaxActiveTailSamples;
    float lfoPhase = 0.0f;
    float lfoPhaseInc = 0.0f;
    float modDepthSamples = 70.0f;
    float widthMain = 0.75f;
    float widthCross = 0.25f;
    float lpLeft = 0.0f;
    float lpRight = 0.0f;
    volatile bool isBlockedUpdate = true;
};

#endif /* LIBS_AUDIO_EFFECT_CHORUS_H_ */
