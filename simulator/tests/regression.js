// Earlier review findings that must stay fixed (v49, v50) and the v51 all-or-nothing Apply.
module.exports = async (p, h) => {
  const { ok } = h, ev = f => p.evaluate(f);
  await ev(() => { localStorage.clear(); SAVED = defaultCfg(); loadActive(SAVED); document.getElementById('reboot').click(); setHe(true); setPump(true); });
  let r = await ev(() => { home(); let n = 0; while (!(C.mode === 'HOMING' && !M.moving && C.homeStage === 'back-off') && n++ < 400 / DT) simTick();
    operatorStop(); for (let i = 0; i < 5 / DT; i++) simTick(); return { mode: C.mode, moving: M.moving, f12: C.faults.has('F-12') }; });
  ok(r.mode === 'NOT HOMED' && !r.moving && r.f12, 'STOP in the pause between referencing stages: no further move');
  r = await ev(() => { command('LIM SIM OPEN'); for (let i = 0; i < 1 / DT; i++) simTick(); const started = requestMove(M.pos + 2000, true); M.moving = true; M.target = M.pos + 5000; M.dir = 1; M.v = 50; operatorStop(); return { lim: LIM.active, started, moving: M.moving }; });
  ok(r.lim && r.started === false && !r.moving, 'closed limit with F-12: opening refused; STOP again stops');
  await ev(() => { command('LIM SIM NORMAL'); ack(); bootHomed(); accEnter('test'); });
  r = await ev(() => { const res = importCfg({ MOT: { run: -100 } }, 'bad'); return { res, d: DRAFT.size }; });
  ok(r.res === false && r.d === 0, 'import with run speed -100 refused, nothing drafted');
  r = await ev(() => { DRAFT.clear(); DRAFT.set('P.Pmin', 10); DRAFT.set('P.spMax', 5); DRAFT.set('P.Kp', 9); const res = applyAll(); return { res, kp: P.Kp, pmin: P.Pmin, d: DRAFT.size, why: C.refusal }; });
  ok(r.res === false && r.kp === 4 && r.pmin === 0.2 && r.d === 3, 'invalid change set rejected whole, nothing applied, drafts kept: ' + r.why);
  r = await ev(() => { DRAFT.clear(); command('PID SPMAX 0.1'); return [...document.querySelectorAll('#con div')].slice(-1)[0].textContent; });
  ok(/^ERR/.test(r), 'serial PID SPMAX below Target min refused: ' + r);
  r = await ev(() => { DRAFT.clear(); C.SP = 5; DRAFT.set('P.Pmin', 6); applyAll(); const asked = overlay && /outside the new limits/.test(overlay.textContent); closeOverlay(); applyFields([FIELDS.limits.find(f => f.id === 'P.Pmin')], true); return { asked, sp: C.SP, pmin: P.Pmin }; });
  ok(r.asked && r.sp === 6 && r.pmin === 6, 'new limits that exclude the target: asked once, target moved with them');
  r = await ev(() => { DRAFT.clear(); loadActive(SAVED); const q = JSON.parse(JSON.stringify(SENS.prof[0])); q.atm = 1.06; q.calId = 'Reissue'; importCfg({ sens: { prof: [q] } }, 't'); return { d: DRAFT.has('S.prof'), atm: SENS.prof[0].atm }; });
  ok(r.d && r.atm === 1.0512, 'revised calibration for a known serial is a draft, not applied');
  r = await ev(() => { DRAFT.clear(); P.Kp = 5; SIM_FLASH_FAIL = true; const rev = SAVED.rev; const res = saveCfg(); SIM_FLASH_FAIL = false; return { res, same: rev === SAVED.rev, dirty: cfgDirty() }; });
  ok(r.res === false && r.same && r.dirty, 'failed flash write: not saved, reported');
  r = await ev(() => { DRAFT.set('P.Ti', 0.5); const was = [C.mode, M.pos]; commExit(); const t = overlay.textContent; closeOverlay(); return { t, was }; });
  ok(/kept for review/.test(r.t) && /Discard drafts/.test(r.t), 'Exit warns: drafts kept, offers Discard drafts');
};
