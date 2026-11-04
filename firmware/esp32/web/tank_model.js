/* TankModel (bridge LLD R31, design-notes §210): the tank's data and its rules, no screen code. The link (cqLink) hands
   it every tank message; views (the status strip) listen with on() and draw from `state`. Raw values only - the views
   turn them into text. Put into a page with the status strip (R30) as window.cqTank. */
var cqTankLib = (function () {
  "use strict";
  // The battery as the drive page shows it: 2S Li-ion resting voltage -> percent (basic_drive.html BATT_CURVE),
  // averaged over a few readings; under 5.5 V the tank runs on USB (no pack).
  var BATT_CURVE = [[8.4, 100], [8.2, 90], [8.0, 80], [7.8, 65], [7.6, 50], [7.4, 35], [7.2, 20], [7.0, 10], [6.8, 5], [6.6, 0]];

  function battPct(v) {
    if (v >= BATT_CURVE[0][0]) return 100;
    for (var i = 1; i < BATT_CURVE.length; ++i) {
      var hi = BATT_CURVE[i - 1], lo = BATT_CURVE[i];
      if (v >= lo[0]) return Math.round(lo[1] + (v - lo[0]) / (hi[0] - lo[0]) * (hi[1] - lo[1]));
    }
    return 0;
  }
  function battState(v, flags) {
    if (v < 5.5) return { usb: true, pct: null, level: "ok" };
    var f = flags || [], pct = battPct(v);
    var level = f.indexOf("batt_critical") >= 0 ? "critical" : f.indexOf("batt_low") >= 0 || pct < 20 ? "low" : "ok";
    return { usb: false, pct: pct, level: level };
  }
  function BattMean(n) { this.n = n; this.vals = []; this.usb = null; }
  BattMean.prototype.push = function (v) {
    var usb = v < 5.5;
    if (usb !== this.usb) { this.vals = []; this.usb = usb; }  // USB <-> pack: the average starts again
    this.vals.push(v); if (this.vals.length > this.n) this.vals.shift();
    return this.vals.reduce(function (a, b) { return a + b; }, 0) / this.vals.length;
  };

  // state: link "off" | "on" | "down"; null = not known yet. ai: null until something says (the bridge tells only /ai,
  // R27.1). A page that sets ai or pos itself (/ai: its feed, the engine's pose) owns it from then on (R30.5).
  function TankModel() {
    this.state = { link: "off", you: "", owner: "none", ownerClient: "", ownerKnown: false, batt: null, ai: null, fps: null,
      hdg: null, pos: null, spd: null, wifi: null, uno: null };
    this.mean = new BattMean(10);
    this.aiOwned = false; this.posOwned = false;
    this.listeners = [];
  }
  TankModel.prototype.on = function (fn) { this.listeners.push(fn); };
  TankModel.prototype.changed = function () { for (var i = 0; i < this.listeners.length; i++) this.listeners[i](this.state); };
  TankModel.prototype.owner = function (o, client) {
    this.state.owner = o || "none"; this.state.ownerClient = client || ""; this.state.ownerKnown = true;
  };
  // one tank message; true when it changed the state (views were told)
  TankModel.prototype.apply = function (m) {
    var s = this.state;
    switch (m.t) {
      case "welcome":
        s.you = m.you || "";
        s.uno = !!(m.tank && m.tank.fw && m.tank.fw.uno);
        if (m.owner !== undefined) this.owner(m.owner, m.owner_client);
        break;
      case "ts":
        if (typeof m.batt_v === "number") s.batt = { v: this.mean.push(m.batt_v), flags: m.flags || [] };
        s.uno = (m.flags || []).indexOf("body_offline") < 0;
        if (m.owner !== undefined) this.owner(m.owner, m.owner_client);  // who drives, each second (as the pages read it)
        break;
      case "tf":
        if (typeof m.yaw === "number") s.hdg = m.yaw;
        if (m.spd_cmd) s.spd = Math.max(Math.abs(m.spd_cmd[0]), Math.abs(m.spd_cmd[1]));
        break;
      case "wifi_status": s.wifi = { mode: m.mode, rssi: m.rssi === undefined ? null : m.rssi }; break;
      case "ai_at": if (this.aiOwned) return false; s.ai = !!m.url; break;
      case "ev":
        if (m.kind === "logo_pose" && typeof m.x_cm === "number" && !this.posOwned) s.pos = { x: m.x_cm / 100, y: m.y_cm / 100 };
        else if (m.kind === "control_changed") this.owner(m.owner, m.client);
        else return false;
        break;
      default: return false;
    }
    this.changed();
    return true;
  };
  TankModel.prototype.linkUp = function () { this.state.link = "on"; this.changed(); };
  // the link dropped: nothing live or moving is kept (R30.5); the average starts again
  TankModel.prototype.linkDown = function () {
    var s = this.state;
    s.link = "down"; s.batt = null; s.uno = null; s.hdg = null; s.spd = null; s.pos = null;
    this.mean = new BattMean(10);
    this.changed();
  };
  TankModel.prototype.aiHelper = function (on) { this.aiOwned = true; this.state.ai = !!on; this.changed(); };
  TankModel.prototype.pose = function (x, y) { this.posOwned = true; this.state.pos = { x: x, y: y }; this.changed(); };
  TankModel.prototype.fps = function (n) { this.state.fps = n === undefined ? null : n; this.changed(); };
  return { battPct: battPct, battState: battState, BattMean: BattMean, TankModel: TankModel };
})();
if (typeof window !== "undefined") window.cqTank = new cqTankLib.TankModel();
if (typeof module !== "undefined") module.exports = cqTankLib;
