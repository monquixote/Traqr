#include "WavetableDSP.h"
#include <emscripten/emscripten.h>

static wt::WavetableDSP g_synth;

extern "C" {

EMSCRIPTEN_KEEPALIVE void wt_init(float sampleRate) { g_synth.init(sampleRate); }
EMSCRIPTEN_KEEPALIVE void wt_reset() { g_synth.reset(); }
EMSCRIPTEN_KEEPALIVE void wt_note_on(int note, float velocity) { g_synth.noteOn(note, velocity); }
EMSCRIPTEN_KEEPALIVE void wt_note_off(int note) { g_synth.noteOff(note); }
EMSCRIPTEN_KEEPALIVE void wt_all_notes_off() { g_synth.allNotesOff(); }
EMSCRIPTEN_KEEPALIVE void wt_set_param(int id, float v) { g_synth.setParam(id, v); }
EMSCRIPTEN_KEEPALIVE float wt_get_param(int id) { return g_synth.getParam(id); }
EMSCRIPTEN_KEEPALIVE int wt_active_voices() { return g_synth.activeVoices(); }
EMSCRIPTEN_KEEPALIVE int wt_param_count() { return wt::P_COUNT; }
EMSCRIPTEN_KEEPALIVE float wt_param_min(int id) { return wt::kParamInfo[id].minV; }
EMSCRIPTEN_KEEPALIVE float wt_param_max(int id) { return wt::kParamInfo[id].maxV; }
EMSCRIPTEN_KEEPALIVE float wt_param_default(int id) { return wt::kParamInfo[id].defV; }

EMSCRIPTEN_KEEPALIVE void wt_render(float* left, float* right, int numSamples)
{
    g_synth.processBlock(left, right, numSamples);
}

EMSCRIPTEN_KEEPALIVE int wt_load_pcm16(const int16_t* samples, int numSamples, int windowSize)
{
    return g_synth.loadPcm16(samples, numSamples, windowSize) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int wt_get_window_size() { return g_synth.getWindowSize(); }
EMSCRIPTEN_KEEPALIVE int wt_get_window_count() { return g_synth.getWindowCount(); }
EMSCRIPTEN_KEEPALIVE int wt_get_total_samples() { return g_synth.getTotalTableSamples(); }

EMSCRIPTEN_KEEPALIVE void wt_get_display_wave(float* outBuf, int numPoints)
{
    g_synth.getDisplayWave(outBuf, numPoints);
}

EMSCRIPTEN_KEEPALIVE void wt_get_env_preview(float* outBuf, int numPoints)
{
    g_synth.getEnvPreview(outBuf, numPoints);
}

EMSCRIPTEN_KEEPALIVE int wt_get_mod_state(float* out3)
{
    return g_synth.getModState(out3);
}

EMSCRIPTEN_KEEPALIVE int wt_export_pti_size() { return g_synth.getExportPtiSize(); }

EMSCRIPTEN_KEEPALIVE int wt_export_pti(uint8_t* outBuf, int maxLen, const char* name)
{
    return g_synth.exportPti(outBuf, maxLen, name);
}

EMSCRIPTEN_KEEPALIVE int wt_import_pti(const uint8_t* data, int byteLen)
{
    return g_synth.importPti(data, byteLen) ? 1 : 0;
}

}
