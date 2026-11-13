// Plan G3 browser check (Playwright run_code): the GAMES card on /ai against the stand-in tank
// firmware/esp32/web/test/fake_tank.py on :8765, the sockets faked as in ai_check.js.
async (page) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const checked = [];
  let busy = 0, streams = 0;  // busy: the next checks answer 503 (another /logo page checking)
  await page.route("**/logo/check", async (r) => { checked.push(r.request().postData());
    if (busy > 0) { busy--; await r.fulfill({ status: 503, contentType: "application/json", body: '{"ok":false,"why":"BUSY"}' }); return; }
    await r.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, prog: 7, diags: [] }) }); });
  await page.route("**/stream*", (r) => { streams++; r.abort(); });
  await page.route("**/logo/programs/chase_with_the_baby", (r) => r.fulfill({ status: 200, contentType: "text/plain",
    body: "; Lesson 6 - chase with the baby\nTO CHASE :cm :near :far\n  WAIT 1\nEND\nBABYGAME 70 100 5\n" }));  // no tank camera in the stand-in
  await page.addInitScript(() => {
    window.__tank = []; window.__feed = [];
    async function jpeg(w, h, colour) {
      const c = new OffscreenCanvas(w, h), x = c.getContext("2d"); x.fillStyle = colour; x.fillRect(0, 0, w, h);
      return new Uint8Array(await (await c.convertToBlob({ type: "image/jpeg" })).arrayBuffer());
    }
    async function frame(withMarks) {
      const cam = await jpeg(320, 240, "#4080c0"), dep = await jpeg(160, 120, "#00ff00");
      // R33: a class picture, all low (red), zlib-packed (final review I1); none from an "old engine"
      const marks = withMarks === false ? new Uint8Array(0) : new Uint8Array(await new Response(
        new Blob([new Uint8Array(4 * 3).fill(2)]).stream().pipeThrough(new CompressionStream("deflate"))).arrayBuffer());
      const h = { seq: 1, t_ms: 1000, boxes: [["toy", 0.91, 100, 100, 300, 300]], cam: cam.length, depth: dep.length };
      if (marks.length) h.marks = [4, 3, marks.length];
      const head = new TextEncoder().encode(JSON.stringify(h));
      const b = new Uint8Array(4 + head.length + cam.length + dep.length + marks.length);
      new DataView(b.buffer).setUint32(0, head.length);
      b.set(head, 4); b.set(cam, 4 + head.length); b.set(dep, 4 + head.length + cam.length);
      b.set(marks, 4 + head.length + cam.length + dep.length);
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
        const f = await frame(sessionStorage.getItem("noMarks") !== "1"); setTimeout(() => this.onmessage && this.onmessage({ data: f }), 30);
      }
      send(s) { const m = JSON.parse(s); (this.feed ? window.__feed : window.__tank).push(m);
        if (!this.feed && m.t === "hello") { this.emit({ t: "welcome", you: "ai-1" }); this.emit({ t: "ai_at", url: "ws://127.0.0.1:8770/feed", proto: 1 }); }
        // the bridge answers the link's ping: a silent stand-in made STOP open its own short socket too (R31, quiet > 1.5 s)
        if (!this.feed && m.t === "ping") this.emit({ t: "pong" }); }
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
  const txt = (id) => q((id) => document.getElementById(id).textContent, id);
  const feed = (o) => q((o) => window.__feedWs.emit(o), o), tank = (o) => q((o) => window.__tankWs.emit(o), o);
  out.locked = await txt("gameSt");                                                          // LOCKED
  out.lockedOff = await q(() => document.getElementById("gameGo").disabled);                 // true
  await feed({ t: "status", state: "ready", mood: "happy", say: "Ready", health: "ok", learning: false, people: true });
  await page.waitForTimeout(100);
  out.ready = await txt("gameSt");                                                           // READY
  await page.click("#cardGame summary");  // the grown-ups' fold
  await page.fill("#gMin", "3");
  await page.click("#gameGo"); await page.waitForTimeout(400);
  out.checkedEnds = (checked[checked.length - 1] || "").trimEnd().split("\n").pop();        // BABYGAME 60 80 3 (the card defaults, §248)
  out.ran = await q(() => window.__tank.filter((m) => m.t === "logo_run").length);           // 1
  out.ranLine = await txt("ranLine");                                                        // lesson 6, ...
  await tank({ t: "ev", kind: "game", state: "playing", caught: 0 });
  await feed({ t: "status", state: "busy", mood: "happy", say: "", health: "ok", learning: false, people: true,
    person: { seen: true, d_cm: 72, coming: 1 } });
  await page.waitForTimeout(100);
  out.playing = [await txt("gameSt"), await txt("gameSay")];                                 // PLAYING, 72 cm coming
  await tank({ t: "ev", kind: "game", state: "caught", caught: 1 }); await page.waitForTimeout(60);
  out.caught = await txt("gameSt");                                                          // CAUGHT!
  await tank({ t: "ev", kind: "game", state: "held", caught: 1 }); await page.waitForTimeout(60);
  out.held = [await txt("gameSt"), await txt("gameSay")];                                    // HOLDING STILL, STOP
  out.link = await q(() => document.querySelector("#cardGame a").getAttribute("href"));      // /logo#chase_with_the_baby
  out.noHScroll = await q(() => document.documentElement.scrollWidth <= window.innerWidth);
  out.pass = out.locked === "LOCKED" && out.lockedOff && out.ready === "READY" && out.checkedEnds === "BABYGAME 60 80 3" &&
    out.ran === 1 && out.ranLine === "lesson 6, chase with the baby" && out.playing[0] === "PLAYING" &&
    /72 cm away, coming/.test(out.playing[1]) && out.caught === "CAUGHT!" && out.held[0] === "HOLDING STILL" &&
    /STOP/.test(out.held[1]) && out.link === "/logo#chase_with_the_baby" && out.noHScroll;
  return out;
}
