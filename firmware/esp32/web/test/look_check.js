// Plan Sunny (bridge LLD R28.5) browser check (Playwright run_code) on the stand-in tank web/test/fake_tank.py :8765.
// "baseline": each page in the cockpit at phone and laptop size -> a hash of every element's computed look.
// "check": the cockpit hashes still equal BASELINE (the cockpit never changes), then the Sunny checks.
async (tab) => {
  const page = await tab.context().newPage();  // a fresh tab: no init scripts left by other checks
  const MODE = "check";  // "baseline" to take a new one (only before a look change)
  // look_baseline.json, taken on plan-z2b ca17997 before Plan Sunny; switch buttons added later carry data-look-skip
  const BASELINE = {"/@390":{"hash":"c8a1249c","n":241},"/logo@390":{"hash":"864ae75","n":211},"/settings@390":{"hash":"c8894188","n":504},"/help@390":{"hash":"79c01293","n":600},"/ai@390":{"hash":"7f459a77","n":207},"/@1440":{"hash":"c8a1249c","n":241},"/logo@1440":{"hash":"3fced05c","n":205},"/settings@1440":{"hash":"c2fe9e62","n":504},"/help@1440":{"hash":"79c01293","n":600},"/ai@1440":{"hash":"9b6295ba","n":207}};
  const PAGES = ["/", "/logo", "/settings", "/help", "/ai"];
  const SIZES = [[390, 844], [1440, 900]];
  const q = (f, a) => page.evaluate(f, a);
  async function lookHash() {
    return q(() => {
      const P = ["color", "background-color", "border-top-color", "border-right-color", "border-bottom-color",
        "border-left-color", "box-shadow", "text-shadow", "font-family", "outline-color", "fill", "stroke", "opacity"];
      let h = 0x811c9dc5, n = 0;
      const add = (s) => { for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; } };
      for (const el of document.querySelectorAll("body *")) {
        if (el.closest("[data-look-skip]")) continue;
        const cs = getComputedStyle(el); n++;
        add(el.tagName + "#" + el.id + "|" + P.map((p) => cs.getPropertyValue(p)).join("|") + ";");
      }
      return { hash: h.toString(16), n };
    });
  }
  const out = { cockpit: {} };
  for (const [w, h] of SIZES) {
    await page.setViewportSize({ width: w, height: h });
    for (const p of PAGES) {
      await page.goto("http://127.0.0.1:8765" + p);
      await q(() => { try { localStorage.setItem("cq_theme", "cockpit"); } catch (e) { /* none */ } });
      await page.reload(); await page.waitForTimeout(1500);
      out.cockpit[p + "@" + w] = await lookHash();
    }
  }
  if (MODE === "baseline") { await page.close(); return out.cockpit; }
  out.changed = Object.keys(BASELINE).filter((k) => JSON.stringify(BASELINE[k]) !== JSON.stringify(out.cockpit[k]));
  out.pass = out.changed.length === 0;
  await page.close();
  return out;
}
