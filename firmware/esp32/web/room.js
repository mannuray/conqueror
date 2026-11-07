/* The /ai page's ROOM tab - its model, words and view (bridge LLD R32, AI engine LLD §23): plain functions, no
   DOM, so Node can test them (node --test firmware/esp32/web/test/room.test.js). pack_web.py puts this file into
   /ai at its ROOM_JS marker, after ai.js (it uses AiCore). */
var cqRoomLib = (function () {
  "use strict";
  var Core = typeof AiCore !== "undefined" ? AiCore : require("./ai.js");
  var LAYERS = ["things", "trail", "names", "why"];
  var BAD_NAME = "Names are letters, spaces and - (up to 24).";
  // SETXY's inputs (Tank Logo LLD §24)
  var MAX_CM = 1000;

  // ---------- the model: what the engine sent, raw ----------
  function RoomModel() {
    this.state = {
      cells: null, crop: null, trail: [], home: null, area: null, things: [], names: [],
      why: { route: [], pending: [], check: null, looked: [] }, seenM2: 0, pose: null, pan: 0,
      layers: { things: true, trail: true, names: true, why: false }, feed: "none", pick: null
    };
    this.fns = [];
  }
  RoomModel.prototype.on = function (fn) { this.fns.push(fn); };
  RoomModel.prototype.changed = function (what) { for (var i = 0; i < this.fns.length; ++i) this.fns[i](what); };
  RoomModel.prototype.applyMap = function (h, cells) {
    this.state.crop = { x0: h.x0, y0: h.y0, w: h.w, h: h.h, cell: h.cell_m };
    this.state.cells = cells;
    this.changed("map");
  };
  RoomModel.prototype.applyRoom = function (m) {
    var s = this.state;
    s.trail = m.trail || []; s.home = m.home || null; s.area = m.area || null; s.seenM2 = m.seen_m2 || 0;
    s.things = (m.things || []).filter(function (t) { return !Core.isPerson(t.word); });
    s.names = (m.names || []).filter(function (n) { return !Core.isPerson(n.name); });
    s.why = m.why || { route: [], pending: [], check: null, looked: [] };
    this.changed("room");
  };
  RoomModel.prototype.applyStatus = function (m) {
    this.state.pose = m.pose || null;
    if (typeof m.pan === "number") this.state.pan = m.pan;
    this.changed("status");
  };
  // R32.7
  RoomModel.prototype.setPick = function (p) { this.state.pick = p; this.changed("pick"); };
  RoomModel.prototype.feedOpen = function () { this.state.feed = "open"; this.changed("feed"); };
  RoomModel.prototype.feedDown = function () { this.state.feed = "lost"; this.changed("feed"); };
  RoomModel.prototype.setLayer = function (key, on) {
    if (LAYERS.indexOf(key) < 0) return;
    this.state.layers[key] = !!on; this.changed("layers");
  };
  RoomModel.prototype.layersJson = function () { return JSON.stringify(this.state.layers); };
  RoomModel.prototype.loadLayers = function (text) {
    var got;
    try { got = JSON.parse(text); } catch (e) { return; }
    if (!got || typeof got !== "object") return;
    for (var i = 0; i < LAYERS.length; ++i) if (typeof got[LAYERS[i]] === "boolean") this.state.layers[LAYERS[i]] = got[LAYERS[i]];
  };

  // A binary message from the feed: a map frame (header kind "map", then the PNG) or null for a camera frame.
  function splitMap(buf) {
    if (buf.byteLength < 4) return null;
    var n = new DataView(buf).getUint32(0), text, head;
    try { text = new TextDecoder().decode(new Uint8Array(buf, 4, n)); } catch (e) { return null; }
    // a camera frame: parsed once, by AiCore.splitFrame
    if (text.indexOf('"kind":"map"') < 0) return null;
    try { head = JSON.parse(text); } catch (e) { return null; }
    if (!head || head.kind !== "map") return null;
    return { header: head, png: new Uint8Array(buf, 4 + n, head.png) };
  }

  // ---------- the words ----------
  var RoomWords = {
    ago: function (s) {
      if (s < 60) return "just now";
      if (s < 3600) return Math.floor(s / 60) + " min ago";
      if (s < 86400) return "today";
      return "earlier";
    },
    facts: function (s) {
      return (s.home ? "home " + s.home : "no home") + " · " + (s.area ? "play area " + s.area.r + " m" : "anywhere") +
        " · floor seen " + (Math.round((s.seenM2 || 0) * 10) / 10).toFixed(1) + " m²";
    },
    // The engine's rules (AI engine LLD §23.3), checked before asking: null = fine, else the reason.
    nameCheck: function (name) {
      var clean = Core.squeeze(name);
      if (!clean || new TextEncoder().encode(clean).length > 24 || !/^[a-z][a-z -]*$/.test(clean)) return BAD_NAME;
      if (Core.isPerson(clean)) return "I don't name people.";
      return null;
    },
    // GO THERE (design-notes §211): SETXY's frame is where the program starts - x right, y ahead, in cm - so the
    // room spot is turned by the pose now (h counter-clockwise from +x). null: no spot, no pose, or too far.
    setxyLine: function (go, pose) {
      if (!go || !pose) return null;
      var dx = go[0] - pose.x, dy = go[1] - pose.y, h = pose.h * Math.PI / 180;
      var ahead = Math.round((dx * Math.cos(h) + dy * Math.sin(h)) * 100);
      var right = Math.round((dx * Math.sin(h) - dy * Math.cos(h)) * 100);
      if (Math.abs(ahead) > MAX_CM || Math.abs(right) > MAX_CM) return null;
      return "SETXY " + (right || 0) + " " + (ahead || 0);
    }
  };

  // ---------- the view: the map on a canvas, up = +x, right = -y ----------
  var MIN_M = 2, PAD = 0.12, PIN_PX = 22, TAP_M = 0.15, TAP_PX = 16, MAX_BLOCK = 4000, CONE_DEG = 82, CONE_M = 1.5;
  function bounds(s) {
    var b = { x0: Infinity, x1: -Infinity, y0: Infinity, y1: -Infinity };
    function add(x, y) { b.x0 = Math.min(b.x0, x); b.x1 = Math.max(b.x1, x); b.y0 = Math.min(b.y0, y); b.y1 = Math.max(b.y1, y); }
    var c = s.crop;
    if (c && c.w > 0) { add(c.x0, c.y0); add(c.x0 + c.w * c.cell, c.y0 + c.h * c.cell); }
    for (var i = 0; i < s.trail.length; ++i) add(s.trail[i][0], s.trail[i][1]);
    if (s.pose) add(s.pose.x, s.pose.y);
    if (!isFinite(b.x0)) add(0, 0);
    return b;
  }
  function centreOf(c, k) { return [c.x0 + (Math.floor(k / c.h) + 0.5) * c.cell, c.y0 + (k % c.h + 0.5) * c.cell]; }
  // the low / high cell nearest (x, y) within reach of its centre + half a cell, else -1
  function nearest(s, x, y, reach) {
    var c = s.crop, best = -1, bd = Infinity;
    if (!c || !s.cells) return -1;
    var r = Math.ceil(reach / c.cell) + 1, ci = Math.floor((x - c.x0) / c.cell), cj = Math.floor((y - c.y0) / c.cell);
    for (var a = ci - r; a <= ci + r; ++a) for (var e = cj - r; e <= cj + r; ++e) {
      if (a < 0 || e < 0 || a >= c.w || e >= c.h || s.cells[a * c.h + e] < 3) continue;
      var p = centreOf(c, a * c.h + e), d = Math.hypot(p[0] - x, p[1] - y);
      if (d <= reach + c.cell / 2 && d < bd) { bd = d; best = a * c.h + e; }
    }
    return best;
  }
  var RoomView = {
    fit: function (s, w, h) {
      var b = bounds(s);
      var xs = Math.max(MIN_M, (b.x1 - b.x0) * (1 + 2 * PAD)), ys = Math.max(MIN_M, (b.y1 - b.y0) * (1 + 2 * PAD));
      var scale = Math.min(w / ys, h / xs);
      return { scale: scale, ox: w / 2 + (b.y0 + b.y1) / 2 * scale, oy: h / 2 + (b.x0 + b.x1) / 2 * scale };
    },
    toScreen: function (t, x, y) { return [t.ox - y * t.scale, t.oy - x * t.scale]; },
    toRoom: function (t, px, py) { return [(t.oy - py) / t.scale, (t.ox - px) / t.scale]; },
    // a name's label: centred on its point
    labelBox: function (t, n) {
      var p = RoomView.toScreen(t, n.x, n.y), w = 7 * n.name.length + 12;
      return { x: p[0] - w / 2, y: p[1] - 11, w: w, h: 22 };
    },
    hit: function (s, t, px, py) {
      var i;
      if (s.layers.things) for (i = 0; i < s.things.length; ++i) {
        var p = RoomView.toScreen(t, s.things[i].x, s.things[i].y);
        if (Math.hypot(p[0] - px, p[1] - py) <= PIN_PX) return { kind: "pin", ref: s.things[i] };
      }
      if (s.layers.names) for (i = 0; i < s.names.length; ++i) {
        var b = RoomView.labelBox(t, s.names[i]);
        if (px >= b.x && px <= b.x + b.w && py >= b.y && py <= b.y + b.h) return { kind: "name", ref: s.names[i] };
      }
      var q = RoomView.toRoom(t, px, py), k = nearest(s, q[0], q[1], Math.max(TAP_M, TAP_PX / t.scale));
      if (k < 0) return { kind: "none", ref: null };
      // the cell's centre: within the engine's 15 cm whatever the reach (R32.7)
      var m = centreOf(s.crop, k);
      return { kind: "block", ref: { x: m[0], y: m[1] } };
    },
    // the block the engine will name (Places.name_at): joined by 8 neighbours, its middle on the block
    blockAt: function (s, x, y, reach) {
      var c = s.crop, k = nearest(s, x, y, reach);
      if (k < 0) return null;
      var seen = {}, todo = [k], cells = [k], sx = 0, sy = 0, i;
      seen[k] = 1;
      while (todo.length && cells.length < MAX_BLOCK) {
        var u = todo.pop(), a = Math.floor(u / c.h), e = u % c.h;
        for (var di = -1; di <= 1; ++di) for (var dj = -1; dj <= 1; ++dj) {
          var na = a + di, ne = e + dj, v = na * c.h + ne;
          if (na < 0 || ne < 0 || na >= c.w || ne >= c.h || seen[v] || s.cells[v] < 3 || cells.length >= MAX_BLOCK) continue;
          seen[v] = 1; todo.push(v); cells.push(v);
        }
      }
      for (i = 0; i < cells.length; ++i) { var p = centreOf(c, cells[i]); sx += p[0]; sy += p[1]; }
      var mid = [sx / cells.length, sy / cells.length], mk = Math.floor((mid[0] - c.x0) / c.cell) * c.h + Math.floor((mid[1] - c.y0) / c.cell);
      // off the block (an L): its cell nearest the mean
      if (!seen[mk]) {
        var best = Infinity;
        for (i = 0; i < cells.length; ++i) { var r = centreOf(c, cells[i]), d = Math.hypot(r[0] - sx / cells.length, r[1] - sy / cells.length); if (d < best) { best = d; mid = r; } }
      }
      return { cells: cells, mid: mid };
    },
    draw: function (ctx, s, t, col) {
      var i, p, c = s.crop, kinds = [null, col.before, col.floor, col.low, col.high];
      ctx.fillStyle = col.bg; ctx.fillRect(0, 0, ctx.canvas ? ctx.canvas.width : 9999, ctx.canvas ? ctx.canvas.height : 9999);
      // each row of cells along y as runs of one kind
      if (c && s.cells) {
        var px = c.cell * t.scale;
        for (var a = 0; a < c.w; ++a) for (var e = 0; e < c.h;) {
          var k = s.cells[a * c.h + e], run = 1;
          while (e + run < c.h && s.cells[a * c.h + e + run] === k) run++;
          if (kinds[k]) {
            p = RoomView.toScreen(t, c.x0 + (a + 1) * c.cell, c.y0 + (e + run) * c.cell);
            ctx.fillStyle = kinds[k]; ctx.fillRect(p[0], p[1], run * px + 0.5, px + 0.5);
          }
          e += run;
        }
      }
      function line(pts, colour, width, dash) {
        if (pts.length < 2) return;
        ctx.strokeStyle = colour; ctx.lineWidth = width; ctx.setLineDash(dash || []); ctx.beginPath();
        for (var j = 0; j < pts.length; ++j) { var q = RoomView.toScreen(t, pts[j][0], pts[j][1]); if (j) ctx.lineTo(q[0], q[1]); else ctx.moveTo(q[0], q[1]); }
        ctx.stroke(); ctx.setLineDash([]);
      }
      function dot(x, y, r, fill, stroke) {
        var q = RoomView.toScreen(t, x, y);
        ctx.beginPath(); ctx.arc(q[0], q[1], r, 0, 2 * Math.PI);
        if (fill) { ctx.fillStyle = fill; ctx.fill(); }
        if (stroke) { ctx.strokeStyle = stroke; ctx.lineWidth = 2; ctx.stroke(); }
        return q;
      }
      function text(str, q, colour) { ctx.fillStyle = colour; ctx.font = "bold 12px sans-serif"; ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.fillText(str, q[0], q[1]); }
      var pk = s.pick && RoomView.blockAt(s, s.pick.x, s.pick.y, TAP_M);
      // the block a name is on, lit (R32.7)
      if (pk) {
        ctx.fillStyle = col.pick;
        for (i = 0; i < pk.cells.length; ++i) {
          var m = centreOf(c, pk.cells[i]), sq = RoomView.toScreen(t, m[0] + c.cell / 2, m[1] + c.cell / 2);
          ctx.fillRect(sq[0], sq[1], c.cell * t.scale + 0.5, c.cell * t.scale + 0.5);
        }
        dot(pk.mid[0], pk.mid[1], 6, col.pick, col.text);
      }
      if (s.area) {
        p = RoomView.toScreen(t, s.area.x, s.area.y);
        ctx.strokeStyle = col.area; ctx.lineWidth = 2; ctx.setLineDash([8, 6]); ctx.beginPath();
        ctx.arc(p[0], p[1], s.area.r * t.scale, 0, 2 * Math.PI); ctx.stroke(); ctx.setLineDash([]);
      }
      if (s.home) text("⌂", dot(0, 0, 12, col.bg, col.area), col.area);
      if (s.layers.why) {
        for (i = 0; i < s.why.looked.length; ++i) dot(s.why.looked[i][0], s.why.looked[i][1], 0.15 * t.scale, col.cone);
        line(s.why.route, col.route, 3, [6, 5]);
        for (i = 0; i < s.why.pending.length; ++i) text("?", dot(s.why.pending[i][0], s.why.pending[i][1], 9, null, col.route), col.route);
        if (s.why.check) dot(s.why.check[0], s.why.check[1], 12, null, col.route);
      }
      if (s.layers.trail) line(s.trail, col.trail, 3);
      if (s.layers.names) for (i = 0; i < s.names.length; ++i) {
        var b = RoomView.labelBox(t, s.names[i]);
        ctx.fillStyle = col.bg; ctx.fillRect(b.x, b.y, b.w, b.h);
        text(s.names[i].name, [b.x + b.w / 2, b.y + b.h / 2], col.text);
      }
      if (s.layers.things) for (i = 0; i < s.things.length; ++i) {
        text(s.things[i].word.charAt(0).toUpperCase(), dot(s.things[i].x, s.things[i].y, 13, col.bg, col.pin), col.pin);
      }
      // the camera's view (heading + pan), then the tank as an arrow
      if (s.pose) {
        var h = (s.pose.h + (s.pan || 0)) * Math.PI / 180, half = CONE_DEG / 2 * Math.PI / 180;
        var o = RoomView.toScreen(t, s.pose.x, s.pose.y);
        var l = RoomView.toScreen(t, s.pose.x + CONE_M * Math.cos(h + half), s.pose.y + CONE_M * Math.sin(h + half));
        var r = RoomView.toScreen(t, s.pose.x + CONE_M * Math.cos(h - half), s.pose.y + CONE_M * Math.sin(h - half));
        ctx.fillStyle = col.cone; ctx.beginPath(); ctx.moveTo(o[0], o[1]); ctx.lineTo(l[0], l[1]); ctx.lineTo(r[0], r[1]); ctx.closePath(); ctx.fill();
        var th = s.pose.h * Math.PI / 180, f = 0.14, w = 0.09;
        var nose = RoomView.toScreen(t, s.pose.x + f * Math.cos(th), s.pose.y + f * Math.sin(th));
        var lt = RoomView.toScreen(t, s.pose.x - f * Math.cos(th) - w * Math.sin(th), s.pose.y - f * Math.sin(th) + w * Math.cos(th));
        var rt = RoomView.toScreen(t, s.pose.x - f * Math.cos(th) + w * Math.sin(th), s.pose.y - f * Math.sin(th) - w * Math.cos(th));
        ctx.fillStyle = col.pin; ctx.beginPath(); ctx.moveTo(nose[0], nose[1]); ctx.lineTo(lt[0], lt[1]); ctx.lineTo(rt[0], rt[1]); ctx.closePath(); ctx.fill();
      }
    }
  };

  return { RoomModel: RoomModel, RoomWords: RoomWords, RoomView: RoomView, splitMap: splitMap };
})();
if (typeof window !== "undefined") window.cqRoom = cqRoomLib;
if (typeof module !== "undefined") module.exports = cqRoomLib;
