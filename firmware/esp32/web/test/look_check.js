// Plan Sunny (bridge LLD R28.5) browser check (Playwright run_code) on the stand-in tank web/test/fake_tank.py :8765.
// The cockpit never changes: each page in the cockpit, at phone and laptop size, is compared element by element
// (computed colours, shadows, fonts, background images, corner radii, ::before / ::after) with the same page from the baseline commit 5521b22 (before any look change),
// served by a second stand-in on :8766:  git worktree add --detach ../look-base 5521b22 &&
// (cd ../look-base/firmware/esp32 && python3 web/test/fake_tank.py 8766). A mismatch is looked at again once (live
// pages can be caught mid-update). Elements Plan Sunny adds carry data-look-skip; the top bar (Plan Top Bar, R29) is
// left out on both sides. Then the Sunny checks.
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
        "border-left-color", "box-shadow", "text-shadow", "font-family", "outline-color", "fill", "stroke", "opacity",
        "background-image", "border-top-left-radius", "border-bottom-right-radius", "display"];
      const out = [];
      for (const el of document.querySelectorAll("body *")) {
        // Plan Top Bar (R29): the top bar changed on purpose - each page's header and the bar's menu are left out on both sides
        if (el.closest("[data-look-skip], header, #cqMenu, #cqDim")) continue;
        for (const pseudo of [null, "::before", "::after"]) {
          const cs = getComputedStyle(el, pseudo);
          if (pseudo && cs.content === "none") continue;
          out.push(el.tagName + "#" + el.id + (pseudo || "") + "|" + P.map((pr) => cs.getPropertyValue(pr)).join("|"));
        }
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
  S.driveTitle = await css(".stick .ttl", "text-shadow");  // final review I3: a white halo, readable on the video
  const caret = () => pixel("#hdgTape", 0.5, 0.92);
  const caretSunny = await caret();
  await page.click("#menuBtn"); await page.click("#miLook"); await page.waitForTimeout(100);
  S.driveFlip = [await theme(), caretSunny !== await caret()];                                   // cockpit, the caret redrawn
  // /logo: the page, STOP, the code font stays fixed-width, LOOK in the ☰ menu flips, the path is redrawn
  await open("/logo", 1440, 900);
  S.logo = [await css("body", "background-color"), await css("#stop", "background-color"), await css("#src", "font-family")];
  S.logoRunInk = await css("#run", "color");  // final review I2: navy on orange (white was 2.5:1)
  await q(() => { const s = document.getElementById("src"); s.value = "FORWAD 50\n"; s.dispatchEvent(new Event("input")); });
  await page.waitForTimeout(1200);
  S.logoDiag = await css(".diag pre", "font-family");  // final review I1: fixed-width, so the ▲ points at the right letter
  await q(() => { const s = document.getElementById("src"); s.value = "FORWARD 50\n"; s.dispatchEvent(new Event("input")); });
  await page.waitForTimeout(1200);
  const pathPx = () => q(() => { const c = document.getElementById("path"), g = c.getContext("2d"), d = g.getImageData(0, 0, c.width, c.height).data;
    let h = 0; for (let i = 0; i < d.length; i += 97) h = (h * 31 + d[i]) >>> 0; return h; });
  const pathSunny = await pathPx();
  await page.click("#mbtn"); await page.click("#miLook"); await page.waitForTimeout(100);
  S.logoFlip = [await theme(), pathSunny !== await pathPx()];                                    // cockpit, redrawn
  // /settings: the page, the header button flips, the Tune graph is redrawn in the new colours
  await open("/settings", 390, 844);
  S.settings = [await css("body", "background-color"), await css("#gCan", "background-color")];
  await page.click("button[data-tab=tune]"); await page.waitForTimeout(300);
  const gridPx = () => q(() => { const c = document.getElementById("gCan"), d = c.getContext("2d").getImageData(0, 0, c.width, c.height).data;
    let h = 0; for (let i = 0; i < d.length; i += 53) h = (h * 31 + d[i]) >>> 0; return h; });
  const gridSunny = await gridPx();
  await page.click("#lookBtn"); await page.waitForTimeout(150);
  S.settingsFlip = [await theme(), gridSunny !== await gridPx()];                                  // cockpit, redrawn
  S.settingsTitle = await q(() => { const r = (s) => document.querySelector(s).getBoundingClientRect();
    return Math.abs(r(".top h1").top + r(".top h1").height / 2 - r("#lookBtn").top - 22) < 4 && r("#lookBtn").left > r(".top h1").right; });  // one row: TITLE … [look][menu][STOP] (R29)
  // /help: follows the choice, no switch of its own, code stays fixed-width, no title glow
  await open("/help", 390, 844);
  S.help = [await css("body", "background-color"), await q(() => !document.getElementById("themeBtn") && !document.getElementById("miLook")),
    await css("pre", "font-family"), await css("h1", "text-shadow")];
  // every page together: a choice made on one page holds on all; a junk saved value gives the default (a mouse here:
  // the cockpit); no page scrolls sideways in Sunny (the drive page in landscape)
  const look = async (p) => { await page.goto("http://127.0.0.1:8765" + p); await page.waitForTimeout(300); return theme(); };
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("http://127.0.0.1:8765/logo"); await q(() => localStorage.setItem("cq_theme", "cockpit")); await page.reload();
  await page.click("#mbtn"); await page.click("#miLook");
  S.allPlay = []; for (const p of PAGES) S.allPlay.push(await look(p));                           // play ×5
  await page.goto("http://127.0.0.1:8765/settings"); await page.click("#lookBtn");
  S.allCockpit = []; for (const p of PAGES) S.allCockpit.push(await look(p));                     // cockpit ×5
  await q(() => localStorage.setItem("cq_theme", "junk"));
  S.junk = await look("/help");                                                                   // cockpit
  await q(() => localStorage.setItem("cq_theme", "play"));
  S.noSideways = [];
  for (const p of PAGES) {
    await page.setViewportSize(p === "/" ? { width: 844, height: 390 } : { width: 390, height: 844 });
    await page.goto("http://127.0.0.1:8765" + p); await page.waitForTimeout(300);
    S.noSideways.push(await q(() => document.scrollingElement.scrollWidth <= innerWidth));        // true ×5
  }
  // private browsing: storage throws - the default look, and the button still flips it for the visit
  const priv = await tab.context().newPage();
  await priv.addInitScript(() => { Object.defineProperty(window, "localStorage", { get() { throw new Error("private"); } }); });
  await priv.goto("http://127.0.0.1:8765/settings"); await priv.waitForTimeout(300);
  const t0 = await priv.evaluate(() => document.documentElement.dataset.theme);
  await priv.click("#lookBtn");
  S.private = [t0, await priv.evaluate(() => document.documentElement.dataset.theme)];            // cockpit, play
  await priv.close();
  // final review minors: one Sunny HUD value (M1); Sunny's shape and no cockpit effects (M2); the browser bar follows
  // the look (M5); darker code colours on cream (M4); digits that do not jitter (M7)
  const M = (S.minors = {});
  await open("/ai", 390, 844);
  M.hud = await q(() => cqTheme.colour("--hud-bg"));                                               // rgba(255,255,255,.88)
  await open("/settings", 390, 844);
  M.settings = [await q(() => getComputedStyle(document.querySelector(".card"), "::before").display),  // none
    await css("body", "background-image"), await css(".card", "border-top-left-radius"),            // none, 18px
    await css("input", "border-top-left-radius"), await q(() => document.querySelector("meta[name=theme-color]").content)];  // 12px, #fff8ec
  await open("/logo", 1440, 900);
  M.logo = [await css("body", "background-image"), await css(".card", "border-top-left-radius"), await css("#run", "border-top-left-radius"),
    await q(() => ["--cmt", "--turn", "--flow", "--num", "--move"].map((n) => cqTheme.colour(n)).join())];
  await open("/", 844, 390);
  M.drive = [await css("#menuBtn", "border-top-left-radius"), await css(".pad", "box-shadow"), await css("#hdgVal", "font-variant-numeric"),
    await q(() => document.querySelector("meta[name=theme-color]").content)];
  await page.click("#menuBtn"); await page.click("#miLook"); await page.waitForTimeout(100);
  M.driveCockpitMeta = await q(() => document.querySelector("meta[name=theme-color]").content);  // #050805
  const minorsOk = M.hud === "rgba(255,255,255,.88)" &&
    M.settings.join("|") === "none|none|18px|12px|#fff8ec" &&
    M.logo.join("|") === "none|18px|12px|#667183,#5c7a00,#087d86,#aa5d00,#178236" &&
    M.drive[0] === "12px" && !M.drive[1].includes("31, 111, 209") && M.drive[2] === "tabular-nums" && M.drive[3] === "#fff8ec" &&
    M.driveCockpitMeta === "#050805";
  const every = (a, v) => a.length === PAGES.length && a.every((x) => x === v);
  out.pass = minorsOk && S.driveTitle.includes("rgb(255, 255, 255)") && S.logoRunInk === "rgb(29, 43, 68)" &&
    S.logoDiag.startsWith("ui-monospace") &&
    every(S.allPlay, "play") && every(S.allCockpit, "cockpit") && S.junk === "cockpit" && every(S.noSideways, true) &&
    S.private.join() === "cockpit,play" &&
    cockpitSame && S.help[0] === "rgb(255, 248, 236)" && S.help[1] && S.help[2].startsWith("ui-monospace") && S.help[3] === "none" && S.settings.join() === "rgb(255, 248, 236),rgb(255, 255, 255)" &&
    S.settingsFlip.join() === "cockpit,true" && S.settingsTitle &&
    S.logo.join("|").startsWith("rgb(255, 248, 236)|rgb(232, 56, 46)|ui-monospace") && S.logoFlip.join() === "cockpit,true" &&
    S.driveGlass.join() === "rgb(29, 43, 68),rgba(255, 255, 255, 0.82)" && S.driveStop === "rgb(232, 56, 46)" &&
    S.driveFlip.join() === "cockpit,true";
  await page.close();
  return out;
}
