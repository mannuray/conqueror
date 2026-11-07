// Design-notes §206 browser check (Playwright run_code): the /ai page's two looks on the stand-in tank
// firmware/esp32/web/test/fake_tank.py on :8765. A touch device (pointer: coarse, faked here through
// sessionStorage "coarse") starts Sunny, a mouse starts the cockpit; the top bar's look button flips and remembers (R29).
async (page) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.addInitScript(() => {
    const real = window.matchMedia.bind(window);
    window.matchMedia = (m) => m === "(pointer: coarse)" ? { matches: sessionStorage.getItem("coarse") === "1", media: m } : real(m);
  });
  const q = (f, a) => page.evaluate(f, a);
  const look = () => q(() => ({ theme: document.documentElement.dataset.theme, bg: getComputedStyle(document.body).backgroundColor,
    stop: getComputedStyle(document.getElementById("stop")).backgroundColor }));
  const out = {};
  await page.goto("http://127.0.0.1:8765/ai");
  await q(() => { localStorage.removeItem("cq_theme"); sessionStorage.setItem("coarse", "0"); }); await page.reload();
  out.mouse = await look();                                   // cockpit, rgb(5, 8, 5), STOP rgb(255, 59, 48)
  await q(() => sessionStorage.setItem("coarse", "1")); await page.reload();
  out.touch = await look();                                   // play, rgb(255, 248, 236), STOP rgb(232, 56, 46)
  out.noHScroll = await q(() => document.scrollingElement.scrollWidth <= innerWidth);  // true in Sunny on a phone
  await page.click("#lookBtn");
  out.flipped = await look();                                 // cockpit
  await page.reload();
  out.remembered = await look();                              // cockpit, though a touch device
  await page.click("#lookBtn"); await page.reload();
  out.back = await look();                                    // play
  out.saved = await q(() => localStorage.getItem("cq_theme"));  // play
  out.pass = out.mouse.theme === "cockpit" && out.mouse.bg === "rgb(5, 8, 5)" && out.mouse.stop === "rgb(255, 59, 48)" &&
    out.touch.theme === "play" && out.touch.bg === "rgb(255, 248, 236)" && out.touch.stop === "rgb(232, 56, 46)" &&
    out.noHScroll && out.flipped.theme === "cockpit" && out.remembered.theme === "cockpit" && out.back.theme === "play" &&
    out.saved === "play";
  return out;
}
