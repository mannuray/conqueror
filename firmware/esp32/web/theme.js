/* The look of every page (bridge LLD R28, design-notes §206-§207): the cockpit or Sunny Play, one choice per device.
   pack_web.py puts this into each page's <head> (the CQ:THEME marker), so the look is set before the page is drawn.
   Canvas code takes its colours from colour("--name") and redraws on the window event "cqtheme". */
var cqTheme = (function () {
  "use strict";
  var KEY = "cq_theme";

  // the saved look when it is one; else Sunny on a touch device and the cockpit with a mouse
  function pick(saved, coarse) {
    if (saved === "play" || saved === "cockpit") return saved;
    return coarse ? "play" : "cockpit";
  }

  function root() { return document.documentElement; }
  function current() { return root().dataset.theme; }

  function flip() {
    var t = current() === "play" ? "cockpit" : "play";
    root().dataset.theme = t;
    try { localStorage.setItem(KEY, t); } catch (e) { /* private mode: this visit only */ }
    syncBar();
    window.dispatchEvent(new Event("cqtheme"));
    return t;
  }

  function colour(name) { return getComputedStyle(root()).getPropertyValue(name).trim(); }

  // the phone's browser bar takes the page colour of the look
  function syncBar() {
    var m = document.querySelector('meta[name="theme-color"]');
    if (m) m.content = colour("--bg");
  }

  if (typeof document !== "undefined") {
    var saved = null;
    try { saved = localStorage.getItem(KEY); } catch (e) { /* private mode */ }
    root().dataset.theme = pick(saved, window.matchMedia("(pointer: coarse)").matches);
    document.addEventListener("DOMContentLoaded", syncBar);  // the page's own :root is read by then
  }
  return { KEY: KEY, pick: pick, current: current, flip: flip, colour: colour };
})();
if (typeof window !== "undefined") window.cqTheme = cqTheme;
if (typeof module !== "undefined") module.exports = cqTheme;
