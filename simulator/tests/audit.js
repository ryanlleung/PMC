// Audit every screen, keypad, dialog and fault state at native size; screenshot each (tests/shots/audit-*.png).
module.exports = async (p, h) => {
  let n = 0;
  const check = async (label, setup) => { await p.evaluate(setup); const r = await h.audit(label); if (r.length) n++; await h.shot('audit-' + label); };
  const go = name => `navStack = []; closeOverlay(); show('${name}', false, true);`;
  const base = () => { localStorage.clear(); SAVED = defaultCfg(); loadActive(SAVED); DRAFT.clear(); C.faults.clear(); setHe(true); setPump(true); accEnter('audit'); };
  await p.evaluate(base);
  for (const sc of ['ctrl', 'valve', 'menu', 'ref', 'info', 'comm', 'guide', 'sensor', 'motor', 'tune', 'limits', 'diag', 'cfg', 'faults', 'trend'])
    await check(sc, go(sc));
  // tabs on Valve setup
  for (const t of ['Reference', 'Drive']) { await p.evaluate(go('motor')); await h.tap(t); await check('motor-' + t.toLowerCase(), 'updateUI()'); }
  // modes on home and the valve page
  await check('ctrl-notref', 'reboot(); accEnter("audit"); ' + go('ctrl'));
  await check('valve-notref', go('valve'));
  await check('ctrl-referencing', 'C.faults.clear(); home(); for (let i = 0; i < 200; i++) simTick(); ' + go('ctrl'));
  await check('ref-referencing', go('ref'));
  await check('ctrl-auto', 'bootHomed(); C.mode = "MANUAL"; M.pos = 33000; C.manPos = M.pos; C.SP = 5; toAuto(); ' + go('ctrl'));
  await check('valve-auto', go('valve'));
  await check('ctrl-paused', 'C.mode = "PAUSED"; ' + go('ctrl'));
  await check('valve-paused', go('valve'));
  await check('ctrl-multifault', 'C.mode = "MANUAL"; raise("F-10", "No pressure response in 5 windows"); raise("F-01", "Druck signal lost"); raise("F-12", "Operator stop"); ' + go('ctrl'));
  await check('menu-multifault', go('menu'));
  await check('faults-multifault', go('faults'));
  await check('faultdetail', 'faultDetail(faultsByPriority()[0][0])');
  await p.evaluate('C.faults.clear(); bootHomed();');
  // extreme valid values
  await check('ctrl-extreme', 'M.pos = P.POSMAX; C.manPos = M.pos; C.SP = 1034; PV = 1034; ' + go('ctrl'));
  await check('valve-extreme', go('valve'));
  await check('diag-extreme', 'M.target = 99072; M.from = 0; M.t0 = 0; M.tEnd = 9999; ' + go('diag'));
  await check('tune-extreme', 'P.Kp = 100; P.Ti = 100; P.DB = 2000; P.outMin = 100; ' + go('tune'));
  await check('limits-extreme', 'P.Pmin = 999.99; P.spMax = 1034; P.alarmLo = 999.99; P.band = 100; ' + go('limits'));
  await check('motor-extreme', 'MOT.accel = 20000; MOT.run = 1000; ' + go('motor'));
  await p.evaluate(() => { loadActive(SAVED); });
  // long metadata
  await check('info-longcal', 'SENS.prof[0].calId = "Druck Leicester reissued certificate CA-5880156-2026 rev B"; SENS.prof[0].calDate = "09 Oct 2026"; ' + go('info'));
  await check('sensor-longcal', go('sensor'));
  await check('caldetail', 'calDetail()');
  await p.evaluate(() => { loadActive(SAVED); });
  // keypads
  await check('kp-target', go('ctrl') + ' keypadSP()');
  await check('kp-position', go('valve') + ' keypadPct()');
  await check('kp-pin', 'accExit("audit"); ' + go('menu') + ' commTile()');
  await p.evaluate(() => accEnter('audit'));
  await check('kp-field', go('tune') + ' editField(FIELDS.tune[0])');
  await h.keys('9999'); await h.tap('Set'); await check('kp-field-error', 'updateUI()');
  await check('kp-travel', go('motor') + ' C.mode = "AUTO"; editField(FIELDS.motor[0]); C.mode = "MANUAL"');
  // dialogs
  const dialogs = { 'dlg-auto': go('ctrl') + ' modePress()', 'dlg-manual': 'C.mode = "AUTO"; askManual(); C.mode = "MANUAL"', 'dlg-paused': 'C.mode = "PAUSED"; modePress(); C.mode = "MANUAL"',
    'dlg-reference': 'homeAsk()', 'dlg-response': go('tune') + ' editField(FIELDS.tune.find(f => f.id === "P.KDIR"))', 'dlg-motordir': 'editField(FIELDS.motor.find(f => f.id === "MOT.rev"))',
    'dlg-exit': 'DRAFT.set("P.Kp", 6); P.Ti = 0.3; commExit()', 'dlg-discard': 'confirmDiscard()', 'dlg-save': 'saveAsk()',
    'dlg-limits': 'DRAFT.clear(); C.SP = 5; DRAFT.set("P.Pmin", 6); applyAll()', 'dlg-target-big': 'DRAFT.clear(); loadActive(SAVED); ' + go('ctrl') + ' keypadSP()' };
  for (const [k, js] of Object.entries(dialogs)) { await check(k, js); if (k === 'dlg-target-big') { await h.keys('500'); await h.tap('Set'); await check('dlg-target-big2', 'updateUI()'); } }
  await check('difflist', 'closeOverlay(); for (const f of allFields().filter(f => f.min != null).slice(0, 12)) DRAFT.set(f.id, f.max); diffList("Unapplied changes", cfgDiffs().nd, "Apply changes puts them in use.")');
  await check('cfg-pending', 'closeOverlay(); P.Kp = 5; ' + go('cfg'));
  await check('sensor-import', go('sensor') + ' DRAFT.clear();'); await h.tap('Import tablesfrom PC'); await check('import-info', 'updateUI()');
  console.log(n ? n + ' states with findings' : 'no findings');
};
