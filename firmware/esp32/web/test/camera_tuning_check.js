// Bridge LLD R24 browser check (Playwright run_code): the Camera card's white balance and night switch.
// Run the stand-in tank (python3 firmware/esp32/web/test/fake_tank.py 8765) and open /settings (packed, icons in).
async (page) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.addInitScript(() => {
    window.__sent = [];
    const cam = { size: "QVGA", quality: 12, fps_cap: 0, brightness: 0, contrast: 0, low_light: false, vflip: true,
                  hmirror: false, wb: "auto", night: false, viewers: 0, fps: 0, kbps: 0 };
    class FakeWS {
      constructor(url) { this.url = url; this.readyState = 0; window.__ws = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
      send(s) { const m = JSON.parse(s); window.__sent.push(m);
        const tank = () => this.emit({ t: "tank", selftest: { seen: false, mask: 0, locked: false, age_s: 0 },
                                       hw: { encoders: true, imu: true, us: true }, camera: cam, ai_url: "" });
        if (m.t === "hello") this.emit({ t: "welcome", you: "settings-1", tank: { fw: { uno: "1.0" } } });
        if (m.t === "tank") tank();
        if (m.t === "camera_set") { Object.assign(cam, m); delete cam.t; delete cam.id; this.emit({ t: "ack", id: m.id }); tank(); } }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { this.readyState = 3; this.onclose && this.onclose(); }
    }
    window.WebSocket = FakeWS;
  });
  const q = (f, a) => page.evaluate(f, a);
  const out = {};
  await page.goto("http://127.0.0.1:8765/settings?v=" + Date.now()); await page.waitForTimeout(500);
  await page.click('#tabs button[data-tab="tank"]'); await page.waitForTimeout(300);
  out.wbShown = await q(() => [...document.querySelectorAll("#cWb button.on")].map((b) => b.dataset.v));   // ["auto"]
  out.nightShown = await q(() => document.getElementById("cNight").textContent);                        // OFF
  await q(() => window.__sent.length = 0);
  await page.click('#cWb button[data-v="home"]'); await page.waitForTimeout(100);
  await page.click("#cNight"); await page.waitForTimeout(100);
  out.sent = await q(() => window.__sent.filter((m) => m.t === "camera_set").map((m) => [m.wb, m.night]));  // [["home",undefined],[undefined,true]]
  out.wbAfter = await q(() => [...document.querySelectorAll("#cWb button.on")].map((b) => b.dataset.v));   // ["home"]
  out.nightAfter = await q(() => document.getElementById("cNight").textContent);                        // ON
  out.hints = await q(() => document.getElementById("t-tank").innerText.includes("for looking, not for driving"));  // true
  out.wide = await q(() => document.documentElement.scrollWidth <= 390);
  return out;
}
