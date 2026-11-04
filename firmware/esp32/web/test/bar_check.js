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
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 10);
      }
      send(s) {
        const m = JSON.parse(s);
        if (["hello", "stop", "logo_stop"].indexOf(m.t) >= 0) window.__sent.push(m.t);  // what STOP sends; not the pages' polls
        if (m.t === "hello") this.emit({ t: "welcome", you: "x-1" });
        if (m.id !== undefined) this.emit({ t: "ack", id: m.id });
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
      const titleLines = await page.evaluate((sel) => { const e = document.querySelector(sel); return e ? Math.round(e.getBoundingClientRect().height / parseFloat(getComputedStyle(e).lineHeight || "20")) : -1; }, p.title);
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
      await page.keyboard.press("Escape");
      ok(await page.evaluate(() => document.getElementById("cqMenu").hidden), at + ": Esc did not close the menu");
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
  return fails.length ? "FAIL\n" + fails.join("\n") : "PASS bar_check (" + PAGES.map((p) => p.path).join(" ") + ")";
}
