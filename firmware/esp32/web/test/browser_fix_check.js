// Plan N4 final-review fixes, browser check (Playwright run_code); before it: PUT /logo/programs/gone on the stand-in tank.
async (page) => {
  await page.context().clearCookies();
  await page.setViewportSize({ width: 390, height: 844 });
  await page.addInitScript(() => {
    if (!sessionStorage.getItem("keep")) { try { localStorage.clear(); } catch (e) { } }
    window.__sent = [];
    class FakeWS {
      constructor(url) { this.url = url; this.readyState = 0; window.__ws = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
      send(s) { const m = JSON.parse(s); window.__sent.push(m);
        if (m.t === "hello") this.emit({ t: "welcome", you: "phone-1", tank: { fw: { uno: "1.0" } } });
        if (m.t === "logo_run" && window.__runReply) this.emit(Object.assign({ id: m.id }, window.__runReply)); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { this.readyState = 3; this.onclose && this.onclose(); }
    }
    window.WebSocket = FakeWS;
    window.__fakeEmit = (o) => window.__ws.emit(o);
  });
  const q = (f, a) => page.evaluate(f, a);
  const out = {};
  await page.goto("http://127.0.0.1:8765/logo"); await page.waitForTimeout(1500);
  // C1: STOP on screen after scrolling to the bottom, and above an open sheet
  await page.click("#pad button:has-text('MORE WORDS')");
  await q(() => window.scrollTo(0, document.body.scrollHeight)); await page.waitForTimeout(100);
  const stopTop = () => q(() => { const r = document.getElementById("stop").getBoundingClientRect();
    const onScreen = r.top >= 0 && r.bottom <= innerHeight;
    const hit = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
    return { onScreen, topmost: !!hit && (hit.id === "stop" || hit.closest("#stop") !== null) }; });
  out.c1Scrolled = await stopTop();
  await page.click("#psave"); await page.waitForTimeout(100);
  out.c1Sheet = await stopTop();
  await page.click("#nameC");
  await q(() => window.scrollTo(0, 0));
  // I5: DELETE on the new program asks first
  await q(() => { const s = document.getElementById("src"); s.value = "FD 10\n"; s.dispatchEvent(new Event("input")); });
  await page.click("#pdel"); await page.waitForTimeout(100);
  out.i5 = { sheet: await q(() => document.getElementById("delS").className), text: await q(() => document.getElementById("src").value) };
  if ((await q(() => document.getElementById("delS").className)).includes("on")) await page.click("#delC");
  await q(() => { const s = document.getElementById("src"); if (!s.value) { s.value = "FD 10\n"; s.dispatchEvent(new Event("input")); } });
  // I4: a program deleted on another device: opening it fails, the editor keeps its own program
  await q(async () => { await fetch("/logo/programs/gone", { method: "DELETE" }); });
  await page.selectOption("#psel", "gone"); await page.waitForTimeout(600);
  out.i4Failed = { title: await q(() => document.getElementById("title").textContent), text: await q(() => document.getElementById("src").value) };
  await page.click("#psave"); await page.waitForTimeout(300);
  out.i4SaveAfterFail = { sheet: await q(() => document.getElementById("nameS").className),
    goneOnTank: await q(async () => (await fetch("/logo/programs/gone")).status) };
  if (out.i4SaveAfterFail.sheet.includes("on")) await page.click("#nameC");
  // I4: the editor is read-only while a program loads
  const busy = q(async () => { const t0 = Date.now(); while (Date.now() - t0 < 1500) { await fetch("/logo/check", { method: "POST", body: "FD 1" }); } });
  await page.waitForTimeout(30);
  await page.selectOption("#psel", "square"); await page.waitForTimeout(80);
  out.i4Loading = await q(() => document.getElementById("src").readOnly);
  await busy; await page.waitForTimeout(800);
  out.i4Loaded = { ro: await q(() => document.getElementById("src").readOnly), text: await q(() => document.getElementById("src").value) };
  // I3: text typed while a SAVE waits behind a busy tank stays as a draft
  await page.waitForTimeout(800);
  const busy2 = q(async () => { const t0 = Date.now(); while (Date.now() - t0 < 1500) { await fetch("/logo/check", { method: "POST", body: "FD 1" }); } });
  await page.waitForTimeout(30);
  await q(() => { const s = document.getElementById("src"); s.value = "REPEAT 4 [ FD 61 RT 90 ]\n"; s.dispatchEvent(new Event("input")); });
  await page.click("#psave"); await page.waitForTimeout(100);
  await q(() => { const s = document.getElementById("src"); s.value = "REPEAT 4 [ FD 62 RT 90 ]\n"; s.dispatchEvent(new Event("input")); });
  await busy2; await page.waitForTimeout(1500);
  out.i3 = { tank: await q(async () => (await fetch("/logo/programs/square")).text()),
    draft: await q(() => localStorage.getItem("cq.logo.draft.square")), title: await q(() => document.getElementById("title").textContent) };
  // I6: key handling with 30 stored 8 KB drafts
  await q(() => { const big = "REPEAT 4 [ FORWARD 50 RIGHT 90 ] ; side\n".repeat(200).slice(0, 8000);
    for (let i = 0; i < 30; i++) localStorage.setItem("cq.logo.draft.p" + i, JSON.stringify({ text: big, saved: "" })); });
  out.i6KeyMs = await q(() => { const s = document.getElementById("src"); s.value = "REPEAT 4 [ FORWARD 50 RIGHT 90 ] ; side\n".repeat(200).slice(0, 8000);
    const t0 = performance.now(); for (let i = 0; i < 10; i++) s.dispatchEvent(new Event("input")); return (performance.now() - t0) / 10; });
  return out;
}
