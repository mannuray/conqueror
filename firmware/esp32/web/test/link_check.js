// Plan Link browser check (Playwright run_code; bridge LLD R31.6): the one link and STOP wiring on each page of the
// stand-in tank firmware/esp32/web/test/fake_tank.py on :8765. The tank's WebSocket is faked here: __mute makes the
// tank silent (open, never answers), __down makes new sockets never open.
async (page) => {
  const BASE = "http://127.0.0.1:8765";
  // each page: what its STOP sends, and where it says it could not reach the tank
  const PAGES = [
    { path: "/ai", stop: "stop,logo_stop", said: () => document.getElementById("typedMsg").textContent },
    { path: "/logo", stop: "stop,logo_stop", said: () => document.getElementById("log").innerText },
    { path: "/settings", stop: "stop,logo_stop", said: () => document.getElementById("toast").textContent },
  ];
  const fails = [];
  const ok = (c, m) => { if (!c) fails.push(m); };
  await page.route("**/stream*", (r) => r.abort());
  await page.addInitScript(() => {
    window.__sockets = [];
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
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto(BASE + p.path); await page.waitForTimeout(400);
    await q(() => { window.__mute = false; window.__down = false; });
    // STOP on a touch that never becomes a click (a sliding tap)
    await q(() => { window.__sockets[window.__sockets.length - 1].sent = [];
      document.getElementById("stop").dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, cancelable: true })); });
    await page.waitForTimeout(50);
    const sent = await q(() => window.__sockets[window.__sockets.length - 1].sent.join());
    ok(sent === p.stop, p.path + ": a touch on STOP sent '" + sent + "'");
    // a silent tank: closed after 4 s, RECONNECTING, then a new socket
    const n0 = await q(() => window.__sockets.length);
    await q(() => { window.__wifiSeen = []; const t = document.getElementById("wifiTxt");  // every text the WiFi chip shows
      new MutationObserver(() => window.__wifiSeen.push(t.textContent)).observe(t, { childList: true, characterData: true, subtree: true });
      window.__mute = true; });
    await page.waitForTimeout(3500);
    ok(await q(() => !window.__sockets[window.__sockets.length - 1].closed), p.path + ": the link was dropped before 4 s of silence");
    await page.waitForTimeout(2200);
    ok(await q(() => window.__wifiSeen.indexOf("RECONNECTING") >= 0), p.path + ": a silent link was never shown as RECONNECTING");
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
  return fails.length ? "FAIL\n" + fails.join("\n") : "PASS link_check (" + PAGES.map((p) => p.path).join(" ") + ")";
}
