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
  const PNG = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==", "base64");
  let streams = 0;  // a stand-in camera on :81: one small picture per request
  await page.route(/:81\/stream/, (r) => { streams++; r.fulfill({ status: 200, contentType: "image/png", body: PNG }); });
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
  // the menu button is the right-most thing in the top bar (owner, tank check)
  out.menuRight = await q(() => { const r = (id) => document.getElementById(id).getBoundingClientRect().right;
    return r("mbtn") > r("cUno") && r("mbtn") > r("stop") && r("mbtn") <= innerWidth; });                  // true
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
  // the camera: wanted on the laptop; HIDE drops the stream and the path grows; SHOW brings it back
  const pathH = () => q(() => document.getElementById("path").getBoundingClientRect().height);
  out.cam = [await q(() => /:81\/stream/.test(document.getElementById("camImg").src)), await pathH(), streams > 0];  // true, h, true
  await page.click("#camHide"); await page.waitForTimeout(300);
  out.camHidden = [await q(() => document.getElementById("camImg").hasAttribute("src")), await pathH(),     // false, larger
    await q(() => localStorage.getItem("logo.cam"))];                                                       // off
  const before = streams;
  await page.click("#camShow"); await page.waitForTimeout(300);
  out.camBack = [await q(() => /:81\/stream/.test(document.getElementById("camImg").src)), streams > before];  // true, true
  await q(() => window.__fakeEmit({ t: "tf", dist_cm: 64 })); await page.waitForTimeout(50);
  out.sonar = await q(() => document.getElementById("camDist").textContent);                                // ▸ 64 cm
  // final review: held keys are swallowed (no browser "save page" / F10 menu) but never act twice
  const key = (k, o) => q(([k, o]) => !window.dispatchEvent(new KeyboardEvent("keydown", Object.assign({ key: k, cancelable: true }, o))), [k, o]);
  const runs = await sent("logo_run");
  out.held = [await key("s", { ctrlKey: true, repeat: true }), await key("F10", { repeat: true }),             // true, true (prevented)
    await key("Enter", { ctrlKey: true, repeat: true }), (await sent("logo_run")) === runs];                 // true, true (no second run)
  out.ime = [await key("Enter", { ctrlKey: true, isComposing: true }), (await sent("logo_run")) === runs];    // false, true (left to the IME)
  // Esc in the DELETE dialog closes it and stops
  const stops = await sent("stop");
  await page.click("#pdel"); await page.waitForTimeout(50); await page.keyboard.press("Escape"); await page.waitForTimeout(50);
  out.escDelete = [await q(() => document.getElementById("delS").classList.contains("on")), (await sent("stop")) > stops];  // false, true
  // a dropped link restarts the camera stream (a broken MJPEG stream keeps its last picture and fires no error)
  const s0 = streams;
  await q(() => window.__ws.close()); await page.waitForTimeout(900);
  out.camRelink = streams > s0;                                                                              // true
  // a hidden page drops the stream; visible again brings it back
  await q(() => { Object.defineProperty(document, "hidden", { configurable: true, get: () => true }); document.dispatchEvent(new Event("visibilitychange")); });
  const hiddenSrc = await q(() => document.getElementById("camImg").hasAttribute("src"));
  await q(() => { Object.defineProperty(document, "hidden", { configurable: true, get: () => false }); document.dispatchEvent(new Event("visibilitychange")); });
  await page.waitForTimeout(300);
  out.camPageHidden = [hiddenSrc, await q(() => document.getElementById("camImg").hasAttribute("src"))];    // false, true
  // short laptop windows: the run pane (camera, path, stats) stays inside its card, above the status bar
  for (const [w, h] of [[1366, 657], [1280, 600]]) {
    await page.setViewportSize({ width: w, height: h }); await page.waitForTimeout(300);
    out["short" + w] = await q(() => { const b = (sel) => document.querySelector(sel).getBoundingClientRect();
      return b("#cPath .stats").bottom <= b("#cPath").bottom + 0.5 && b("#cPath").bottom <= b("#bar").top + 0.5 &&
        document.scrollingElement.scrollHeight <= innerHeight; });                                           // true
  }
  // narrow again: the phone layout, no camera stream, the problems back under the editor
  await page.setViewportSize({ width: 390, height: 844 }); await page.waitForTimeout(300);
  out.phone = [await q(() => document.getElementById("camImg").hasAttribute("src")),        // false
    await q(() => document.getElementById("diags").parentNode.id),                          // cEdit
    await q(() => document.getElementById("help").parentNode.id),                           // cEdit
    await q(() => document.scrollingElement.scrollHeight > innerHeight)];                   // true: the phone page scrolls
  return out;
}
