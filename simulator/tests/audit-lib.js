// Screen audit for the PMC simulator, run in the page at native 480 x 272 (scale 1).
// Checks: horizontal clipping, cut wrapped text, text overlapping other text or buttons, text outside its card or
// button, glyphs missing from the LVGL Montserrat fonts, touch targets under 40 px, font sizes outside 12/16/26/40.
module.exports.audit = () => {
  const out = [], dev = document.getElementById('dev'), D = dev.getBoundingClientRect();
  const visible = e => { for (let x = e; x && x !== dev; x = x.parentElement) { const cs = getComputedStyle(x); if (cs.display === 'none' || cs.visibility === 'hidden') return false; } return true; };
  const scrollable = e => { for (let x = e; x && x !== dev; x = x.parentElement) { const o = getComputedStyle(x).overflowY; if (o === 'auto' || o === 'scroll') return true; } return false; };
  const rel = r => ({ l: r.left - D.left, r: r.right - D.left, t: r.top - D.top, b: r.bottom - D.top });
  // text box of the glyphs: large digits leave empty ascender space above, which is not a visible overlap
  const textRect = e => { const rg = document.createRange(); rg.selectNodeContents(e); const r = rel(rg.getBoundingClientRect()); const fs = parseFloat(getComputedStyle(e).fontSize); if (fs >= 26) { r.t += fs * 0.22; r.b -= fs * 0.12; } return r; };
  const leaves = [...dev.querySelectorAll('div, b, small, span, p')].filter(e => visible(e) && [...e.childNodes].some(n => n.nodeType === 3 && n.textContent.trim()) && !e.closest('.sym'));
  const name = e => '"' + e.textContent.trim().slice(0, 40) + '"';
  // top-most layer only: elements under an open overlay are hidden from the user
  const ov = dev.querySelector('.ov');
  const onTop = e => !ov || ov.contains(e) || e.closest('.hdr') || (getComputedStyle(e.closest('[style*="z-index"]') || dev).zIndex | 0) > 10;
  const L = leaves.filter(onTop);
  L.forEach(e => {
    const cs = getComputedStyle(e), fs = parseFloat(cs.fontSize);
    if (![12, 16, 26, 40].includes(Math.round(fs))) out.push('FONT ' + fs + 'px ' + name(e));
    if (!scrollable(e) && !e.dataset.trunc) {
      // sub-pixel: scrollWidth rounds, so compare the text's own width (an ellipsis can hide 0.3 px of overflow)
      const tw = (() => { const rg = document.createRange(); rg.selectNodeContents(e); return rg.getBoundingClientRect().width; })();
      if ((e.scrollWidth > e.clientWidth + 1 || (cs.whiteSpace === 'nowrap' && e.style.width && cs.overflow !== 'visible' && tw > e.clientWidth + 0.05)) && e.clientWidth) out.push('CLIP-H ' + name(e) + ' ' + tw.toFixed(1) + '>' + e.clientWidth);
      if (e.scrollHeight > e.clientHeight + 1 && e.clientHeight && cs.overflow !== 'visible') out.push('CLIP-V ' + name(e) + ' ' + e.scrollHeight + '>' + e.clientHeight);
    }
    const t = textRect(e), box = e.closest('.card, .b, .modal, .hdr');
    if (box && !scrollable(e) && !e.dataset.trunc) { const bx = rel(box.getBoundingClientRect()); if (t.l < bx.l - 1 || t.r > bx.r + 1 || t.t < bx.t - 1 || t.b > bx.b + 1) out.push('OUTSIDE ' + name(e)); }
    if (!scrollable(e) && (t.l < -1 || t.r > 481 || t.t < -1 || t.b > 273)) out.push('OFFSCREEN ' + name(e));
    for (const ch of e.textContent) { const c = ch.codePointAt(0); if (!((c >= 0x20 && c <= 0x7E) || c === 0xB0 || c === 0x2022)) out.push('GLYPH U+' + c.toString(16).toUpperCase() + ' ' + name(e)); }
  });
  // text against text in other elements, and against buttons it is not inside
  for (let i = 0; i < L.length; i++) for (let j = i + 1; j < L.length; j++) {
    if (L[i].contains(L[j]) || L[j].contains(L[i]) || scrollable(L[i]) || scrollable(L[j]) || L[i].dataset.trunc || L[j].dataset.trunc) continue;   // clamped labels: hidden lines are not drawn
    const a = textRect(L[i]), b = textRect(L[j]), tol = 2;
    if (a.l < b.r - tol && b.l < a.r - tol && a.t < b.b - tol && b.t < a.b - tol) out.push('OVERLAP ' + name(L[i]) + ' / ' + name(L[j]));
  }
  const btns = [...dev.querySelectorAll('.b')].filter(b => visible(b) && onTop(b));
  btns.forEach(b => { const r = rel(b.getBoundingClientRect()); if (r.r - r.l < 40 || r.b - r.t < 40) out.push('TARGET ' + Math.round(r.r - r.l) + 'x' + Math.round(r.b - r.t) + ' ' + name(b)); });
  L.forEach(e => { if (e.closest('.b') || scrollable(e)) return; const t = textRect(e); btns.forEach(b => { if (b.contains(e)) return; const r = rel(b.getBoundingClientRect()); if (t.l < r.r - 2 && r.l < t.r - 2 && t.t < r.b - 2 && r.t < t.b - 2 && !(b.closest('.ov') && !e.closest('.ov'))) out.push('UNDER-BUTTON ' + name(e) + ' / ' + name(b)); }); });
  return [...new Set(out)];
};
