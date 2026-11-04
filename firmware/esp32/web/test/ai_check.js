// Plan Z2b-1 Task 11 browser check (Playwright run_code): the /ai page on the stand-in tank
// firmware/esp32/web/test/fake_tank.py on :8765. Both WebSockets are faked here, told apart by URL: the tank's /ws
// (hello -> welcome + ai_at) and the engine's :8770/feed (hello, things, status, one binary frame).
async (page) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const checked = [];
  await page.route("**/logo/check", async (r) => { checked.push(r.request().postData());
    await r.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, prog: 7, diags: [] }) }); });
  await page.route("**/stream*", (r) => r.abort());  // no tank camera in the stand-in
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
  out.pass = out.drawn && out.say === "Ready to play!" && out.checked === 'AREA 2  FIND "toy' && out.ran.join() === "7" &&
    out.stopOnTank && !out.stopOnFeed && out.unknown === "I don't know what a cup is." && out.teachShown &&
    JSON.stringify(out.teach) === '[["cup",["mug"]]]' && out.noAi === "AI helper not running - start cq-drive serve" &&
    out.stopStill === 2 && out.noHScroll;
  return out;
}
