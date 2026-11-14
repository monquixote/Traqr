const fs = require('fs');
const path = require('path');
const vm = require('vm');

const html = fs.readFileSync(path.join(__dirname, 'juno_harness/index.html'), 'utf8');
const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>\s*<\/body>/);
if (!scriptMatch) throw new Error('Script block not found');

const mockElement = () => ({
    addEventListener: () => {},
    append: () => {},
    querySelector: () => null,
    querySelectorAll: () => [],
    style: {},
    classList: { add: () => {}, remove: () => {} },
    getContext: () => ({ clearRect: () => {}, beginPath: () => {}, stroke: () => {}, moveTo: () => {}, lineTo: () => {} })
});

const sandbox = {
    console,
    TextEncoder,
    ArrayBuffer,
    Uint8Array,
    Uint32Array,
    Float32Array,
    DataView,
    document: {
        getElementById: mockElement,
        createElement: mockElement
    },
    window: {
        addEventListener: () => {}
    },
    navigator: {},
    JunoModule: () => Promise.resolve({})
};

vm.createContext(sandbox);

const P_DEFS = {
    octave: 1.0, tune: 0.0, saw: 0.8, pulse: 0.0, pw: 0.5, pwmDepth: 0.3, pwmSrc: 1.0,
    sub: 0.2, subOct: 0.0, noise: 0.0, drift: 0.35, hpf: 0.0, cutoff: 0.62, res: 0.15,
    fenv: 0.4, flfo: 0.0, keytrack: 0.5, ota: 0.25, fA: 0.005, fD: 0.6, fS: 0.3,
    fR: 0.4, aA: 0.01, aD: 0.5, aS: 0.8, aR: 0.4, vca: 0.0, lfoRate: 5.0, lfoDelay: 0.0,
    lfoPitch: 0.0, chorus: 1.0, volume: 0.9
};

const scriptCode = `
${scriptMatch[1]}

globalThis.extracted = {
    KEYS,
    ID,
    NPARAMS,
    INSTRUMENT_TYPE_ID,
    PRESETS,
    DEFAULT_MACROS,
    encodePatchBinary,
    createZip,
    safe
};
`;

vm.runInContext(scriptCode, sandbox);
const { KEYS, ID, NPARAMS, INSTRUMENT_TYPE_ID, PRESETS, DEFAULT_MACROS, encodePatchBinary, createZip, safe } = sandbox.extracted;

function paramsFromPreset(p) {
    const out = new Float32Array(NPARAMS);
    for (let i = 0; i < NPARAMS; i++) {
        const k = KEYS[i];
        out[i] = (p[k] !== undefined) ? p[k] : P_DEFS[k];
    }
    return out;
}

const outDir = path.resolve(__dirname, '../patches/JULYO');
fs.mkdirSync(outDir, { recursive: true });

const filesForZip = [];
let count = 0;

for (const [key, p] of Object.entries(PRESETS)) {
    const filename = `${safe(p.name)}.patch`;
    const params = paramsFromPreset(p);
    const macros = p.macros || DEFAULT_MACROS();
    const data = encodePatchBinary(params, macros);
    
    const filePath = path.join(outDir, filename);
    fs.writeFileSync(filePath, Buffer.from(data));
    filesForZip.push({ name: `JULYO/${filename}`, data });
    count++;
    console.log(`Generated: ${filename} (${data.length} bytes)`);
}

const zipData = createZip(filesForZip);
const zipPath = path.resolve(__dirname, '../patches/JULYO_Patches.zip');
fs.writeFileSync(zipPath, Buffer.from(zipData));
console.log(`\nSuccessfully exported ${count} patches to ${outDir}`);
console.log(`Successfully created ZIP: ${zipPath} (${zipData.length} bytes)`);
