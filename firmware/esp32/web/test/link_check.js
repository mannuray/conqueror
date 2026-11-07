// Plan Link browser check (Playwright run_code; bridge LLD R31.6): the one link and STOP wiring on each page of the
// stand-in tank firmware/esp32/web/test/fake_tank.py on :8765. The tank's WebSocket is faked here: __mute makes the
// tank silent (open, never answers), __down makes new sockets never open.
async (page) => {
  const BASE = "http://127.0.0.1:8765";
  // each page: what its STOP sends, where it says it could not reach the tank, and where it shows the link lost
  const PAGES = [
    { path: "/", stop: "stop", said: () => document.getElementById("toast").textContent, lost: ["latChip", "OFF"], size: [844, 390] },
    { path: "/ai", stop: "stop,logo_stop", said: () => document.getElementById("typedMsg").textContent, lost: ["wifiTxt", "RECONNECTING"] },
    { path: "/logo", stop: "stop,logo_stop", said: () => document.getElementById("log").innerText, lost: ["wifiTxt", "RECONNECTING"] },
    { path: "/settings", stop: "stop,logo_stop", said: () => document.getElementById("toast").textContent, lost: ["wifiTxt", "RECONNECTING"] },
  ];
  const fails = [];
  const ok = (c, m) => { if (!c) fails.push(m); };
  await page.route("**/stream*", (r) => r.abort());
  await page.addInitScript(() => {
    window.__sockets = [];
    window.__down = sessionStorage.getItem("down") === "1";  // a tank out of reach from the first load
    class FakeWS {
      constructor(url) {
        this.url = url; this.readyState = 0; this.sent = []; this.closed = false;
        if (url.indexOf(":8770") >= 0) return;  // the AI helper's feed: never opens here
        window.__sockets.push(this);
        if (window.__down) return;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 10);
      }
      send(s) {
        const m = JSON.parse(s); this.sent.push(m.t);
        if (window.__mute) return;
        if (m.t === "hello") this.emit({ t: "welcome", you: "x-1", tank: { fw: { uno: "1.0" } } });
        if (m.t === "ping") this.emit({ t: "pong" });
        if (m.id !== undefined) this.emit({ t: "ack", id: m.id });
      }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { if (this.closed) return; this.closed = true; this.readyState = 3; setTimeout(() => this.onclose && this.onclose({}), 1); }
    }
    window.WebSocket = FakeWS;
  });
  const q = (f, a) => page.evaluate(f, a);
  for (const p of PAGES) {
    await page.setViewportSize(p.size ? { width: p.size[0], height: p.size[1] } : { width: 390, height: 844 });
    await page.goto(BASE + p.path); await page.waitForTimeout(400);
    await q(() => { window.__mute = false; window.__down = false; });
    // STOP on a touch that never becomes a click (a sliding tap) - with the menu open, which it closes (final review)
    await page.click("#menuBtn");
    await q(() => { window.__sockets[window.__sockets.length - 1].sent = [];
      document.getElementById("stop").dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, cancelable: true })); });
    await page.waitForTimeout(50);
    const sent = await q(() => window.__sockets[window.__sockets.length - 1].sent.join());
    ok(sent === p.stop, p.path + ": a touch on STOP sent '" + sent + "'");
    ok(await q(() => !cqBar.isOpen()), p.path + ": a sliding STOP tap left the menu open");
    // a silent tank: closed after 4 s, RECONNECTING, then a new socket
    const n0 = await q(() => window.__sockets.length);
    await q((id) => { window.__wifiSeen = []; const t = document.getElementById(id);  // every text the link chip shows
      new MutationObserver(() => window.__wifiSeen.push(t.textContent)).observe(t, { childList: true, characterData: true, subtree: true });
      window.__mute = true; }, p.lost[0]);
    await page.waitForTimeout(3500);
    ok(await q(() => !window.__sockets[window.__sockets.length - 1].closed), p.path + ": the link was dropped before 4 s of silence");
    await page.waitForTimeout(2200);
    ok(await q((txt) => window.__wifiSeen.indexOf(txt) >= 0, p.lost[1]), p.path + ": a silent link was never shown as " + p.lost[1]);
    ok(await q(() => window.__sockets.length) > n0, p.path + ": no new link after a silent one");
    // the link down: STOP through its own socket, and the page says it could not reach the tank
    await q(() => { window.__down = true; window.__sockets.forEach((s) => s.close()); });
    await page.waitForTimeout(100);
    const n1 = await q(() => window.__sockets.length);
    await page.click("#stop");
    await page.waitForTimeout(2400);
    const said = await q(p.said);
    ok(/Couldn't reach the tank - use another STOP/.test(said), p.path + ": STOP with the link down said '" + said.slice(-80) + "'");
    ok(await q((n) => window.__sockets.slice(n).some((s) => s.closed), n1), p.path + ": STOP's own socket was not opened and closed");
  }
  // the drive page before any link: its WiFi chip says off (red), never a green "-" (owner 2026-11-05: R31 lost the
  // old /info poll that turned it red)
  await page.setViewportSize({ width: 844, height: 390 });
  await page.goto(BASE + "/"); await q(() => sessionStorage.setItem("down", "1")); await page.reload(); await page.waitForTimeout(600);
  const chip = await q(() => { const c = document.getElementById("wifiChip"); return { cls: c.className, text: c.textContent.trim() }; });
  ok(/\bbad\b/.test(chip.cls) && chip.text !== "-", "/: before any link the WiFi chip is " + JSON.stringify(chip));
  await q(() => sessionStorage.removeItem("down"));
  return fails.length ? "FAIL\n" + fails.join("\n") : "PASS link_check (" + PAGES.map((p) => p.path).join(" ") + ")";
}
