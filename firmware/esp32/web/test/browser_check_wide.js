// Revision N6 browser check (Tank Logo LLD §23.6, Playwright run_code): the laptop layout on the stand-in tank
// firmware/esp32/web/test/fake_tank.py on :8765. Each line of `out` says what it should be.
async (page) => {
  await page.addInitScript(() => {  // a stand-in WebSocket: records what the page sends, answers hello
    try { localStorage.clear(); } catch (e) { /* a fresh browser */ }
    window.__sent = [];
    class FakeWS {
      constructor(url) { this.url = url; this.readyState = 0; window.__ws = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
      send(s) { const m = JSON.parse(s); window.__sent.push(m);
        if (m.t === "hello") this.emit({ t: "welcome", you: "phone-1", tank: { fw: { uno: "1.0" } } });
        if (m.t === "logo_run") this.emit({ t: "ack", id: m.id }); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { this.readyState = 3; this.onclose && this.onclose(); }
    }
    window.WebSocket = FakeWS;
    window.__fakeEmit = (o) => window.__ws.emit(o);
  });
  const q = (f, a) => page.evaluate(f, a), st = () => q(() => document.getElementById("status").textContent);
  const sent = (t) => q((t) => window.__sent.filter((m) => m.t === t).length, t);
  // the page does not scroll, and STOP / RUN / STEP are inside the window
  const fits = () => q(() => {
    const inside = (id) => { const r = document.getElementById(id).getBoundingClientRect();
      return r.top >= 0 && r.bottom <= innerHeight && r.left >= 0 && r.right <= innerWidth && r.height > 0; };
    return document.scrollingElement.scrollHeight <= innerHeight && document.scrollingElement.scrollWidth <= innerWidth &&
      inside("stop") && inside("run") && inside("step");
  });
  const out = {};
  for (const [w, h] of [[1280, 800], [1024, 700]]) {
    await page.setViewportSize({ width: w, height: h });
    await page.goto("http://127.0.0.1:8765/logo"); await page.waitForTimeout(1500);
    const long = Array.from({ length: 300 }, (_, i) => "FORWARD " + (i + 1)).join("\n") + "\n";
    await page.locator("#src").fill(long); await page.waitForTimeout(1200);
    await q(() => { for (let i = 0; i < 200; i++) window.__fakeEmit({ t: "ev", kind: "logo_note", line: 1, msg: "note " + i }); });
    await page.waitForTimeout(300);
    out["fits" + w] = [await fits(),                                                         // true
      await q(() => { const s = document.getElementById("src"); return s.scrollHeight > s.clientHeight; })];  // true: the editor scrolls itself
  }
  // 1024 x 700 from here: a mistake shows PROBLEMS by itself, under the editor
  await page.locator("#src").fill("FORWAD 50\n"); await page.waitForTimeout(1200);
  out.problems = [await q(() => document.getElementById("cMsg").className),                 // card t-problems
    await q(() => document.getElementById("tProb").textContent),                            // PROBLEMS 1
    await q(() => document.getElementById("diags").parentNode.id)];                         // pslot
  await page.locator("#src").fill("REPEAT 4 [ FORWARD 50 RIGHT 90 ]\n"); await page.waitForTimeout(1200);
  // Ctrl+Enter runs (from inside the editor), a start shows MESSAGES, the status bar has the line and round
  await page.locator("#src").focus(); await page.keyboard.press("Control+Enter"); await page.waitForTimeout(100);
  await q(() => { window.__fakeEmit({ t: "ev", kind: "logo_started" }); window.__fakeEmit({ t: "ev", kind: "logo_line", line: 1, round: 3 }); });
  await page.waitForTimeout(100);
  out.run = [await sent("logo_run"), await q(() => document.getElementById("cMsg").className), await st(),   // 1, t-messages, line 1 · round 3
    await q(() => document.getElementById("help").classList.contains("hide"))];                              // true while running
  // Esc in the editor stops
  await page.keyboard.press("Escape"); await page.waitForTimeout(50);
  out.esc = [await sent("stop"), await sent("logo_stop")];                                 // 1, 1
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_ended", reason: 7040, line: 1, msg: "Stopped by you." }));
  await page.waitForTimeout(1300);
  // F10 starts in step mode; Ctrl+S saves (a new program asks for a name)
  await page.keyboard.press("F10"); await page.waitForTimeout(100);
  out.f10 = await q(() => window.__sent.filter((m) => m.t === "logo_run").map((m) => m.step).join());      // false,true
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_ended", reason: 7040, line: 1, msg: "Stopped by you." }));
  await page.waitForTimeout(1300);
  await page.keyboard.press("Control+s"); await page.waitForTimeout(100);
  out.save = await q(() => document.getElementById("nameS").classList.contains("on"));     // true
  await page.keyboard.press("Escape"); await page.waitForTimeout(50);
  out.escSheet = [await q(() => document.getElementById("nameS").classList.contains("on")), await sent("stop")];  // false, 2
  // narrow again: the phone layout, the problems and the help back under the editor
  await page.setViewportSize({ width: 390, height: 844 }); await page.waitForTimeout(300);
  out.phone = [await q(() => document.getElementById("diags").parentNode.id),               // cEdit
    await q(() => document.getElementById("help").parentNode.id),                           // cEdit
    await q(() => document.scrollingElement.scrollHeight > innerHeight)];                   // true: the phone page scrolls
  return out;
}
