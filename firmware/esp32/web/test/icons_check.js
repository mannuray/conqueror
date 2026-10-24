// Bridge LLD R25 browser check (Playwright run_code): the shared Lucide icons on Settings and Drive.
// Run the stand-in tank (python3 firmware/esp32/web/test/fake_tank.py 8765); it serves the packed pages (icons in).
// An icon is told apart by its parts: wifi 4, wifi-zero 1, battery-full 5, battery-low 3, plug 4, cpu 10.
async (page) => {
  await page.addInitScript(() => {
    class FakeWS {
      constructor(url) { this.readyState = 0; window.__ws = this; setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
      send(s) { const m = JSON.parse(s); if (m.t === "hello") this.emit({ t: "welcome", you: "x-1", tank: { fw: { uno: "1.0" } } }); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { this.readyState = 3; this.onclose && this.onclose(); }
    }
    window.WebSocket = FakeWS;
  });
  let rssi = -50;
  await page.route(/\/info$/, (r) => r.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ net: { rssi } }) }));
  const q = (f, a) => page.evaluate(f, a);
  const parts = (sel) => q((sel) => { const g = document.querySelector(sel + " svg"); return g ? g.children.length : 0; }, sel);
  const emit = async (o) => { await q((o) => window.__ws.emit(o), o); await page.waitForTimeout(80); };
  const out = {};
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.goto("http://127.0.0.1:8765/settings"); await page.waitForTimeout(600);
  await emit({ t: "wifi_status", mode: "home", rssi: -50 }); const w1 = await parts("#cWifi");
  await emit({ t: "wifi_status", mode: "home", rssi: -80 });
  out.settingsWifi = [w1, await parts("#cWifi"), await q(() => document.getElementById("cWifi").textContent.trim())];  // 4, 1, HOME
  // the battery is averaged like Drive's (10 readings, started again between USB and the pack): USB in between
  await emit({ t: "ts", batt_v: 8.2, flags: [] }); const b1 = await parts("#cBatt");
  await emit({ t: "ts", batt_v: 5.0, flags: [] }); const b2 = await parts("#cBatt");
  await emit({ t: "ts", batt_v: 7.0, flags: ["batt_low"] });
  out.settingsBatt = [b1, b2, await parts("#cBatt"), await q(() => document.getElementById("cBatt").className)];      // 5, 4, 3, chip warn
  out.settingsUno = await parts("#cUno");                                                                         // 10 (cpu)
  out.noBars = await q(() => document.querySelectorAll(".bars").length);                                           // 0
  await page.setViewportSize({ width: 844, height: 390 });
  await page.goto("http://127.0.0.1:8765/"); await page.waitForTimeout(800);
  const d1 = await parts("#wifiChip");
  rssi = -80; await q(() => window.dispatchEvent(new Event("focus"))); await page.goto("http://127.0.0.1:8765/"); await page.waitForTimeout(800);
  out.driveWifi = [d1, await parts("#wifiChip")];                                                                   // 4, 1
  return out;
}
