const fs = require('fs');
const path = require('path');

// Test runner for TEAR WASM DSP
const ModuleFactory = require('./tear_dsp.js');

ModuleFactory().then((Module) => {
    console.log("WASM loaded successfully!");

    const tear_init = Module.cwrap('tear_init', null, ['number']);
    const tear_note_on = Module.cwrap('tear_note_on', null, ['number', 'number']);
    const tear_note_off = Module.cwrap('tear_note_off', null, []);
    const tear_set_param = Module.cwrap('tear_set_param', null, ['number', 'number']);
    const tear_render = Module.cwrap('tear_render', null, ['number', 'number']);
    const tear_is_playing = Module.cwrap('tear_is_playing', 'number', []);

    tear_init(44100.0);
    console.log("tear_init(44100) OK");

    const BUFFER_SIZE = 256;
    const ptr = Module._malloc(BUFFER_SIZE * 4);

    // Test each sync mode
    for (let mode = 0; mode < 5; mode++) {
        tear_set_param(1 /* SYNC_MODE */, mode);
        tear_set_param(0 /* SYNC_RATIO */, 12.0); // +12 st
        tear_note_on(60, 0.9); // Middle C

        tear_render(ptr, BUFFER_SIZE);
        const samples = Module.HEAPF32.subarray(ptr >> 2, (ptr >> 2) + BUFFER_SIZE);

        let hasNaN = false;
        let peak = 0;
        for (let i = 0; i < BUFFER_SIZE; i++) {
            if (isNaN(samples[i]) || !isFinite(samples[i])) {
                hasNaN = true;
            }
            if (Math.abs(samples[i]) > peak) peak = Math.abs(samples[i]);
        }

        console.log(`Sync Mode ${mode}: peak=${peak.toFixed(4)}, hasNaN=${hasNaN}`);
        if (hasNaN) {
            console.error(`ERROR: NaN or Inf detected in mode ${mode}`);
            process.exit(1);
        }
    }

    tear_note_off();
    tear_render(ptr, BUFFER_SIZE);
    console.log("Note off processed OK. All DSP verification checks PASSED!");
    Module._free(ptr);
    process.exit(0);
}).catch(err => {
    console.error("Failed to load WASM:", err);
    process.exit(1);
});
