// Bridge LLD R23 browser check (Playwright run_code): the Settings page's "Camera centre" row.
// Run the stand-in tank (python3 firmware/esp32/web/test/fake_tank.py 8765) and open /settings (packed, icons in).
async (page) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.addInitScript(() => {  // a stand-in WebSocket: records what the page sends, answers the setting reads
    window.__sent = [];
    class FakeWS {
      constructor(url) { this.url = url; this.readyState = 0; window.__ws = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
      send(s) { const m = JSON.parse(s); window.__sent.push(m);
        if (m.t === "hello") this.emit({ t: "welcome", you: "settings-1", tank: { fw: { uno: "1.0" } } });
        if (m.t === "setting" && m.value === undefined) this.emit({ t: "setting", id: m.setting_id, value: m.setting_id === 27 ? -50 : 0 });
        if (m.t === "setting" && m.try && !window.__noAck) this.emit({ t: "ack", id: m.id }); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { this.readyState = 3; this.onclose && this.onclose(); }
    }
    window.WebSocket = FakeWS;
    window.__fakeEmit = (o) => window.__ws.emit(o);
  });
  const q = (f, a) => page.evaluate(f, a);
  const sent = (t) => q((t) => window.__sent.filter((m) => m.t === t), t);
  const out = {};
  await page.goto("http://127.0.0.1:8765/settings?v=" + Date.now()); await page.waitForTimeout(800);
  out.reads = (await sent("setting")).map((m) => m.setting_id).filter((id) => id === 27 || id === 28);  // [27, 28]
  out.shown = await q(() => document.getElementById("trimV").textContent);                              // pan -5.0° · tilt 0.0°
  await page.click('#tabs button[data-tab="tank"]'); await page.waitForTimeout(100);  // the Camera card is on TANK
  await q(() => window.__sent.length = 0);
  await page.click("#trimR");  // look further right: pan_trim - 5, after one cam_home
  await page.click("#trimU");  // look further up: tilt_trim - 5
  await page.click("#trimR");
  await page.waitForTimeout(100);
  out.home = (await sent("cam_home")).length;                                                       // 1
  out.tries = (await sent("setting")).map((m) => [m.setting_id, m.value, m.try === true]);         // [27,-55,t] [28,-5,t] [27,-60,t]
  out.shownAfter = await q(() => document.getElementById("trimV").textContent);                    // pan -6.0° · tilt -0.5° (unsaved)
  await q(() => window.__sent.length = 0);
  await page.click("#trimSave");
  await page.waitForTimeout(100);
  out.save = (await sent("setting")).map((m) => [m.setting_id, m.value, m.try === true]);          // [27,-60,false]
  await q(() => window.__fakeEmit({ t: "confirmed", what: "setting" })); await page.waitForTimeout(50);
  await q(() => window.__sent.length = 0);
  await page.click("#trimL");  // a step after the save
  await page.click("#trimUndo");  // back to what was saved
  await page.waitForTimeout(100);
  out.undo = (await sent("setting")).map((m) => [m.setting_id, m.value, m.try === true]);          // [27,-55,t] [27,-60,t]
  await q(() => window.__sent.length = 0);
  await page.click("#trimL"); await page.click("#trimL");
  for (let i = 0; i < 70; i++) await page.click("#trimL");  // never past +30.0°
  out.max = await q(() => document.getElementById("trimV").textContent);                            // pan +30.0° ...
  await q(() => { window.__sent.length = 0; window.__noAck = true; });
  await page.click("#trimD");  // refused by the bridge (someone else drives): the shown trim goes back
  const tr = (await sent("setting"))[0];
  await q((id) => window.__fakeEmit({ t: "nack", id: id, reason: "NOT_OWNER", message: "Someone else is driving" }), tr.id);
  await page.waitForTimeout(50);
  out.refused = await q(() => document.getElementById("trimV").textContent);                        // pan +30.0° · tilt -0.5°
  await page.click("#trimSave");
  const sv = (await sent("setting")).slice(-1)[0];
  await q((id) => window.__fakeEmit({ t: "nack", id: id, reason: "BUSY" }), sv.id); await page.waitForTimeout(50);
  await q(() => window.__fakeEmit({ t: "confirmed", what: "wifi" })); await page.waitForTimeout(50);  // another change
  await q(() => window.__sent.length = 0);
  await page.click("#trimSave");  // still unsaved: Save sends again
  out.resave = (await sent("setting")).map((m) => [m.setting_id, m.value]);                          // [[27,300]]
  const rs = (await sent("setting")).slice(-1)[0];
  await q((id) => window.__fakeEmit({ t: "nack", id: id, reason: "BUSY" }), rs.id); await page.waitForTimeout(50);
  // Final review I1: two quick steps, both refused -> back to what the Uno has (not the page's own earlier step).
  const trimNow = () => q(() => document.getElementById("trimV").textContent);
  const lastTries = async (n) => (await sent("setting")).filter((m) => m.try).slice(-n);
  await q(() => { window.__sent.length = 0; });
  await page.click("#trimR"); await page.click("#trimR");  // 300 -> 295 -> 290, no reply yet
  let [a, b] = await lastTries(2);
  await q((ids) => ids.forEach((id) => window.__fakeEmit({ t: "nack", id: id, reason: "BUSY" })), [a.id, b.id]);
  await page.waitForTimeout(60);
  out.twoRefused = await trimNow();                                                                  // pan +30.0° (the Uno's)
  await page.click("#trimR"); await page.click("#trimR");  // one refused, the next accepted
  [a, b] = await lastTries(2);
  await q((x) => { window.__fakeEmit({ t: "nack", id: x[0], reason: "BUSY" }); window.__fakeEmit({ t: "ack", id: x[1] }); }, [a.id, b.id]);
  await page.waitForTimeout(60);
  out.mixed = await trimNow();                                                                       // pan +29.0° (B reached the Uno)
  // Final review M1: saving track balance saves the Uno's whole block - the tried trims are saved too.
  await q(() => window.__fakeEmit({ t: "confirmed", what: "setting" })); await page.waitForTimeout(50);
  await q(() => { window.__sent.length = 0; });
  await page.click("#trimUndo"); await page.waitForTimeout(50);
  out.undoAfterOtherSave = (await sent("setting")).length;                                           // 0 (nothing unsaved)
  // Final review M4: a new link (bridge or Uno restarted) forgets the pending replies and homes the camera again.
  await q(() => { window.__noAck = false; window.__ws.close(); });
  await page.waitForTimeout(1500);
  await q(() => { window.__sent.length = 0; });
  await page.click("#trimR"); await page.waitForTimeout(50);
  out.rehome = (await sent("cam_home")).length;                                                      // 1
  out.wide = await q(() => document.documentElement.scrollWidth <= 390);                             // true
  return out;
}
