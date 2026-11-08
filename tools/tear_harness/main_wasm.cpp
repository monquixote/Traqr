#include "TearDSP.h"
#include <emscripten/emscripten.h>

static tear::TearDSP g_synth;

extern "C" {

EMSCRIPTEN_KEEPALIVE
void tear_init(float sampleRate) {
    g_synth.init(sampleRate);
}

EMSCRIPTEN_KEEPALIVE
void tear_reset() {
    g_synth.reset();
}

EMSCRIPTEN_KEEPALIVE
void tear_note_on(int note, float velocity) {
    g_synth.noteOn(static_cast<uint8_t>(note), velocity);
}

EMSCRIPTEN_KEEPALIVE
void tear_note_off() {
    g_synth.noteOff();
}

EMSCRIPTEN_KEEPALIVE
void tear_set_param(int paramId, float value) {
    g_synth.setParam(static_cast<uint8_t>(paramId), value);
}

EMSCRIPTEN_KEEPALIVE
float tear_get_param(int paramId) {
    return g_synth.getParam(static_cast<uint8_t>(paramId));
}

EMSCRIPTEN_KEEPALIVE
void tear_render(float* outputBuffer, int numSamples) {
    g_synth.processBlock(outputBuffer, numSamples);
}

EMSCRIPTEN_KEEPALIVE
int tear_is_playing() {
    return g_synth.isPlaying() ? 1 : 0;
}

}
