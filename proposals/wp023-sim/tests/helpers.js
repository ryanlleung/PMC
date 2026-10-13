// Test helpers: tap by visible text (as a touch press), type on the keypad, run simulated time, read state, audit.
const { audit } = require('./audit-lib.js');
module.exports = (p, out) => {
  const shot = async n => { await p.evaluate(() => updateUI()); await p.waitForTimeout(60); await p.locator('#devclip').screenshot({ path: out + '/' + n + '.png' }); };
  const tap = async (txt, prefix) => { await p.evaluate(([t, pre]) => {
    const vis = e => { for (let x = e; x && x !== document.body; x = x.parentElement) if (getComputedStyle(x).display === 'none') return false; return true; };
    const els = [...document.querySelectorAll('#dev .b, #dev .seg, #dev .card, #dev .fld, #dev .ro, #dev .ov div')].filter(e => { if (!vis(e)) return false; const s = e.textContent.replace(/\s+/g, ' ').trim(); return pre ? s.startsWith(t) : s === t; });
    if (!els.length) throw new Error('no element ' + t);
    const el = els[els.length - 1];
    el.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true })); el.dispatchEvent(new PointerEvent('pointerup', { bubbles: true }));
  }, [txt, prefix]); await p.waitForTimeout(40); };
  const keys = async str => { for (const ch of str) { if (ch === '<') await p.evaluate(() => { const b = [...document.querySelectorAll('#dev .b')].find(x => x.querySelector('[data-lv="LV_SYMBOL_BACKSPACE"]')); b.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true })); }); else await tap(ch); } };
  const run = async sec => p.evaluate(s => { const n = Math.round(s / DT); for (let i = 0; i < n; i++) { simTick(); runQuick(); } updateUI(); }, sec);
  const st = () => p.evaluate(() => ({ cur: current, mode: C.mode, acc: ACC.on, drafts: [...DRAFT.keys()], dirty: cfgDirty(), rev: SAVED.rev, pv: PV == null ? null : +PV.toFixed(2), pos: +pctOf(M.pos).toFixed(1), moving: M.moving, target: M.target, faults: [...C.faults.keys()], foot: G.ftxt.textContent, badge: G.badge.textContent, ov: overlay ? overlay.textContent : '' }));
  const doAudit = async label => { await p.evaluate(() => updateUI()); const r = await p.evaluate(`(${audit.toString()})()`); console.log((r.length ? 'AUDIT ' : 'AUDIT ok ') + label + (r.length ? ': ' + r.join(' | ') : '')); return r; };
  return { shot, tap, keys, run, st, audit: doAudit };
};
