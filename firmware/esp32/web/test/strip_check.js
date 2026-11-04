// Plan Strip browser check (Playwright run_code; bridge LLD R30.4): the one status strip on /ai, /logo and /settings
// of the stand-in tank firmware/esp32/web/test/fake_tank.py on :8765. The tank's WebSocket is faked here; window.__emit
// sends the page a tank message.
async (page) => {
  const BASE = "http://127.0.0.1:8765";
  const PAGES = ["/ai", "/logo", "/settings"];
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
  // final review: every chip, its parts and their ::before / ::after (a page's own .dot::after once reached the strip)
  // the AI HELPER chip is set alike for the measure (/ai knows from its feed, the others do not) and put back after
  const look = () => page.evaluate((P) => { const was = cqStrip.view.ai; cqStrip.view.ai = false; cqStrip.render();
    const out = Array.from(document.querySelectorAll("#cqStrip, #cqStrip *")).filter((el) => !el.closest("#cFps")).map((el) =>  // pictures/s: /ai only
      [null, "::before", "::after"].map((ps) => { const cs = getComputedStyle(el, ps); return el.id + (ps || "") + ":" + P.map((p) => cs.getPropertyValue(p)).join("|") + "|" + cs.content; }).join(";")).join("\n");
    cqStrip.view.ai = was; cqStrip.render(); return out; }, LOOK);
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
    else { const mine = (await look()).split("\n"), theirs = looks[key].split("\n"), i = mine.findIndex((x, j) => x !== theirs[j]);
      ok(i < 0, at + ": the strip looks different from /ai at " + mine[i] + " vs " + theirs[i]); }
    // the top bar stays on the first row with the strip in the header (laptop sizes in between too)
    for (const lw of w === 1440 && theme === "cockpit" ? [1024, 1280, 1600] : []) {
      await page.setViewportSize({ width: lw, height: 700 }); await page.waitForTimeout(150);
      // owner: the strip starts from the left, where it starts on /ai
      // (from 1500 px it follows the title on the first row: there the gap after the title is compared, titles differ)
      const at0 = await page.evaluate((wide) => { const r = document.getElementById("cBatt").getBoundingClientRect();
        const t = document.querySelector("header .title, .top h1").getBoundingClientRect();
        return [Math.round(wide ? r.left - t.right : r.left), Math.round(r.top)]; }, lw >= 1500);
      if (p === "/ai") looks["pos" + lw] = at0;
      else ok(Math.abs(at0[0] - looks["pos" + lw][0]) <= 1 && Math.abs(at0[1] - looks["pos" + lw][1]) <= 1,
        p + " " + lw + "px: the strip starts at " + at0 + ", on /ai at " + looks["pos" + lw]);
      ok(await page.evaluate(() => document.getElementById("stop").getBoundingClientRect().top < 20), p + " " + lw + "px: the top bar left the first row");
      ok(await page.evaluate(() => document.scrollingElement.scrollWidth <= innerWidth), p + " " + lw + "px: sideways scroll");
      await page.setViewportSize({ width: w, height: h });
    }
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
      // the bridge tells only /ai where the AI helper is: here the chip says it does not know, in the normal colour
      ok((await text("cAi")) === "AI HELPER --" && await page.evaluate(() => document.getElementById("cAi").className === "sc"), at + ": AI HELPER without ai_at is " + (await text("cAi")));
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
