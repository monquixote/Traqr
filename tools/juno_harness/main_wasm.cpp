#include "JunoDSP.h"
#include <emscripten/emscripten.h>

static juno::JunoDSP g_synth;

extern "C" {

EMSCRIPTEN_KEEPALIVE void juno_init(float sampleRate) { g_synth.init(sampleRate); }
EMSCRIPTEN_KEEPALIVE void juno_reset() { g_synth.reset(); }
EMSCRIPTEN_KEEPALIVE void juno_note_on(int note, float velocity) { g_synth.noteOn(note, velocity); }
EMSCRIPTEN_KEEPALIVE void juno_note_off(int note) { g_synth.noteOff(note); }
EMSCRIPTEN_KEEPALIVE void juno_all_notes_off() { g_synth.allNotesOff(); }
EMSCRIPTEN_KEEPALIVE void juno_set_param(int id, float v) { g_synth.setParam(id, v); }
EMSCRIPTEN_KEEPALIVE float juno_get_param(int id) { return g_synth.getParam(id); }
EMSCRIPTEN_KEEPALIVE int juno_active_voices() { return g_synth.activeVoices(); }
EMSCRIPTEN_KEEPALIVE int juno_param_count() { return juno::P_COUNT; }
EMSCRIPTEN_KEEPALIVE float juno_param_min(int id) { return juno::kParamInfo[id].minV; }
EMSCRIPTEN_KEEPALIVE float juno_param_max(int id) { return juno::kParamInfo[id].maxV; }
EMSCRIPTEN_KEEPALIVE float juno_param_default(int id) { return juno::kParamInfo[id].defV; }
EMSCRIPTEN_KEEPALIVE void juno_render(float* left, float* right, int numSamples) {
    g_synth.processBlock(left, right, numSamples);
}

}
