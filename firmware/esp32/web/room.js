/* The /ai page's ROOM tab - its model, words and view (bridge LLD R32, AI engine LLD §23): plain functions, no
   DOM, so Node can test them (node --test firmware/esp32/web/test/room.test.js). pack_web.py puts this file into
   /ai in place of <!--CQ:ROOM_JS-->, after ai.js (it uses AiCore). */
var cqRoomLib = (function () {
  "use strict";
  var Core = typeof AiCore !== "undefined" ? AiCore : require("./ai.js");
  var LAYERS = ["things", "trail", "names", "why"];
  var BAD_NAME = "Names are letters, spaces and - (up to 24).";
  var MAX_CM = 1000;  // SETXY's inputs (Tank Logo LLD §24)

  // ---------- the model: what the engine sent, raw ----------
  function RoomModel() {
    this.state = {
      cells: null, crop: null, trail: [], home: null, area: null, things: [], names: [],
      why: { route: [], pending: [], check: null, looked: [] }, seenM2: 0, pose: null, pan: 0,
      layers: { things: true, trail: true, names: true, why: false }, feed: "none"
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

  return { RoomModel: RoomModel, RoomWords: RoomWords };
})();
if (typeof window !== "undefined") window.cqRoom = cqRoomLib;
if (typeof module !== "undefined") module.exports = cqRoomLib;
