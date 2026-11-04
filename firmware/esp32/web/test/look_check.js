// Plan Sunny (bridge LLD R28.5) browser check (Playwright run_code) on the stand-in tank web/test/fake_tank.py :8765.
// The cockpit never changes: each page in the cockpit, at phone and laptop size, is compared element by element
// (computed colours, shadows, fonts) with the same page from the baseline commit 5521b22 (before any look change),
// served by a second stand-in on :8766:  git worktree add --detach ../look-base 5521b22 &&
// (cd ../look-base/firmware/esp32 && python3 web/test/fake_tank.py 8766). A mismatch is looked at again once (live
// pages can be caught mid-update). Elements Plan Sunny adds carry data-look-skip. Then the Sunny checks.
async (tab) => {
  const page = await tab.context().newPage();  // a fresh tab: no init scripts left by other checks
  // the tank's socket never opens and never drops: no reconnect cycle changing chips and log lines mid-compare
  await page.addInitScript(() => { window.WebSocket = class { constructor(u) { this.url = u; this.readyState = 0; } send() {} close() {} }; });
  const PAGES = ["/", "/logo", "/settings", "/help", "/ai"];
  const SIZES = [[390, 844], [1440, 900]];
  const q = (f, a) => page.evaluate(f, a);
  async function looks(port, p) {
    await page.goto("http://127.0.0.1:" + port + p);
    await q(() => { localStorage.clear(); localStorage.setItem("cq_theme", "cockpit"); });  // no saved drafts: two origins
    await page.reload(); await page.waitForTimeout(1500);
    return q(() => {
      const P = ["color", "background-color", "border-top-color", "border-right-color", "border-bottom-color",
        "border-left-color", "box-shadow", "text-shadow", "font-family", "outline-color", "fill", "stroke", "opacity"];
      const out = [];
      for (const el of document.querySelectorAll("body *")) {
        if (el.closest("[data-look-skip]")) continue;
        const cs = getComputedStyle(el);
        out.push(el.tagName + "#" + el.id + "|" + P.map((pr) => cs.getPropertyValue(pr)).join("|"));
      }
      return out;
    });
  }
  const firstDiff = (a, b) => { for (let i = 0; i < Math.max(a.length, b.length); i++) if (a[i] !== b[i]) return [i, a[i], b[i]]; return null; };
  const out = { changed: {} };
  for (const [w, h] of SIZES) {
    await page.setViewportSize({ width: w, height: h });
    for (const p of PAGES) {
      let d = firstDiff(await looks(8766, p), await looks(8765, p));
      if (d) d = firstDiff(await looks(8766, p), await looks(8765, p));
      if (d) out.changed[p + "@" + w] = d;
    }
  }
  const cockpitSame = Object.keys(out.changed).length === 0;
  // Sunny: each page opened with cq_theme = "play"
  const open = async (p, w, h) => {
    await page.setViewportSize({ width: w, height: h });
    await page.goto("http://127.0.0.1:8765" + p);
    await q(() => localStorage.setItem("cq_theme", "play")); await page.reload(); await page.waitForTimeout(800);
  };
  const css = (sel, prop) => q(([s, pr]) => getComputedStyle(document.querySelector(s)).getPropertyValue(pr), [sel, prop]);
  const theme = () => q(() => document.documentElement.dataset.theme);
  const pixel = (sel, fx, fy) => q(([s, fx, fy]) => { const c = document.querySelector(s);
    const d = c.getContext("2d").getImageData(Math.round(c.width * fx), Math.round(c.height * fy), 1, 1).data; return [d[0], d[1], d[2], d[3]].join(); }, [sel, fx, fy]);
  const S = (out.sunny = {});
  // Drive (landscape: the page asks to rotate in portrait). Its background is the camera: the glass is checked.
  await open("/", 844, 390);
  S.driveGlass = [await css("#menuBtn", "color"), await css("#menuBtn", "background-color")];  // navy on white glass
  S.driveStop = await css("#stop", "background-color");                                          // rgb(232, 56, 46)
  const caret = () => pixel("#hdgTape", 0.5, 0.92);
  const caretSunny = await caret();
  await page.click("#menuBtn"); await page.click("#miLook"); await page.waitForTimeout(100);
  S.driveFlip = [await theme(), caretSunny !== await caret()];                                   // cockpit, the caret redrawn
  out.pass = cockpitSame &&
    S.driveGlass.join() === "rgb(29, 43, 68),rgba(255, 255, 255, 0.82)" && S.driveStop === "rgb(232, 56, 46)" &&
    S.driveFlip.join() === "cockpit,true";
  await page.close();
  return out;
}
