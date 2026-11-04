/* One status strip on /ai, /logo and /settings (bridge LLD R30, design-notes §209): battery, AI HELPER, pictures/s (/ai
   only), HDG, POS, SPEED, WIFI, UNO. pack_web.py puts this in at the CQ:STRIP marker with strip.html and strip.css. A page
   hands every tank message to cqStrip.tank(m) and calls cqStrip.linkDown() when its socket drops; /ai also gives the AI
   helper's feed state, the engine's pose and pictures/s. Each chip's text lives in `view`, so the rules run in node too. */
var cqStripLib = (function () {
  "use strict";
  // The battery as the drive page shows it: 2S Li-ion resting voltage -> percent (basic_drive.html BATT_CURVE),
  // averaged over a few readings; under 5.5 V the tank runs on USB (no pack).
  var BATT_CURVE = [[8.4, 100], [8.2, 90], [8.0, 80], [7.8, 65], [7.6, 50], [7.4, 35], [7.2, 20], [7.0, 10], [6.8, 5], [6.6, 0]];
  var CELL_PX = 21;  // the battery cell's inside, full

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
  // WiFi by RSSI as the drive page (basic_drive.html pollWifi); no reading = no signal
  function wifiIcon(rssi) {
    if (rssi === null || rssi === undefined) return "wifi-zero";
    return rssi > -55 ? "wifi" : rssi > -65 ? "wifi-high" : rssi > -75 ? "wifi-low" : "wifi-zero";
  }
  function wifiText(mode, rssi) {
    if (mode === "home") return { icon: wifiIcon(rssi), text: "HOME", warn: rssi === null || rssi === undefined || rssi <= -65 };
    if (mode === "connecting") return { icon: "wifi-zero", text: "CONNECTING", warn: true };
    return { icon: "wifi", text: "OWN WIFI", warn: false };
  }
  function posText(x, y) { return "POS " + x.toFixed(1) + "," + y.toFixed(1); }

  function Strip() {
    this.mean = new BattMean(10);
    this.aiOwned = false;  // /ai sets the AI HELPER chip from its own feed; ai_at then leaves it alone
    this.view = { batt: { text: "--", fill: 0, level: "ok", title: "battery" }, ai: false, fps: "-- / s", hdg: "HDG --",
      pos: "POS --", spd: "SPEED --", wifi: { icon: "wifi-zero", text: "WIFI --", warn: false }, uno: { text: "UNO --", warn: false } };
    this.render = function () {};
  }
  Strip.prototype.batt = function (v, flags) {
    var b = battState(v, flags);
    this.view.batt = b.usb ? { text: "USB", fill: 0, level: "ok", title: "on USB power" }
      : { text: b.pct + " %", fill: Math.round(b.pct * CELL_PX / 100), level: b.level, title: "battery " + v.toFixed(2) + " V" };
  };
  Strip.prototype.tank = function (m) {
    var v = this.view;
    switch (m.t) {
      case "welcome": v.uno = m.tank && m.tank.fw && m.tank.fw.uno ? { text: "UNO ONLINE", warn: false } : { text: "UNO OFFLINE", warn: true }; break;
      case "ts":
        if (typeof m.batt_v === "number") this.batt(this.mean.push(m.batt_v), m.flags || []);
        v.uno = (m.flags || []).indexOf("body_offline") >= 0 ? { text: "UNO OFFLINE", warn: true } : { text: "UNO ONLINE", warn: false };
        break;
      case "tf":
        if (typeof m.yaw === "number") v.hdg = "HDG " + Math.round(m.yaw) + "°";
        if (m.spd_cmd) v.spd = "SPEED " + Math.max(Math.abs(m.spd_cmd[0]), Math.abs(m.spd_cmd[1])) + "%";
        break;
      case "wifi_status": v.wifi = wifiText(m.mode, m.rssi); break;
      case "ai_at": if (!this.aiOwned) v.ai = !!m.url; break;
      case "ev": if (m.kind === "logo_pose" && typeof m.x_cm === "number") v.pos = posText(m.x_cm / 100, m.y_cm / 100); break;
      default: return;
    }
    this.render();
  };
  Strip.prototype.linkDown = function () {
    this.mean = new BattMean(10);
    this.view.wifi = { icon: "wifi-off", text: "RECONNECTING", warn: true };
    this.view.batt = { text: "NO TANK", fill: 0, level: "low", title: "battery" };
    this.view.uno = { text: "UNO --", warn: false };
    this.render();
  };
  Strip.prototype.aiHelper = function (on) { this.aiOwned = true; this.view.ai = !!on; this.render(); };
  Strip.prototype.pose = function (x, y) { this.view.pos = posText(x, y); this.render(); };
  Strip.prototype.fps = function (n) { this.view.fps = n === null || n === undefined ? "-- / s" : n + " / s"; this.render(); };

  // the chips on the page: only text, classes and a width are set (no markup from messages)
  function bind(s) {
    function el(id) { return document.getElementById(id); }
    s.showFps = function () { el("cFps").hidden = false; };
    s.render = function () {
      var v = s.view;
      el("battTxt").textContent = v.batt.text; el("battFill").style.width = v.batt.fill + "px"; el("cBatt").title = v.batt.title;
      el("cBatt").className = "sc batt" + (v.batt.level === "critical" ? " crit" : v.batt.level === "low" ? " low" : "");
      el("aiTxt").textContent = v.ai ? "AI HELPER" : "AI HELPER OFF"; el("cAi").className = v.ai ? "sc on" : "sc warn";
      el("cFps").textContent = v.fps; el("cHdg").textContent = v.hdg; el("cPos").textContent = v.pos; el("cSpd").textContent = v.spd;
      el("wifiTxt").textContent = v.wifi.text; el("cWifi").className = "sc" + (v.wifi.warn ? " warn" : "");
      var icons = el("cWifi").querySelectorAll("svg");
      for (var i = 0; i < icons.length; i++) icons[i].setAttribute("class", icons[i].getAttribute("data-w") === v.wifi.icon ? "on" : "");
      el("unoTxt").textContent = v.uno.text; el("cUno").className = "sc" + (v.uno.warn ? " warn" : "");
    };
    s.render();
    return s;
  }
  return { battPct: battPct, battState: battState, BattMean: BattMean, wifiIcon: wifiIcon, wifiText: wifiText, posText: posText,
    Strip: Strip, bind: bind };
})();
if (typeof document !== "undefined" && document.getElementById("cqStrip")) window.cqStrip = cqStripLib.bind(new cqStripLib.Strip());
if (typeof module !== "undefined") module.exports = cqStripLib;
