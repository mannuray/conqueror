/* The status strip (bridge LLD R30, R31): a view of the page's TankModel (window.cqTank) - battery, AI HELPER,
   pictures/s (/ai only), HDG, POS, SPEED, WIFI, UNO. It only draws: texts(state) turns the model's raw values into
   each chip's text, and it redraws whenever the model changes. Only text, classes and a width are set. */
var cqStripView = (function () {
  "use strict";
  var T = typeof module !== "undefined" ? require("./tank_model.js") : cqTankLib;  // node loads it; a page has it first (pack_web)
  var CELL_PX = 21;  // the battery cell's inside, full
  // WiFi by RSSI as the drive page (basic_drive.html pollWifi); no reading = no signal
  function wifiIcon(rssi) {
    if (rssi === null || rssi === undefined) return "wifi-zero";
    return rssi > -55 ? "wifi" : rssi > -65 ? "wifi-high" : rssi > -75 ? "wifi-low" : "wifi-zero";
  }
  // the house WiFi by its name, cut to 10 letters (owner 2026-11-05: "HOME" was taken for the AI's home spot); the
  // tank's own WiFi a tower and TANK HOTSPOT (ap_fallback: it found no house WiFi - a warning)
  var NAME_MAX = 10;
  function wifiText(mode, rssi, ssid) {
    if (mode === "home") {
      var name = !ssid ? "HOUSE WIFI" : ssid.length > NAME_MAX ? ssid.slice(0, NAME_MAX - 1) + "…" : ssid;
      return { icon: wifiIcon(rssi), text: name, warn: rssi === null || rssi === undefined || rssi <= -65 };
    }
    if (mode === "connecting") return { icon: "wifi-zero", text: "CONNECTING", warn: true };
    return { icon: "radio-tower", text: "TANK HOTSPOT", warn: mode === "ap_fallback" };
  }
  function posText(x, y) { return "POS " + x.toFixed(1) + "," + y.toFixed(1); }

  // each chip's text from the model's state
  function texts(s) {
    var batt = { text: "--", fill: 0, level: "ok", title: "battery" };
    if (s.batt) {
      var b = T.battState(s.batt.v, s.batt.flags);
      batt = b.usb ? { text: "USB", fill: 0, level: "ok", title: "on USB power" }
        : { text: b.pct + " %", fill: Math.round(b.pct * CELL_PX / 100), level: b.level, title: "battery " + s.batt.v.toFixed(2) + " V" };
    } else if (s.link === "down") batt = { text: "NO TANK", fill: 0, level: "low", title: "battery" };
    return {
      batt: batt, ai: s.ai,
      fps: s.fps === null ? "-- / s" : s.fps + " / s",
      hdg: s.hdg === null ? "HDG --" : "HDG " + Math.round(s.hdg) + "°",
      pos: s.pos === null ? "POS --" : posText(s.pos.x, s.pos.y),
      spd: s.spd === null ? "SPEED --" : "SPEED " + s.spd + "%",
      wifi: s.link === "down" ? { icon: "wifi-off", text: "RECONNECTING", warn: true }
        : s.wifi ? wifiText(s.wifi.mode, s.wifi.rssi, s.wifi.ssid) : { icon: "wifi-zero", text: "WIFI --", warn: false },
      uno: s.uno === null ? { text: "UNO --", warn: false } : s.uno ? { text: "UNO ONLINE", warn: false } : { text: "UNO OFFLINE", warn: true }
    };
  }

  function bind(model) {
    function el(id) { return document.getElementById(id); }
    function render() {
      var v = texts(model.state);
      el("battTxt").textContent = v.batt.text; el("battFill").style.width = v.batt.fill + "px"; el("cBatt").title = v.batt.title;
      el("cBatt").className = "sc batt" + (v.batt.level === "critical" ? " crit" : v.batt.level === "low" ? " low" : "");
      // the bridge tells only /ai where the AI helper is (R27.1): elsewhere the chip says it does not know, without a warning
      el("aiTxt").textContent = v.ai === null ? "AI HELPER --" : v.ai ? "AI HELPER" : "AI HELPER OFF";
      el("cAi").className = v.ai === null ? "sc" : v.ai ? "sc on" : "sc warn";
      el("cFps").textContent = v.fps; el("cHdg").textContent = v.hdg; el("cPos").textContent = v.pos; el("cSpd").textContent = v.spd;
      el("wifiTxt").textContent = v.wifi.text; el("cWifi").className = "sc" + (v.wifi.warn ? " warn" : "");
      var icons = el("cWifi").querySelectorAll("svg");
      for (var i = 0; i < icons.length; i++) icons[i].setAttribute("class", icons[i].getAttribute("data-w") === v.wifi.icon ? "on" : "");
      el("unoTxt").textContent = v.uno.text; el("cUno").className = "sc" + (v.uno.warn ? " warn" : "");
    }
    model.on(render);
    render();
    return { render: render, showFps: function () { el("cFps").hidden = false; } };
  }
  return { wifiIcon: wifiIcon, wifiText: wifiText, posText: posText, texts: texts, bind: bind };
})();
if (typeof document !== "undefined" && document.getElementById("cqStrip")) {
  window.cqStrip = cqStripView.bind(window.cqTank);  // the page's view: render(), showFps(); data goes to window.cqTank
}
if (typeof module !== "undefined") module.exports = cqStripView;
