// The seven review workflows, driven by taps on the 480 x 272 screen. Screenshots: tests/shots/wfN-*.png
module.exports = async (p, h) => {
  const { tap, keys, run, st, shot, ok } = h;
  const ev = f => p.evaluate(f);
  await ev(() => { localStorage.clear(); SAVED = defaultCfg(); loadActive(SAVED); setHe(true); setPump(true); });

  // 1. Unreferenced startup -> reference -> set target -> AUTO
  await ev(() => document.getElementById('reboot').click());
  let o = await st(); ok(o.badge === 'NOT REFERENCED' && o.mode === 'NOT HOMED', 'WF1 real start boots NOT REFERENCED'); await shot('wf1-1-boot');
  await tap('ReferenceValve'); await shot('wf1-2-ask'); await tap('Reference'); await run(3); await shot('wf1-3-referencing');
  await run(400); o = await st(); ok(o.mode === 'MANUAL' && o.pos === 0, 'WF1 referenced, MANUAL at 0 %');
  await tap('MoveValve'); await tap('Enterposition'); await keys('34'); await tap('Set'); await run(120); await tap('Back');
  await tap('SetTarget'); await keys('5'); await shot('wf1-4-target'); await tap('Set');
  await tap('StartAUTO'); await tap('Start AUTO'); await run(5); o = await st(); ok(o.mode === 'AUTO', 'WF1 AUTO started');
  await shot('wf1-5-regulating'); const s1 = await ev(() => { C.msg = null; return statusLine().text; });
  await run(600); const s2 = await ev(() => { C.msg = null; return statusLine().text; }); await shot('wf1-6-stable');
  ok(/Regulating|Within|Stable/.test(s1) && /Stable|Within/.test(s2), 'WF1 status "' + s1 + '" then "' + s2 + '"');

  // 2. AUTO -> Valve page -> MANUAL -> enter position -> navigate away while moving
  await tap('ViewValve'); await shot('wf2-1-valve-auto'); await tap('Switch toMANUAL'); await shot('wf2-2-confirm'); await tap('Switch to MANUAL');
  o = await st(); ok(o.mode === 'MANUAL' && o.cur === 'valve', 'WF2 switched to MANUAL from the Valve page');
  await tap('Enterposition'); await keys('60'); await tap('Set'); await run(2); await shot('wf2-3-moving');
  const a = await st(); await tap('Back'); await run(1); const b = await st();
  ok(a.moving && b.moving && a.target === b.target && b.cur === 'ctrl', 'WF2 leaving the Valve page keeps the move');
  await shot('wf2-4-home-moving'); await run(400); o = await st(); ok(Math.abs(o.pos - 60) < 0.2, 'WF2 move finished at ' + o.pos + ' %');

  // 3. Paused AUTO -> resume, or switch to MANUAL
  await tap('StartAUTO'); await tap('Start AUTO'); await run(30);
  await ev(() => { C.mode = 'PAUSED'; ev('W-02', 'No convergence (test)'); }); await shot('wf3-1-paused');
  ok((await ev(() => [...document.querySelectorAll('#dev .b')].filter(b => b.offsetParent).map(b => b.textContent).join('|'))).includes('Switch toMANUAL'), 'WF3 paused home shows Resume AUTO and Switch to MANUAL');
  await tap('ResumeAUTO'); await shot('wf3-2-resume-ask'); await tap('Resume AUTO'); o = await st(); ok(o.mode === 'AUTO', 'WF3 resumed AUTO');
  await ev(() => { C.mode = 'PAUSED'; updateUI(); }); await tap('Switch toMANUAL'); await tap('Switch to MANUAL'); o = await st(); ok(o.mode === 'MANUAL', 'WF3 paused -> MANUAL from the status line');

  // 4. Edit several commissioning pages -> review -> apply -> save -> restart
  await run(5); await tap('Menu'); await tap('Commissioning', true); await keys('2468'); await tap('Set'); await shot('wf4-1-comm');
  await tap('Control tuning'); await tap('Gain, %/mbar', true); await keys('6'); await tap('Set'); await tap('Back');
  o = await st(); ok(o.cur === 'comm' && !o.ov, 'WF4 Back with a draft: no Apply/Discard question');
  await tap('Pressure limits'); await tap('Target band, mbar', true); await keys('0.2'); await tap('Set'); await tap('Back');
  await tap('Valve setup'); await tap('Drive'); await tap('Run speed, steps/s', true); await keys('500'); await tap('Set'); await shot('wf4-2-drive-draft'); await tap('Back');
  await tap('Configuration'); await shot('wf4-3-config');
  await tap('Unapplied changes', true); const list = (await st()).ov; await shot('wf4-4-list');
  ok(/Gain/.test(list) && /Target band/.test(list) && /Run speed/.test(list), 'WF4 review lists all three by label and page'); await tap('Close');
  await tap('Apply3 changes'); o = await st(); ok(!o.drafts.length && o.dirty, 'WF4 applied together, not saved');
  await shot('wf4-5-applied'); await tap('Save allsettings'); await shot('wf4-6-save-ask'); await tap('Save');
  o = await st(); ok(o.rev === 2 && !o.dirty, 'WF4 saved as rev 2');
  await ev(() => document.getElementById('reboot').click());
  o = await ev(() => [P.Kp, P.band, MOT.run, SAVED.rev]); ok(o.join() === '6,0.2,500,2', 'WF4 kept after restart: ' + o.join(', '));

  // 5. Import revised calibration -> inspect identity/status -> apply and save
  await ev(() => { bootHomed(); accEnter('test'); show('sensor', false, true); });
  await ev(() => { const q = JSON.parse(JSON.stringify(SENS.prof[0])); q.atm = 1.06; q.calId = 'Druck reissue CA-5880156'; q.calDate = '09 Oct 2026'; importCfg({ sens: { prof: [q] } }, 'PC tool'); });
  await shot('wf5-1-sensor-imported'); await tap('Calibration', true); o = await st(); await shot('wf5-2-detail');
  ok(/Druck reissue/.test(o.ov) && /Unapplied change/.test(o.ov), 'WF5 detail shows new identity, state Unapplied change'); await tap('Close');
  await tap('Apply1 change'); await ev(() => show('info', false, true)); await shot('wf5-3-info-applied');
  o = await ev(() => document.querySelector('#dev').innerText); ok(/Druck reissue/.test(o) && /Not saved/.test(o), 'WF5 System information: identity, date, Not saved apart');
  await ev(() => show('cfg', false, true)); await tap('Save allsettings'); await tap('Save');
  await ev(() => show('info', false, true)); await shot('wf5-4-info-saved'); o = await ev(() => calState(SENS.prof[SENS.active])); ok(o === 'Saved', 'WF5 calibration saved');

  // 6. STOP and fault recovery, including during referencing
  await ev(() => show('ctrl', false, true)); await tap('Menu'); await tap('Reference valveFind closed position'); await tap('StartReference'); await tap('Reference'); await run(3);
  await shot('wf6-1-referencing'); await tap('STOP'); await run(5); o = await st();
  ok(o.mode === 'NOT HOMED' && !o.moving && o.badge === 'STOPPED', 'WF6 STOP while referencing: stopped, not referenced'); await shot('wf6-2-stopped');
  await ev(() => { home && 0; }); await ev(() => show('ctrl', false, true)); await tap('Resetfault'); o = await st(); ok(!o.faults.length && o.mode === 'NOT HOMED', 'WF6 reset; still NOT REFERENCED');
  await ev(() => { bootHomed(); document.getElementById('b-druck').click(); }); await run(2); o = await st(); ok(o.faults.includes('F-01'), 'WF6 F-01 on Druck unplugged'); await shot('wf6-3-f01');
  await tap('Resetfault'); o = await st(); ok(o.faults.includes('F-01'), 'WF6 reset refused while unplugged');
  await ev(() => document.getElementById('b-druck').click()); await run(2); await tap('Resetfault'); o = await st(); ok(!o.faults.length && o.mode === 'MANUAL', 'WF6 reset after replug, MANUAL');

  // 7. Failed save, and commissioning timeout with pending edits
  await ev(() => { accEnter('test'); show('tune', false, true); P.Kp = 7; document.getElementById('b-flash').click(); updateUI(); });
  const rev0 = (await st()).rev;
  await tap('Save allsettings'); await tap('Save'); o = await st(); await shot('wf7-1-save-failed');
  const why = await ev(() => C.refusal);
  ok(o.rev === rev0 && o.dirty && /Save failed/.test(o.foot + why), 'WF7 failed save reported, rev unchanged, still not saved: ' + JSON.stringify([o.rev, o.dirty, o.foot, why]));
  await ev(() => document.getElementById('b-flash').click());
  await tap('Integral time, min', true); await keys('0.4'); await tap('Set');
  const before = await st();
  await ev(() => { ACC.last = performance.now() - 11 * 60 * 1000; updateUI(); }); o = await st(); await shot('wf7-2-timeout');
  ok(!o.acc && o.drafts.includes('P.Ti') && o.mode === before.mode && o.pos === before.pos, 'WF7 timeout: access closed, draft kept, mode and valve unchanged');
  await tap('Commissioning', true); await keys('2468'); await tap('Set'); await shot('wf7-3-reentry'); o = await st();
  ok(o.drafts.includes('P.Ti') && /unapplied/.test(o.foot), 'WF7 draft offered for review after re-entry');
};
