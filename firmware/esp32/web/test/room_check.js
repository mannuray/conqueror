// Plan Z2b-2 Task 8 browser check (Playwright run_code): the /ai page's ROOM tab on the stand-in tank
// firmware/esp32/web/test/fake_tank.py on :8765. Both WebSockets are faked as in ai_check.js; the engine's feed also
// sends the ROOM tab's map frame (a PNG of cell kinds made with a canvas here) and its room message (AI engine LLD §23.2).
async (page) => {
  const checked = [];
  await page.route("**/logo/check", async (r) => { checked.push(r.request().postData());
    await r.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, prog: 7, diags: [] }) }); });
  await page.route("**/stream*", (r) => r.abort());
  await page.addInitScript(() => {
    window.__tank = []; window.__feed = []; window.__feeds = [];
    // a 3 x 2 m crop: floor, a high block at x 1.5..1.9, y -0.2..0.2 (kinds 2 and 4)
    window.__mapFrame = async () => {
      const w = 60, h = 40, c = new OffscreenCanvas(h, w), x = c.getContext("2d");
      const img = x.createImageData(h, w);
      for (let i = 0; i < w; ++i) for (let j = 0; j < h; ++j) {
        const cx = -0.5 + (i + 0.5) * 0.05, cy = -1.0 + (j + 0.5) * 0.05;
        const k = cx > 1.5 && cx < 1.9 && cy > -0.2 && cy < 0.2 ? 4 : 2, o = (i * h + j) * 4;
        img.data[o] = img.data[o + 1] = img.data[o + 2] = k; img.data[o + 3] = 255;
      }
      x.putImageData(img, 0, 0);
      const png = new Uint8Array(await (await c.convertToBlob({ type: "image/png" })).arrayBuffer());
      const head = new TextEncoder().encode(JSON.stringify({ kind: "map", seq: 1, cell_m: 0.05, x0: -0.5, y0: -1.0, w: w, h: h, png: png.length }));
      const b = new Uint8Array(4 + head.length + png.length);
      new DataView(b.buffer).setUint32(0, head.length); b.set(head, 4); b.set(png, 4 + head.length);
      return b.buffer;
    };
    window.__room = (go) => ({ t: "room", trail: [[0, 0], [0.3, 0.1]], home: "room2", area: { x: 0, y: 0, r: 3 },
      things: [{ word: "ball", x: 1.0, y: 0.6, ago_s: 120, where: "near the bed" }],
      names: [{ id: 1, name: "bed", x: 1.7, y: 0.0, go: go === undefined ? [1.2, 0.0] : go }],
      why: { route: [], pending: [], check: null, looked: [] }, seen_m2: 6.0 });
    class FakeWS {
      constructor(url) {
        this.url = url; this.readyState = 0; this.feed = url.indexOf(":8770/feed") >= 0; this.sent = [];
        if (this.feed) { window.__feedWs = this; window.__feeds.push(this); } else window.__tankWs = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); if (this.feed) this.greet(); }, 20);
      }
      greet() {
        this.emit({ t: "hello", proto: 1 });
        this.emit({ t: "things", list: [{ word: "ball", names: [], measured: true }] });
        this.emit({ t: "status", state: "ready", mood: "happy", say: "Ready to play!", health: "ok", learning: false, pose: { x: 0, y: 0, h: 0 }, pan: 0 });
      }
      async sendMap() { const f = await window.__mapFrame(); this.onmessage && this.onmessage({ data: f }); }
      send(s) { const m = JSON.parse(s); this.sent.push(m); (this.feed ? window.__feed : window.__tank).push(m);
        if (!this.feed && m.t === "hello") { this.emit({ t: "welcome", you: "ai-1" }); this.emit({ t: "ai_at", url: "ws://127.0.0.1:8770/feed", proto: 1 }); }
        if (!this.feed && m.t === "ping") this.emit({ t: "pong" }); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { if (this.readyState === 3) return; this.readyState = 3; setTimeout(() => this.onclose && this.onclose(), 1); }
    }
    window.WebSocket = FakeWS;
  });
  const q = (f, a) => page.evaluate(f, a);
  const out = {};
  const screen = (x, y) => q(([x, y]) => { const P = cqRoom.page, c = document.getElementById("roomMap").getBoundingClientRect();
    const p = cqRoom.RoomView.toScreen(P.transform(), x, y);  // the transform is in CSS pixels
    return [c.left + p[0], c.top + p[1]]; }, [x, y]);
  const pixel = (x, y) => q(([x, y]) => { const c = document.getElementById("roomMap"), p = cqRoom.RoomView.toScreen(cqRoom.page.transform(), x, y);
    const k = c.width / c.getBoundingClientRect().width;
    const d = c.getContext("2d").getImageData(Math.round(p[0] * k), Math.round(p[1] * k), 1, 1).data; return [d[0], d[1], d[2]]; }, [x, y]);
  const tap = async (x, y) => { const p = await screen(x, y); await page.mouse.click(p[0], p[1]); await page.waitForTimeout(150); };
  const endRun = () => q(() => window.__tankWs.emit({ t: "ev", kind: "logo_ended", reason: "done", line: 1, msg: "Done." }));

  for (const size of [{ width: 390, height: 844 }, { width: 1440, height: 900 }]) {
    await page.setViewportSize(size);
    await page.goto("http://127.0.0.1:8765/ai"); await page.waitForTimeout(800);
    const tag = size.width < 900 ? "phone" : "laptop";
    out[tag + "Tabs"] = await q(() => Array.prototype.map.call(document.querySelectorAll("header nav button"), (b) => b.textContent.trim()).join());
    // Review Focus 1: the map and room arrive while PLAY shows - kept, drawn when ROOM opens
    await q(() => window.__feedWs.sendMap()); await q(() => window.__feedWs.emit(window.__room()));
    await page.waitForTimeout(200);
    await page.click('header nav button[data-tab="room"]'); await page.waitForTimeout(300);
    out[tag + "RoomOn"] = await q(() => window.__feedWs.sent.some((m) => m.t === "room" && m.on === true));
    const bg = await pixel(-0.4, 0.9), block = await pixel(1.7, 0.15);
    out[tag + "Drawn"] = JSON.stringify(bg) !== JSON.stringify(block);
    const r = await q(() => { const m = document.getElementById("roomMapCard").getBoundingClientRect(), p = document.getElementById("roomPanel").getBoundingClientRect();
      return { mapLeft: m.left, mapTop: m.top, mapBottom: m.bottom, panelLeft: p.left, panelTop: p.top, scroll: document.documentElement.scrollWidth <= innerWidth }; });
    out[tag + "Layout"] = (tag === "laptop" ? r.panelLeft > r.mapLeft + 100 && Math.abs(r.panelTop - r.mapTop) < 40 : r.panelTop >= r.mapBottom - 1) && r.scroll;
    out[tag + "Facts"] = await q(() => document.getElementById("roomFacts").textContent);
    out[tag + "List"] = await q(() => document.getElementById("roomList").textContent.replace(/\s+/g, " ").trim());
  }
  // the pin's card, FIND: the Play tab's run path, nothing else
  await tap(1.0, 0.6);
  out.pinCard = await q(() => document.getElementById("roomCard").textContent.replace(/\s+/g, " ").trim());
  const sentBefore = await q(() => window.__tank.length);
  await page.click("#roomCard .rcFind"); await page.waitForTimeout(300);
  out.findLine = checked[checked.length - 1];
  out.findOnly = await q((n) => window.__tank.slice(n).map((m) => m.t).join(), sentBefore);  // logo_run
  await endRun(); await page.waitForTimeout(100);
  // GO THERE from the newest pose (Review Focus 2): the card opens, the tank moves, then the press
  await tap(1.7, 0.0);
  out.nameCard = await q(() => document.getElementById("roomCard").textContent.replace(/\s+/g, " ").trim());
  await q(() => window.__feedWs.emit({ t: "status", state: "ready", mood: "happy", say: "Ready to play!", health: "ok", learning: false, pose: { x: 0.5, y: 0, h: 0 }, pan: 0 }));
  await page.waitForTimeout(100);
  await page.click("#roomCard .rcGo"); await page.waitForTimeout(300);
  out.goLine = checked[checked.length - 1];
  await endRun(); await page.waitForTimeout(100);
  await q(() => window.__feedWs.emit(window.__room(null))); await page.waitForTimeout(700);
  await tap(1.7, 0.0);
  out.goNull = await q(() => { const b = document.querySelector("#roomCard .rcGo"); return [b.disabled, document.getElementById("roomCard").textContent.indexOf("I don't know a way there yet.") >= 0]; });
  // a block: name it; the engine's refusal shown
  await q(() => document.querySelector("#roomCard .rcClose") && document.querySelector("#roomCard .rcClose").click());
  await page.waitForTimeout(150);
  const unlit = await pixel(1.6, -0.1);
  await tap(1.85, -0.15);
  out.lit = JSON.stringify(await pixel(1.6, -0.1)) !== JSON.stringify(unlit);  // R32.7: the whole joined block lights
  out.hint = await q(() => document.getElementById("roomHint").textContent);
  out.newCard = await q(() => document.getElementById("roomCard").textContent.indexOf("What's this?") >= 0);
  await page.fill("#roomCard input", "Toy Box"); await page.click("#roomCard .rcName"); await page.waitForTimeout(100);
  const nm = await q(() => window.__feed.filter((m) => m.t === "name").pop());
  out.nameSent = nm && nm.name === "Toy Box" && Math.abs(nm.x - 1.85) < 0.05 && Math.abs(nm.y + 0.15) < 0.05 &&
    Math.abs(Math.round((nm.x + 0.5) / 0.025)) % 2 === 1 && Math.abs(Math.round((nm.y + 1.0) / 0.025)) % 2 === 1;
  await q((id) => window.__feedWs.emit({ t: "rep", id: id, ok: false, msg: "Tap a red or amber block to name it." }), nm.id);
  await page.waitForTimeout(100);
  out.refusal = await q(() => document.getElementById("roomCard").textContent.indexOf("Tap a red or amber block to name it.") >= 0);
  out.teachUntouched = await q(() => document.getElementById("teachMsg").textContent === "");
  await page.click("#roomCard .rcClose"); await page.waitForTimeout(150);
  out.unlitAgain = JSON.stringify(await pixel(1.6, -0.1)) === JSON.stringify(unlit);
  await tap(1.85, -0.15);
  // the look flips: the map redraws at once (Review Focus 5)
  const before = await pixel(1.7, 0.15);
  await page.click("#lookBtn"); await page.waitForTimeout(150);
  out.flip = JSON.stringify(await pixel(1.7, 0.15)) !== JSON.stringify(before);
  await page.click("#lookBtn"); await page.waitForTimeout(100);
  // the feed lost and back: its words, then room on again (Review Focus 3)
  await q(() => window.__feedWs.close()); await page.waitForTimeout(100);
  out.lost = await q(() => document.getElementById("roomNote").textContent);
  await page.waitForTimeout(2400);
  out.roomOnAgain = await q(() => window.__feeds.length >= 2 && window.__feedWs.sent.some((m) => m.t === "room" && m.on === true));
  await q(() => window.__tankWs.emit({ t: "ai_at", url: null })); await page.waitForTimeout(200);
  out.noAi = await q(() => document.getElementById("roomNote").textContent);
  // layers remembered per browser
  await page.uncheck("#layTrail"); await page.reload(); await page.waitForTimeout(600);
  await page.click('header nav button[data-tab="room"]'); await page.waitForTimeout(100);
  out.layerKept = !(await page.isChecked("#layTrail"));
  await page.check("#layTrail");
  await page.click('header nav button[data-tab="play"]'); await page.waitForTimeout(100);
  out.roomOff = await q(() => (window.__feedWs ? window.__feedWs.sent : []).some((m) => m.t === "room" && m.on === false) || !window.__feedWs);
  out.pass = out.phoneTabs === "[ PLAY ],[ ROOM ]" && out.laptopTabs === out.phoneTabs && out.phoneRoomOn && out.laptopRoomOn &&
    out.phoneDrawn && out.laptopDrawn && out.phoneLayout && out.laptopLayout &&
    out.laptopFacts === "home room2 · play area 3 m · floor seen 6.0 m²" && /ball near the bed · 2 min ago/.test(out.laptopList) &&
    /ball/.test(out.pinCard) && /FIND "ball$/.test(out.findLine) && out.findOnly === "logo_run" &&
    /bed/.test(out.nameCard) && /SETXY 0 70$/.test(out.goLine) && out.goNull[0] && out.goNull[1] && out.newCard && out.nameSent &&
    out.refusal && out.teachUntouched && out.lit && out.unlitAgain && out.hint === "Tap a red or amber block to name it." && out.flip && out.lost === "Lost the AI helper - reconnecting…" && out.roomOnAgain &&
    out.noAi === "Start the AI helper on the laptop to see the room." && out.layerKept && out.roomOff;
  return out;
}
