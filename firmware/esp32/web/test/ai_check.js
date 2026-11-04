// Plan Z2b-1 Task 11 browser check (Playwright run_code): the /ai page on the stand-in tank
// firmware/esp32/web/test/fake_tank.py on :8765. Both WebSockets are faked here, told apart by URL: the tank's /ws
// (hello -> welcome + ai_at) and the engine's :8770/feed (hello, things, status, one binary frame).
async (page) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const checked = [];
  let busy = 0, streams = 0;  // busy: the next checks answer 503 (another /logo page checking)
  await page.route("**/logo/check", async (r) => { checked.push(r.request().postData());
    if (busy > 0) { busy--; await r.fulfill({ status: 503, contentType: "application/json", body: '{"ok":false,"why":"BUSY"}' }); return; }
    await r.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, prog: 7, diags: [] }) }); });
  await page.route("**/stream*", (r) => { streams++; r.abort(); });  // no tank camera in the stand-in
  await page.addInitScript(() => {
    window.__tank = []; window.__feed = [];
    async function jpeg(w, h, colour) {
      const c = new OffscreenCanvas(w, h), x = c.getContext("2d"); x.fillStyle = colour; x.fillRect(0, 0, w, h);
      return new Uint8Array(await (await c.convertToBlob({ type: "image/jpeg" })).arrayBuffer());
    }
    async function frame() {
      const cam = await jpeg(320, 240, "#4080c0"), dep = await jpeg(160, 120, "#00ff00");
      const head = new TextEncoder().encode(JSON.stringify({ seq: 1, t_ms: 1000, boxes: [["toy", 0.91, 100, 100, 300, 300]], cam: cam.length, depth: dep.length }));
      const b = new Uint8Array(4 + head.length + cam.length + dep.length);
      new DataView(b.buffer).setUint32(0, head.length);
      b.set(head, 4); b.set(cam, 4 + head.length); b.set(dep, 4 + head.length + cam.length);
      return b.buffer;
    }
    class FakeWS {
      constructor(url) {
        this.url = url; this.readyState = 0; this.feed = url.indexOf(":8770/feed") >= 0;
        if (this.feed) window.__feedWs = this; else window.__tankWs = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); if (this.feed) this.greet(); }, 20);
      }
      async greet() {
        this.emit({ t: "hello", proto: 1 });
        this.emit({ t: "things", list: [{ word: "toy", names: ["car"], measured: true }, { word: "ball", names: [], measured: true }] });
        this.emit({ t: "status", state: "ready", mood: "happy", say: "Ready to play!", health: "ok", learning: false });
        const f = await frame(); setTimeout(() => this.onmessage && this.onmessage({ data: f }), 30);
      }
      send(s) { const m = JSON.parse(s); (this.feed ? window.__feed : window.__tank).push(m);
        if (!this.feed && m.t === "hello") { this.emit({ t: "welcome", you: "ai-1" }); this.emit({ t: "ai_at", url: "ws://127.0.0.1:8770/feed", proto: 1 }); } }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { if (this.readyState === 3) return; this.readyState = 3; setTimeout(() => this.onclose && this.onclose(), 1); }
    }
    window.WebSocket = FakeWS;
    window.__feeds = [];  // every feed socket the page made (final review I2: never more than one open)
    const Real = FakeWS;
    window.WebSocket = class extends Real { constructor(url) { super(url); if (this.feed) window.__feeds.push(this); } };
    window.__spoken = [];  // the voice (final review I5): what was spoken, after which gesture
    window.SpeechSynthesisUtterance = function (t) { this.text = t; };
    Object.defineProperty(window, "speechSynthesis", { configurable: true,  // a read-only getter: replaced, not assigned
      value: { cancel() {}, speak(u) { window.__spoken.push(u.text); } } });
  });
  const q = (f, a) => page.evaluate(f, a);
  const out = {};
  await page.goto("http://127.0.0.1:8765/ai"); await page.waitForTimeout(1200);
  const lit = (id) => q((id) => { const c = document.getElementById(id), d = c.getContext("2d").getImageData(10, 10, 1, 1).data; return [d[0], d[1], d[2]]; }, id);
  out.view1 = await lit("v1");                                          // the blue picture
  out.view2 = await lit("v2");                                          // the picture under green depth: more green
  out.drawn = out.view1[2] > 120 && out.view2[1] > out.view1[1] + 40;   // true
  out.say = await q(() => document.getElementById("say").textContent);  // Ready to play!
  out.things = await q(() => document.getElementById("things").textContent);  // TOYBALL
  await page.click("#find"); await page.waitForTimeout(300);
  out.checked = checked[0];                                             // AREA 2  FIND "toy
  out.ran = await q(() => window.__tank.filter((m) => m.t === "logo_run").map((m) => m.prog));  // [7]
  await page.click("#stop"); await page.waitForTimeout(100);
  out.stopOnTank = await q(() => window.__tank.some((m) => m.t === "stop"));   // true
  out.stopOnFeed = await q(() => window.__feed.some((m) => m.t === "stop"));   // false
  await q(() => window.__tankWs.emit({ t: "ev", kind: "logo_ended", reason: "stop", line: 1, msg: "Stopped by you." }));
  await page.waitForTimeout(100);
  await page.fill("#typed", "cup"); await page.click("#typedGo"); await page.waitForTimeout(100);
  out.unknown = await q(() => document.getElementById("typedMsg").textContent);  // I don't know what a cup is.
  out.teachShown = await q(() => !document.getElementById("teach").classList.contains("hidden"));  // true
  await page.fill("#tNames", "mug"); await page.click("#tGo"); await page.waitForTimeout(100);
  out.teach = await q(() => window.__feed.filter((m) => m.t === "teach").map((m) => [m.word, m.names]));  // [["cup",["mug"]]]
  await q(() => window.__tankWs.emit({ t: "ai_at", url: null })); await page.waitForTimeout(200);
  out.noAi = await q(() => document.getElementById("camNote").textContent);  // AI helper not running - start cq-drive serve
  await page.click("#stop"); await page.waitForTimeout(100);
  out.stopStill = await q(() => window.__tank.filter((m) => m.t === "stop").length);  // 2
  out.noHScroll = await q(() => document.scrollingElement.scrollWidth <= innerWidth);  // true
  // Final review I2: the same ai_at again (each phone unlock / tank reconnect) never opens a second feed.
  await q(() => window.__tankWs.emit({ t: "ai_at", url: "ws://127.0.0.1:8770/feed", proto: 1 })); await page.waitForTimeout(300);
  for (let i = 0; i < 3; i++) { await q(() => window.__tankWs.emit({ t: "ai_at", url: "ws://127.0.0.1:8770/feed", proto: 1 })); await page.waitForTimeout(100); }
  await page.waitForTimeout(2500);
  out.openFeeds = await q(() => window.__feeds.filter((f) => f.readyState === 1).length);  // 1
  out.aiChip = await q(() => document.getElementById("cAi").textContent);                // ● AI HELPER
  // Final review I5: a tap (click) unlocks the voice; a sentence is then spoken.
  out.unlockSpoken = await q(() => window.__spoken.length > 0);                        // the silent unlock word went out
  await q(() => window.__feeds.find((f) => f.readyState === 1).emit({ t: "status", state: "looking", mood: "curious", say: "I'm looking round for the toy.", health: "ok" }));
  await page.waitForTimeout(100);
  out.spoken = await q(() => window.__spoken.filter((t) => t.trim()));                   // [..., "I'm looking round for the toy."]
  // Final review M4: a STOP with no logo_ended afterwards still frees the buttons.
  await q(() => window.__tankWs.emit({ t: "ai_at", url: "ws://127.0.0.1:8770/feed", proto: 1 }));
  await page.click("#find"); await page.waitForTimeout(200);
  await page.click("#stop"); await page.waitForTimeout(2200);
  out.freedAfterStop = await q(() => !document.getElementById("find").disabled);        // true
  // Final review M3: a busy check (503) is tried again, not called a failure.
  busy = 2; const ranBefore = await q(() => window.__tank.filter((m) => m.t === "logo_run").length);
  await page.click("#wander"); await page.waitForTimeout(1600);
  out.busyRetried = (await q(() => window.__tank.filter((m) => m.t === "logo_run").length)) - ranBefore;  // 1
  await q(() => window.__tankWs.emit({ t: "ev", kind: "logo_ended", reason: "finished", line: 1, msg: "" }));
  // Final review M5: without the AI helper, hiding and showing the page tries the tank's camera again.
  await q(() => window.__tankWs.emit({ t: "ai_at", url: null })); await page.waitForTimeout(200);
  const vis = (h) => q((h) => { Object.defineProperty(document, "hidden", { configurable: true, get: () => h });
    document.dispatchEvent(new Event("visibilitychange")); }, h);
  await vis(true); await page.waitForTimeout(100);
  const before = streams;
  await vis(false); await page.waitForTimeout(400);
  out.streamAgain = streams > before;                                                     // true
  out.pass = out.drawn && out.say === "Ready to play!" && out.checked === 'AREA 2  FIND "toy' && out.ran.join() === "7" &&
    out.stopOnTank && !out.stopOnFeed && out.unknown === "I don't know what a cup is." && out.teachShown &&
    JSON.stringify(out.teach) === '[["cup",["mug"]]]' && out.noAi === "AI helper not running - start cq-drive serve" &&
    out.stopStill === 2 && out.noHScroll && out.openFeeds === 1 && out.aiChip === "● AI HELPER" &&
    out.unlockSpoken && out.spoken[out.spoken.length - 1] === "I'm looking round for the toy." && out.freedAfterStop &&
    out.busyRetried === 1 && out.streamAgain;
  return out;
}
