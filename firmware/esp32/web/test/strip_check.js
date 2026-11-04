// Plan Strip browser check (Playwright run_code; bridge LLD R30.4): the one status strip on /ai, /logo and /settings
// of the stand-in tank firmware/esp32/web/test/fake_tank.py on :8765. The tank's WebSocket is faked here; window.__emit
// sends the page a tank message.
async (page) => {
  const BASE = "http://127.0.0.1:8765";
  const PAGES = ["/ai"];
  const ORDER = "cBatt,cAi,cFps,cHdg,cPos,cSpd,cWifi,cUno";
  const fails = [];
  const ok = (c, m) => { if (!c) fails.push(m); };
  await page.route("**/stream*", (r) => r.abort());
  await page.addInitScript(() => {
    window.__ws = null;
    class FakeWS {
      constructor(url) { this.url = url; this.readyState = 0; if (url.indexOf(":8770") < 0) window.__ws = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 10); }
      send(s) { const m = JSON.parse(s); if (m.t === "hello" && this === window.__ws) this.emit({ t: "welcome", you: "x-1", tank: { fw: { uno: "1.0" } } }); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { if (this.readyState === 3) return; this.readyState = 3; setTimeout(() => this.onclose && this.onclose({}), 1); }
    }
    window.WebSocket = FakeWS;
    window.__emit = (o) => window.__ws.emit(o);
  });
  const LOOK = ["background-color", "color", "border-top-color", "border-top-left-radius", "font-family", "font-size", "padding-left", "height"];
  const look = () => page.evaluate((P) => { const cs = getComputedStyle(document.getElementById("cHdg")); return P.map((p) => cs.getPropertyValue(p)).join("|"); }, LOOK);
  const text = (id) => page.evaluate((id) => document.getElementById(id).textContent.trim(), id);
  const emit = async (o) => { await page.evaluate((o) => window.__emit(o), o); await page.waitForTimeout(40); };
  const looks = {};
  for (const theme of ["cockpit", "play"]) for (const [w, h] of [[390, 844], [1440, 900]]) for (const p of PAGES) {
    const at = p + " " + w + "px " + theme;
    await page.setViewportSize({ width: w, height: h });
    await page.goto(BASE + p); await page.evaluate((t) => localStorage.setItem("cq_theme", t), theme); await page.reload();
    await page.waitForTimeout(300);
    ok(await page.evaluate(() => Array.from(document.querySelectorAll("#cqStrip > .sc")).map((x) => x.id).join()) === ORDER, at + ": chips not in order");
    const fpsShown = await page.evaluate(() => !document.getElementById("cFps").hidden);
    ok(fpsShown === (p === "/ai"), at + ": pictures/s shown " + fpsShown);
    ok(await page.evaluate(() => document.scrollingElement.scrollWidth <= innerWidth), at + ": sideways scroll");
    const key = w + theme;
    if (!looks[key]) looks[key] = await look();
    else ok((await look()) === looks[key], at + ": the chips look different from /ai: " + (await look()) + " vs " + looks[key]);
    if (w !== 390 || theme !== "cockpit") continue;
    // the values, from the tank's messages
    ok((await text("cUno")) === "UNO ONLINE", at + ": UNO after welcome is " + (await text("cUno")));
    await emit({ t: "ts", batt_v: 7.6, flags: [] });
    ok((await text("cBatt")) === "50 %", at + ": battery " + (await text("cBatt")));
    await emit({ t: "tf", yaw: 91.6, spd_cmd: [-40, 25] });
    ok((await text("cHdg")) === "HDG 92°" && (await text("cSpd")) === "SPEED 40%", at + ": HDG / SPEED " + (await text("cHdg")) + " " + (await text("cSpd")));
    await emit({ t: "wifi_status", mode: "home", rssi: -50 });
    ok((await text("cWifi")) === "HOME" && await page.evaluate(() => document.querySelector("#cWifi svg.on").getAttribute("data-w")) === "wifi", at + ": WiFi chip");
    await emit({ t: "ts", batt_v: 5.0, flags: ["body_offline"] });
    ok((await text("cBatt")) === "USB" && (await text("cUno")) === "UNO OFFLINE", at + ": USB / UNO OFFLINE " + (await text("cBatt")) + " " + (await text("cUno")));
    if (p !== "/ai") {
      await emit({ t: "ai_at", url: "ws://127.0.0.1:8770/feed", proto: 1 });
      ok((await text("cAi")) === "AI HELPER", at + ": ai_at did not turn AI HELPER on");
      await emit({ t: "ev", kind: "logo_pose", x_cm: 30, y_cm: 50, heading: 0 });
      ok((await text("cPos")) === "POS 0.3,0.5", at + ": POS " + (await text("cPos")));
    }
    await page.evaluate(() => window.__ws.close()); await page.waitForTimeout(60);
    ok((await text("cWifi")) === "RECONNECTING", at + ": a dropped socket shows " + (await text("cWifi")));
  }
  return fails.length ? "FAIL\n" + fails.join("\n") : "PASS strip_check (" + PAGES.join(" ") + ")";
}
