// Plan Top Bar browser check (Playwright run_code; bridge LLD R29.5): the one top bar on each page of the stand-in
// tank firmware/esp32/web/test/fake_tank.py on :8765. The tank's WebSocket is faked here: it records what each page
// sends and acks every command with an id.
async (page) => {
  const BASE = "http://127.0.0.1:8765";
  // each page: its path, its title element, what its STOP sends (the drive page: landscape only, no title)
  const PAGES = [
    { path: "/", title: null, stop: ["stop"], sizes: [[844, 390], [1440, 900]] },
    { path: "/ai", title: "header .title", stop: ["stop", "logo_stop"] },
    { path: "/settings", title: ".top h1", stop: ["stop", "logo_stop"] },
    { path: "/logo", title: ".top h1", stop: ["stop", "logo_stop"] },
    { path: "/help", title: "header .title", stop: ["hello", "stop", "logo_stop"] },  // a socket opened for the press
  ];
  const SIZES = [[390, 844], [1440, 900]];
  const fails = [];
  const ok = (c, m) => { if (!c) fails.push(m); };
  await page.route("**/stream*", (r) => r.abort());  // no tank camera in the stand-in
  await page.addInitScript(() => {
    window.__sent = []; window.__sockets = [];
    class FakeWS {
      constructor(url) {
        this.url = url; this.readyState = 0; this.closed = false; window.__sockets.push(this);
        if (window.__down) return;  // __down: the tank is out of reach - a socket made now never opens
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 10);
      }
      send(s) {
        const m = JSON.parse(s);
        if (["hello", "stop", "logo_stop"].indexOf(m.t) >= 0) window.__sent.push(m.t);  // what STOP sends; not the pages' polls
        if (m.t === "hello") this.emit({ t: "welcome", you: "x-1" });
        if (m.id !== undefined && !window.__silent) this.emit({ t: "ack", id: m.id });  // __silent: a tank that never answers
      }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { if (this.closed) return; this.closed = true; this.readyState = 3; setTimeout(() => this.onclose && this.onclose({}), 1); }
    }
    window.WebSocket = FakeWS;
  });
  const rects = () => page.evaluate(() => ["lookBtn", "menuBtn", "stop"].map((id) => {
    const r = document.getElementById(id).getBoundingClientRect(); return { l: r.left, r: r.right, t: r.top, h: r.height };
  }));
  for (const p of PAGES) {
    for (const [w, h] of p.sizes || SIZES) {
      const at = p.path + " " + w + "px";
      await page.setViewportSize({ width: w, height: h });
      await page.goto(BASE + p.path);
      await page.waitForTimeout(300);
      // the three: one row, in order, 44 px, at the top right
      const [a, b, c] = await rects();
      ok(Math.abs(a.t - b.t) < 2 && Math.abs(b.t - c.t) < 2, at + ": the three are not in one row");
      ok(a.r <= b.l && b.r <= c.l, at + ": order is not look, menu, STOP");
      ok([a, b, c].every((x) => Math.abs(x.h - 44) < 1), at + ": heights " + [a.h, b.h, c.h]);
      ok(w - c.r <= 16 && c.r <= w, at + ": STOP's right edge " + c.r + " is not within 16 px of the window");
      ok(c.t < 80, at + ": the bar is not at the top (" + c.t + ")");
      const titleLines = await page.evaluate((sel) => { const e = document.querySelector(sel); if (!e) return -1;
        const cs = getComputedStyle(e), lh = parseFloat(cs.lineHeight) || parseFloat(cs.fontSize) * 1.3;  // "normal": about 1.2 of the font
        return Math.round(e.getBoundingClientRect().height / lh); }, p.title);
      ok(p.title === null || titleLines === 1, at + ": the title takes " + titleLines + " lines");
      ok(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), at + ": sideways scroll");
      // the menu: five entries in order, this page marked; Esc and the dim layer close it
      await page.click("#menuBtn");
      const menu = await page.evaluate(() => ({
        shown: !document.getElementById("cqMenu").hidden && !document.getElementById("cqDim").hidden,
        order: Array.from(document.querySelectorAll("#cqMenu a")).map((x) => x.getAttribute("data-page")).join(),
        current: Array.from(document.querySelectorAll('#cqMenu a[aria-current="page"]')).map((x) => x.getAttribute("data-page")).join(),
        // one entry under the other, every one inside the window (a page's own styles must not reach the menu)
        stacked: Array.from(document.querySelectorAll("#cqMenu a")).map((x) => x.getBoundingClientRect())
          .every((r, i, all) => r.left >= 0 && r.right <= innerWidth && (i === 0 || r.top >= all[i - 1].bottom)),
        bottom: document.querySelector("#cqMenu a:last-child").getBoundingClientRect().bottom,
      }));
      ok(menu.shown, at + ": the menu did not open");
      // final review: the dim layer really dims (a page without --dim-layer gave a see-through one); focus moves into the menu
      ok(await page.evaluate(() => { const c = getComputedStyle(document.getElementById("cqDim")).backgroundColor;
        return c !== "rgba(0, 0, 0, 0)" && c !== "transparent"; }), at + ": the dim layer is see-through");
      ok(await page.evaluate(() => document.activeElement === document.querySelector("#cqMenu a")), at + ": focus did not move to the first menu entry");
      ok(menu.order === "/,/logo,/ai,/settings,/help", at + ": menu order " + menu.order);
      ok(menu.current === p.path, at + ": marked " + menu.current);
      ok(menu.stacked, at + ": the menu entries are not one under the other inside the window");
      ok(menu.bottom <= h, at + ": the last menu entry ends below the window (" + menu.bottom + ")");
      // STOP still works with the menu open (Playwright refuses a click on a covered button)
      await page.evaluate(() => { window.__sent = []; });
      await page.click("#stop", { timeout: 2000 }).catch((e) => fails.push(at + ": STOP not clickable over the menu - " + e.message.split("\n")[0]));
      await page.waitForTimeout(100);
      const sent = await page.evaluate(() => window.__sent.join());
      ok(sent === p.stop.join(), at + ": STOP sent '" + sent + "', wanted '" + p.stop.join() + "'");
      ok(await page.evaluate(() => !cqBar.isOpen()), at + ": STOP left the menu open (its answer would sit under the dim layer)");
      await page.click("#menuBtn");
      await page.keyboard.press("Escape");
      ok(await page.evaluate(() => document.getElementById("cqMenu").hidden), at + ": Esc did not close the menu");
      ok(await page.evaluate(() => document.activeElement === document.getElementById("menuBtn")), at + ": focus did not go back to ☰");
      // Back on iOS brings the page back from its cache: the menu must not come back open
      await page.click("#menuBtn");
      await page.evaluate(() => window.dispatchEvent(new PageTransitionEvent("pageshow", { persisted: true })));
      ok(await page.evaluate(() => !cqBar.isOpen() && !document.documentElement.classList.contains("cq-open")), at + ": the menu stayed open after a page restore");
      await page.click("#menuBtn");
      await page.mouse.click(8, h - 60);
      ok(await page.evaluate(() => document.getElementById("cqMenu").hidden && document.getElementById("cqDim").hidden), at + ": the dim layer did not close the menu");
      await page.click("#menuBtn"); await page.click("#menuBtn");
      ok(await page.evaluate(() => document.getElementById("cqMenu").hidden), at + ": the menu button again did not close the menu");
      // the look flips and holds on the next load
      const before = await page.evaluate(() => document.documentElement.dataset.theme);
      await page.click("#lookBtn");
      const after = await page.evaluate(() => document.documentElement.dataset.theme);
      ok(before !== after, at + ": the look did not flip");
      await page.reload(); await page.waitForTimeout(200);
      ok(await page.evaluate(() => document.documentElement.dataset.theme) === after, at + ": the look did not hold");
    }
  }
  // Drive: [🔈][STICK] then the three in one row on a laptop; on a phone the three on top and [🔈][STICK] just under
  // them (the heading tape keeps the middle); the old ☰ MENU sheet and LOOK item are gone; one sheet at a time.
  for (const [w, h, oneRow] of [[1440, 900, true], [844, 390, false], [667, 375, false]]) {
    await page.setViewportSize({ width: w, height: h });
    await page.goto(BASE + "/"); await page.waitForTimeout(300);
    const g = await page.evaluate(() => { const r = (id) => document.getElementById(id).getBoundingClientRect();
      return { a: r("audioChip"), c: r("ctlChip"), l: r("lookBtn"), s: r("stop"), t: document.querySelector(".hdg").getBoundingClientRect() }; });
    const at = "/ " + w + "px";
    if (oneRow) ok(g.a.right <= g.c.left && g.c.right <= g.l.left && Math.abs((g.a.top + g.a.bottom) / 2 - (g.l.top + g.l.bottom) / 2) < 2,
      at + ": not one row [🔈][STICK][🎨][☰][STOP]");
    else ok(g.a.top >= g.s.bottom && g.c.right <= g.s.right + 0.5 && Math.abs(g.c.right - g.s.right) < 2 && g.a.right <= g.c.left,
      at + ": [🔈][STICK] are not right under the three");
    ok(g.l.left >= g.t.right && (oneRow ? g.a.left >= g.t.right : true), at + ": the top bar runs into the heading tape");
    const gauge = await page.evaluate(() => document.querySelector(".dist").getBoundingClientRect().top);
    ok(gauge >= Math.max(g.a.bottom, g.s.bottom), at + ": the distance gauge (top " + gauge + ") runs into the top bar or its chips");
  }
  await page.setViewportSize({ width: 844, height: 390 });
  await page.goto(BASE + "/"); await page.waitForTimeout(300);
  ok(await page.evaluate(() => !document.getElementById("menuSheet") && !document.getElementById("miLook") && !document.getElementById("dim")),
    "/: the old MENU sheet, LOOK item or dim layer is still there");
  await page.click("#moreBtn"); await page.click("#menuBtn");
  ok(await page.evaluate(() => !document.getElementById("moreSheet").classList.contains("show") && cqBar.isOpen()), "/: the menu did not close the MAX sheet");
  await page.evaluate(() => document.getElementById("moreBtn").click());
  ok(await page.evaluate(() => document.getElementById("moreSheet").classList.contains("show") && !cqBar.isOpen()), "/: the MAX sheet did not close the menu");
  // Help: STOP opens one socket, sends hello, stop, logo_stop, says "Stopped" on the ack and closes it; a tank that never
  // answers: "Couldn't reach the tank" after 2 s, closed; a double tap leaves no socket open
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(BASE + "/help"); await page.waitForTimeout(300);
  const helpState = () => page.evaluate(() => ({ msg: document.getElementById("stopMsg").textContent,
    shown: !document.getElementById("stopMsg").hidden, n: window.__sockets.length, open: window.__sockets.filter((x) => !x.closed).length }));
  ok(await page.evaluate(() => window.__sockets.length) === 0, "/help: a socket was open before STOP");
  await page.click("#stop"); await page.waitForTimeout(200);
  let st = await helpState();
  ok(st.shown && st.msg === "Stopped" && st.n === 1 && st.open === 0, "/help: STOP with an answer gave " + JSON.stringify(st));
  await page.evaluate(() => { window.__silent = true; });
  await page.click("#stop"); await page.waitForTimeout(500);
  st = await helpState();
  ok(st.msg === "Stopping…" && st.open === 1, "/help: while waiting it says '" + st.msg + "' (wanted Stopping…), or its socket is not open");
  await page.waitForTimeout(1800);
  st = await helpState();
  ok(st.msg === "Couldn't reach the tank - use another STOP" && st.open === 0, "/help: a silent tank gave " + JSON.stringify(st));
  await page.evaluate(() => { window.__silent = false; });
  await page.click("#stop"); await page.click("#stop"); await page.waitForTimeout(300);
  st = await helpState();
  ok(st.n === 4 && st.open === 0 && st.msg === "Stopped", "/help: a double tap gave " + JSON.stringify(st));
  // final review: only the latest press's answer shows (a late timeout of an earlier press never covers it)
  await page.evaluate(() => { window.__silent = true; });
  await page.click("#stop"); await page.waitForTimeout(50);
  await page.evaluate(() => { window.__silent = false; });
  await page.click("#stop"); await page.waitForTimeout(2300);
  st = await helpState();
  ok(st.msg === "Stopped" && st.open === 0, "/help: an earlier press's late answer showed: " + JSON.stringify(st));
  // final review: the answer is seen even when STOP is pressed far down the page
  await page.evaluate(() => window.scrollTo(0, document.body.scrollHeight)); await page.waitForTimeout(100);
  await page.click("#stop"); await page.waitForTimeout(200);
  ok(await page.evaluate(() => { const r = document.getElementById("stopMsg").getBoundingClientRect();
    return r.height > 0 && r.top >= 0 && r.bottom <= innerHeight; }), "/help: STOP's answer is off screen when pressed far down the page");
  // /logo: its own menu and back link are gone; Esc with the menu open closes it and still stops (Esc is STOP from
  // anywhere on /logo, even in a dialog, which it also closes - Tank Logo LLD §23)
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto(BASE + "/logo"); await page.waitForTimeout(300);
  ok(await page.evaluate(() => !document.getElementById("mbtn") && !document.querySelector("nav#menu") && !document.getElementById("miLook") &&
    !Array.from(document.querySelectorAll("header a")).some((x) => /DRIVE/.test(x.textContent))), "/logo: the old menu, LOOK item or DRIVE link is still there");
  await page.click("#menuBtn");
  await page.evaluate(() => { window.__sent = []; });
  await page.keyboard.press("Escape"); await page.waitForTimeout(100);
  ok(await page.evaluate(() => document.getElementById("cqMenu").hidden), "/logo: Esc did not close the menu");
  ok(await page.evaluate(() => window.__sent.join()) === "stop,logo_stop", "/logo: Esc with the menu open did not stop");
  await page.waitForTimeout(300);
  await page.evaluate(() => { window.__sent = []; });
  await page.keyboard.press("Escape"); await page.waitForTimeout(100);
  ok(await page.evaluate(() => window.__sent.join()) === "stop,logo_stop", "/logo: Esc again did not stop");
  // final review: /logo's keys do nothing behind the open menu (Ctrl+Enter would start a program)
  await page.evaluate(() => { const s = document.getElementById("src"); s.value = "FORWARD 10\n"; s.dispatchEvent(new Event("input")); });
  await page.waitForTimeout(1200);
  await page.click("#menuBtn");
  await page.evaluate(() => { window.__logoRun = 0; window.__sockets.forEach((x) => { const s0 = x.send.bind(x);
    x.send = (t) => { if (JSON.parse(t).t === "logo_run") window.__logoRun++; return s0(t); }; }); });
  await page.keyboard.press("Control+Enter"); await page.keyboard.press("F10"); await page.waitForTimeout(200);
  ok(await page.evaluate(() => window.__logoRun) === 0, "/logo: Ctrl+Enter / F10 ran a program behind the open menu");
  await page.keyboard.press("Escape");
  // every page together: the three at the same place and size on the pages with a header (within 1 px); a short laptop
  // window and a phone on its side reach all five menu entries; no sideways scroll
  for (const [w, h] of [[390, 844], [1440, 900], [1280, 600], [844, 390]]) {
    const seen = {};
    for (const path of ["/ai", "/settings", "/logo", "/help"]) {
      await page.setViewportSize({ width: w, height: h });
      await page.goto(BASE + path); await page.waitForTimeout(300);
      seen[path] = (await rects()).map((x) => [x.l, x.t, x.r - x.l, x.h].map(Math.round).join(",")).join(" ");
      await page.click("#menuBtn");
      const reach = await page.evaluate(() => { const m = document.getElementById("cqMenu"), last = m.querySelector("a:last-child");
        m.scrollTop = m.scrollHeight; const r = last.getBoundingClientRect(); return r.bottom <= innerHeight && r.top >= 0; });
      ok(reach, path + " " + w + "x" + h + ": the last menu entry cannot be reached");
      ok(await page.evaluate(() => document.scrollingElement.scrollWidth <= innerWidth), path + " " + w + "x" + h + ": sideways scroll");
    }
    const first = seen["/ai"];
    for (const path in seen) ok(seen[path].split(" ").every((b, i) => b.split(",").every((v, j) => Math.abs(v - first.split(" ")[i].split(",")[j]) <= 1)),
      w + "x" + h + ": the three on " + path + " (" + seen[path] + ") are not where they are on /ai (" + first + ")");
  }
  for (const [w, h] of [[1280, 600], [844, 390]]) {
    await page.setViewportSize({ width: w, height: h });
    await page.goto(BASE + "/"); await page.waitForTimeout(300);
    await page.click("#menuBtn");
    ok(await page.evaluate(() => { const m = document.getElementById("cqMenu"); m.scrollTop = m.scrollHeight;
      const r = m.querySelector("a:last-child").getBoundingClientRect(); return r.bottom <= innerHeight; }), "/ " + w + "x" + h + ": the last menu entry cannot be reached");
  }
  // Settings with its own socket down: STOP still tries, through a socket of its own, and says when it cannot reach the tank
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(BASE + "/settings"); await page.waitForTimeout(300);
  await page.evaluate(() => { window.__down = true; window.__sockets.forEach((x) => x.close()); });
  await page.waitForTimeout(100);
  const before = await page.evaluate(() => window.__sockets.length);
  await page.click("#stop"); await page.waitForTimeout(2300);
  const down = await page.evaluate((b) => ({ toast: document.getElementById("toast").textContent, n: window.__sockets.length,
    stopSock: window.__sockets.slice(b).some((x) => x.closed) }), before);  // the press's socket closed (the page's own retries may be open)
  ok(down.n > before && down.stopSock && down.toast === "Couldn't reach the tank - use another STOP", "/settings: STOP with the socket down gave " + JSON.stringify(down));
  return fails.length ? "FAIL\n" + fails.join("\n") : "PASS bar_check (" + PAGES.map((p) => p.path).join(" ") + ")";
}
