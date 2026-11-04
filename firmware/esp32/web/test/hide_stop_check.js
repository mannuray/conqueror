// design-notes §201 browser check (Playwright run_code): the drive page's hide / close STOP only when it might be
// driving. Stand-in tank firmware/esp32/web/test/fake_tank.py on :8765; the WebSocket is faked here.
async (page) => {
  await page.setViewportSize({ width: 844, height: 390 });
  await page.addInitScript(() => {
    window.__sent = [];
    class FakeWS {
      constructor(url) { this.url = url; this.readyState = 0; window.__ws = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
      send(s) { const m = JSON.parse(s); window.__sent.push(m);
        if (m.t === "hello") this.emit({ t: "welcome", you: "phone-1", owner: "none", owner_client: "", tank: { fw: { uno: "1.0" } } }); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { this.readyState = 3; this.onclose && this.onclose(); }
    }
    window.WebSocket = FakeWS;
  });
  const q = (f, a) => page.evaluate(f, a);
  const owner = (o, client) => q(([o, c]) => window.__ws.emit({ t: "ev", kind: "control_changed", owner: o, client: c }), [o, client]);
  const vis = (hidden) => q((h) => { Object.defineProperty(document, "hidden", { configurable: true, get: () => h });
    document.dispatchEvent(new Event("visibilitychange")); }, hidden);
  const stops = () => q(() => window.__sent.filter((m) => m.t === "stop").length);
  // hide with this owner: how many STOPs went out; then shown again (a new socket, welcome owner none)
  async function hideWith(o, client) {
    await owner(o, client); await page.waitForTimeout(50);
    const before = await stops();
    await vis(true); await page.waitForTimeout(50);
    const n = (await stops()) - before;
    await vis(false); await page.waitForTimeout(200);
    return n;
  }
  const out = {};
  await page.goto("http://127.0.0.1:8765/"); await page.waitForTimeout(800);
  out.otherPhone = await hideWith("human", "phone-2");   // 0: another phone drives
  out.program = await hideWith("human", "phone-15");      // 0: a program from another client drives (owner human)
  out.ir = await hideWith("ir", "");                      // 0: the IR remote drives
  out.me = await hideWith("human", "phone-1");            // 1: this page drives
  out.nobody = await hideWith("none", "");                // 1: no owner - sent as before
  // pagehide while another phone drives: nothing
  await owner("human", "phone-2"); await page.waitForTimeout(50);
  let before = await stops();
  await q(() => window.dispatchEvent(new Event("pagehide"))); await page.waitForTimeout(50);
  out.pagehideOther = (await stops()) - before;           // 0
  await owner("human", "phone-1"); await page.waitForTimeout(50);
  before = await stops();
  await q(() => window.dispatchEvent(new Event("pagehide"))); await page.waitForTimeout(50);
  out.pagehideMe = (await stops()) - before;              // 1
  out.pass = out.otherPhone === 0 && out.program === 0 && out.ir === 0 && out.me === 1 && out.nobody === 1 &&
    out.pagehideOther === 0 && out.pagehideMe === 1;
  return out;
}
