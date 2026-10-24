// Loads simulator/pmc.wasm in node: the same calls the page makes.
const fs = require('fs'), path = require('path');
function wasmBytes() {
  const js = fs.readFileSync(path.join(__dirname, '..', 'pmc-wasm.js'), 'utf8');
  return Buffer.from(js.match(/PMC_WASM = "([^"]*)"/)[1], 'base64');
}
function load() {
  let inst, halted = null;
  const mem = () => new Uint8Array(inst.exports.memory.buffer);
  const cstr = p => { const m = mem(); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  const imports = {
    env: { js_halt: p => { halted = cstr(p); } },
    wasi_snapshot_preview1: { random_get: (p, n) => { const m = mem(); for (let i = 0; i < n; i++) m[p + i] = (Math.random() * 256) | 0; return 0; } },
  };
  inst = new WebAssembly.Instance(new WebAssembly.Module(wasmBytes()), imports);
  const x = inst.exports;
  x._initialize();
  const sim = {
    x, halted: () => halted,
    out: '',
    boot(flash) {
      x.sim_bench_init();
      const f = new Uint8Array(x.memory.buffer, x.sim_flash(), x.sim_flash_size());
      if (flash) f.set(flash); else f.fill(0xff);
      const d = new Date(2026, 9, 24, 14, 0, 0);
      x.sim_boot(0, d.getTime() / 1000 - d.getTimezoneOffset() * 60);
    },
    run(ms) { for (let t = 0; t < ms; t += 100) { x.sim_run(Math.min(100, ms - t)); sim.drain(); } },
    drain() { const n = x.sim_tx_len(); if (n) { sim.out += Buffer.from(new Uint8Array(x.memory.buffer, x.sim_tx(), n)).toString(); x.sim_tx_clear(); } },
    send(line) { const b = Buffer.from(line + '\r'); new Uint8Array(x.memory.buffer, x.sim_rx_buf(), b.length).set(b); x.sim_rx(b.length); },
    cmd(line, ms = 300) { sim.out = ''; sim.send(line); sim.run(ms); return sim.out; },
    tap(px, py, ms = 400) { x.sim_touch(px, py, 1); sim.run(120); x.sim_touch(px, py, 0); sim.run(ms); },
    png(file) {
      const fb = new Uint16Array(x.memory.buffer, x.sim_fb(), 480 * 272);
      fs.writeFileSync(file + '.raw', Buffer.from(fb.buffer, fb.byteOffset, fb.byteLength));
      require('child_process').execFileSync('python3', [path.join(__dirname, '..', '..', 'tools', 'ui-preview', 'render.py'), file + '.raw', file]);
      fs.unlinkSync(file + '.raw');
    },
    get: id => x.sim_get(id), set: (id, v) => x.sim_set(id, v),
    flash: () => new Uint8Array(x.memory.buffer, x.sim_flash(), x.sim_flash_size()).slice(),
  };
  return sim;
}
module.exports = { load };
