// Plan N4 Task 4 browser check (Playwright run_code): stand-in tank firmware/esp32/web/test/fake_tank.py on :8765.
async (page) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.addInitScript(() => {  // a stand-in WebSocket: records what the page sends, answers hello
    try { localStorage.clear(); } catch (e) { /* a fresh browser */ }
    window.__sent = [];
    class FakeWS {
      constructor(url) { this.url = url; this.readyState = 0; window.__ws = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
      send(s) { const m = JSON.parse(s); window.__sent.push(m);
        if (m.t === "hello") this.emit({ t: "welcome", you: "phone-1", tank: { fw: { uno: "1.0" } } });
        if (m.t === "wifi_status") this.emit({ t: "wifi_status", mode: "home", rssi: -54 });
        if (m.t === "logo_run" && window.__runReply) this.emit(Object.assign({ id: m.id }, window.__runReply)); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { this.readyState = 3; this.onclose && this.onclose(); }
    }
    window.WebSocket = FakeWS;
    window.__fakeEmit = (o) => window.__ws.emit(o);
  });
  const q = (f, a) => page.evaluate(f, a), st = () => q(() => document.getElementById("status").textContent);
  const out = {};
  await page.goto("http://127.0.0.1:8765/logo"); await page.waitForTimeout(1500);
  out.open = await st();                                                     // ● READY
  const ta = page.locator("#src");
  await ta.fill("TO SQUARE :size\n  REPEAT 4 [ FORWAD :size RIGHT 90 ]\nEND\nSQUARE 50\n");
  await page.waitForTimeout(1200);
  out.mistake = [await st(), await q(() => document.getElementById("diags").innerText)];  // 1 MISTAKE, line 2 quoted, ▲
  await ta.fill("TO SQUARE :size\n  REPEAT 4 [ FORWARD :size RIGHT 90 ]\nEND\nSQUARE 50\n");
  await page.waitForTimeout(1200);
  out.ready = [await st(), await q(() => document.getElementById("run").disabled)];        // READY, false
  await q(() => { window.__runReply = { t: "ack" }; });
  await page.click("#run"); await page.waitForTimeout(100);
  await q(() => { const f = window.__fakeEmit; f({ t: "ev", kind: "logo_line", line: 2, round: 2 });
    f({ t: "ev", kind: "logo_pose", x_cm: 0, y_cm: 50, heading: 0 }); f({ t: "ev", kind: "logo_pose", x_cm: 30, y_cm: 50, heading: 90 }); });
  await page.waitForTimeout(200);
  out.running = [await st(), await q(() => document.getElementById("src").readOnly),
    await q(() => document.querySelector("#hl div.run").textContent)];                      // RUNNING · line 2, true, ROUND 2
  await page.click("#stop");
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_ended", reason: 7040, line: 2, msg: "Stopped." }));
  await page.waitForTimeout(100);
  out.stopped = [await st(), await q(() => window.__sent.filter(m => m.t === "stop" || m.t === "logo_stop").map(m => m.t).join())];
  // step mode in the bridge's order: pause at 1, the move on line 1, pause at 2
  await page.click("#step"); await page.waitForTimeout(80);
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_line", line: 1, round: 0 })); await page.waitForTimeout(40);
  out.step1 = await st();                                                    // STEP · line 1
  await page.click("#run"); await page.waitForTimeout(40);
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_line", line: 1, round: 0 })); await page.waitForTimeout(40);
  out.stepMove = [await st(), await q(() => document.getElementById("run").disabled)];    // RUNNING, true
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_ended", reason: "finished", line: 4, msg: "" }));
  await page.waitForTimeout(60);
  // hidden page ends our run
  await page.click("#run"); await page.waitForTimeout(100);
  await q(() => { Object.defineProperty(document, "hidden", { configurable: true, get: () => true }); document.dispatchEvent(new Event("visibilitychange")); });
  await q(() => { Object.defineProperty(document, "hidden", { configurable: true, get: () => false }); document.dispatchEvent(new Event("visibilitychange")); });
  await page.waitForTimeout(1200); out.hidden = await st();                  // Ended (7046) … hidden
  // another device's program
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_pose", x_cm: 1, y_cm: 2, heading: 0 })); await page.waitForTimeout(60);
  out.other = [await st(), await q(() => document.getElementById("run").disabled)];       // ON ANOTHER DEVICE, true
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_ended", reason: 7040, line: 1, msg: "x" })); await page.waitForTimeout(60);
  // save as: an existing name asks twice; save while another client keeps the tank busy
  await page.click("#psave"); await page.fill("#nameI", "square"); await page.click("#nameO"); await page.waitForTimeout(300);
  out.replaceAsk = await q(() => document.getElementById("nameW").textContent);         // … press OK again to replace it.
  const hog = q(async () => Promise.all([0, 1, 2, 3, 4, 5].map(() => fetch("/logo/check", { method: "POST", body: "FD 1" }).then(r => r.status))));
  await page.fill("#nameI", "celebrate"); await page.click("#nameO");
  out.hog = await hog; await page.waitForTimeout(1500);                     // [200, 503, 503, …]
  out.saved = await q(() => document.getElementById("psel").textContent);    // … celebrate …
  await page.click("#pren"); await page.fill("#nameI", "party"); await page.click("#nameO"); await page.waitForTimeout(800);
  out.renamed = await q(() => document.getElementById("psel").textContent);  // party, no celebrate
  // a draft against a copy changed elsewhere
  await page.selectOption("#psel", "square"); await page.waitForTimeout(500);
  await q(() => { const s = document.getElementById("src"); s.value = "REPEAT 4 [ FD 70 RT 90 ]\n"; s.dispatchEvent(new Event("input")); });
  await q(async () => { await fetch("/logo/programs/square", { method: "PUT", body: "FD 5\n" }); });
  await page.selectOption("#psel", "first_square"); await page.waitForTimeout(500);
  out.exampleAsks = await (async () => { await page.click("#psave"); await page.waitForTimeout(80);
    const t = await q(() => document.getElementById("nameT").textContent); await page.click("#nameC"); return t; })();  // SAVE AS
  await page.selectOption("#psel", "square"); await page.waitForTimeout(700);
  out.draft = [await q(() => document.getElementById("src").value), await q(() => document.getElementById("log").innerText.split("\n").pop())];
  // delete
  await page.selectOption("#psel", "party"); await page.waitForTimeout(500);
  await page.click("#pdel"); await page.click("#delO"); await page.waitForTimeout(800);
  out.deleted = await q(() => document.getElementById("psel").textContent);  // no party
  // word button: inside brackets, number selected
  await q(() => { const s = document.getElementById("src"); s.value = "REPEAT 4 [  ]"; s.setSelectionRange(11, 11); s.dispatchEvent(new Event("input")); });
  await page.click("#pad button:has-text('FORWARD')"); await page.waitForTimeout(50);
  out.insert = await q(() => { const s = document.getElementById("src"); return [s.value, s.value.slice(s.selectionStart, s.selectionEnd)]; });
  // an 8 KB text and a 2000-point path
  const big = "REPEAT 4 [ FORWARD 50 RIGHT 90 ] ; a square side by side\n".repeat(140).slice(0, 8000);
  out.colourMs = await q((t) => { const s = document.getElementById("src"); s.value = t; const t0 = performance.now();
    for (let i = 0; i < 10; i++) s.dispatchEvent(new Event("input")); return (performance.now() - t0) / 10; }, big);
  await q(() => { const s = document.getElementById("src"); s.value = "; bigpath\nFOREVER [ FD 10 RT 5 ]\n"; s.dispatchEvent(new Event("input")); });
  await page.waitForTimeout(1200);
  out.pathMs = await q(() => { const t0 = performance.now(); window.dispatchEvent(new Event("resize")); return performance.now() - t0; });
  out.cutShort = await q(() => document.getElementById("pnote").textContent);  // path cut short …
  return out;
}
