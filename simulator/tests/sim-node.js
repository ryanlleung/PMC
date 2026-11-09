// Loads simulator/pmc.wasm in node: the same calls the page makes.
const fs = require('fs'), path = require('path');
// file: a .wasm built by wasm/build.sh (out/pmc.wasm); else pmc-wasm.js.
function wasmBytes(file) {
  if (file) return fs.readFileSync(file);
  const js = fs.readFileSync(path.join(__dirname, '..', 'pmc-wasm.js'), 'utf8');
  return Buffer.from(js.match(/PMC_WASM = "([^"]*)"/)[1], 'base64');
}
function load(file) {
  let inst, halted = null;
  const mem = () => new Uint8Array(inst.exports.memory.buffer);
  const cstr = p => { const m = mem(); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  const imports = {
    env: { js_halt: p => { halted = cstr(p); } },
    wasi_snapshot_preview1: { random_get: (p, n) => { const m = mem(); for (let i = 0; i < n; i++) m[p + i] = (Math.random() * 256) | 0; return 0; } },
  };
  inst = new WebAssembly.Instance(new WebAssembly.Module(wasmBytes(file)), imports);
  const x = inst.exports;
  x._initialize();
  const sim = {
    x, halted: () => halted,
    version: () => cstr(x.sim_version()),
    out: '',
    // opts.remote: false turns Remote control Off; otherwise it is On, the
    // default from 0.13.7, so the link takes set commands. opts.reset: 0
    // power-on (default), 1 watchdog, 2 software (Restart). Every start is
    // OFF (0.15.1): turned on at once (the double tap) unless opts.off.
    boot(flash, opts = {}) {
      // application_init() runs once per instance, as on the board: use a new load() per boot.
      if (sim.booted) throw new Error('boot() twice on one instance: use a new load()');
      sim.booted = true;
      x.sim_bench_init();
      const f = new Uint8Array(x.memory.buffer, x.sim_flash(), x.sim_flash_size());
      if (flash) f.set(flash); else f.fill(0xff);
      const d = new Date(2026, 9, 24, 14, 0, 0);
      x.sim_boot(opts.reset || 0, d.getTime() / 1000 - d.getTimezoneOffset() * 60);
      if (opts.remote === false) x.sim_remote(0);
      if (!opts.off) x.sim_turn_on();
    },
    run(ms) { for (let t = 0; t < ms; t += 100) { x.sim_run(Math.min(100, ms - t)); sim.drain(); } },
    drain() { const n = x.sim_tx_len(); if (n) { sim.out += Buffer.from(new Uint8Array(x.memory.buffer, x.sim_tx(), n)).toString(); x.sim_tx_clear(); } },
    send(line) {
      // sim_rx_buf holds 256 bytes: longer lines go in parts, as the page sends them.
      const b = Buffer.from(line + '\r');
      for (let i = 0; i < b.length; i += 256) {
        const part = b.subarray(i, i + 256);
        new Uint8Array(x.memory.buffer, x.sim_rx_buf(), part.length).set(part);
        x.sim_rx(part.length);
      }
    },
    cmd(line, ms = 300) { sim.out = ''; sim.send(line); sim.run(ms); return sim.out; },
    tap(px, py, ms = 400) { x.sim_touch(px, py, 1); sim.run(120); x.sim_touch(px, py, 0); sim.run(ms); },
    png(file) {
      const fb = new Uint16Array(x.memory.buffer, x.sim_fb(), 480 * 272);
      fs.writeFileSync(file + '.raw', Buffer.from(fb.buffer, fb.byteOffset, fb.byteLength));
      require('child_process').execFileSync('python3', [path.join(__dirname, '..', '..', 'tools', 'ui-preview', 'render.py'), file + '.raw', file]);
      fs.unlinkSync(file + '.raw');
    },
    get: id => x.sim_get(id), set: (id, v) => x.sim_set(id, v),
    // Test hooks (sim_main.c): what the screens do. The text port takes the
    // remote command set only, so these reach the firmware directly.
    why: () => cstr(x.sim_why()),
    home(ms = 120000) { const ok = x.sim_home(); sim.run(ms); return ok; },
    setClosed() { const ok = x.sim_set_closed(); sim.run(300); return ok; },
    shutdown(ms = 300, close = true) { const ok = x.sim_shutdown(close ? 1 : 0); sim.run(ms); return ok; },   // Shut down, confirmed
    turnOn(ms = 300) { const ok = x.sim_turn_on(); sim.run(ms); return ok; },
    cal(what, mbar, ms = 15000) { const ok = x.sim_cal(what, mbar); sim.run(ms); return ok; },   // 1 vacuum, 2 air; mbar < 0: room sensor
    save(key = -1, value = 0) { const ok = x.sim_save(key, value); sim.run(1000); return ok; },
    setting: key => x.sim_setting(key),
    reg: () => x.sim_reg(),            // 0 none, 1 regulating, 2 in band, 3 stable
    offer: () => x.sim_offer(),        // saved position offered at start-up, or -2147483648
    busy: () => !!x.sim_busy(), override: () => !!x.sim_override(),
    popup: () => !!x.sim_popup(),     // a popup is open (ui_overlay_open)
    flash: () => new Uint8Array(x.memory.buffer, x.sim_flash(), x.sim_flash_size()).slice(),
  };
  return sim;
}
// cfg_key_t (InitialTesting/cfg.h), for save() and setting().
const CFG = { POSMAX: 0, REVERSE: 1, BACKOFF: 2, RETURN: 3, FHOME: 4, FCREEP: 5, RATE: 6, START: 7, ACCEL: 8,
  KP: 9, TI: 10, OUTMIN: 11, DB: 12, KDIR: 13, BAND: 14, KD: 15,
  KP2: 16, TI2: 17, KD2: 18, KP3: 19, TI3: 20, KD3: 21, SCHED: 22, B1: 23, B2: 24, HYST: 25, DBC: 26 };   // SENSOR (27) went in 0.16.2
module.exports = { load, CFG };
