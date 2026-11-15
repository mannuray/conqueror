/* The look of every page (bridge LLD R28, design-notes §206-§207): the cockpit or Sunny Play, one choice per device.
   pack_web.py puts this into each page's <head> (the CQ:THEME marker), so the look is set before the page is drawn.
   Canvas code takes its colours from colour("--name") and redraws on the window event "cqtheme".
   Every button answers a press with a press look while the finger is down (at least 150 ms), the class cq-press (bridge LLD R31.10, design-notes §253). */
var cqTheme = (function () {
  "use strict";
  var KEY = "cq_theme";
  var PRESS_MS = 150;  // how long a pressed button shows the press look

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

  // the button a press landed on (from an icon inside it too); none when disabled
  function pressTarget(node) {
    var b = node && node.closest ? node.closest("button") : null;
    return b && !b.disabled ? b : null;
  }
  // the press look stays while the finger is down, and at least PRESS_MS in all (a quick tap is seen too)
  function releaseDelay(downMs, nowMs) { return Math.max(0, PRESS_MS - (nowMs - downMs)); }
  var held = null, heldAt = 0;
  function pressed(b) {
    if (!b) return;
    lift();
    b.classList.add("cq-press");
    held = b; heldAt = Date.now();
  }
  function lift() {
    var b = held;
    if (!b) return;
    held = null;
    setTimeout(function () { b.classList.remove("cq-press"); }, releaseDelay(heldAt, Date.now()));
  }

  if (typeof document !== "undefined") {
    // capture: a button's own handler that prevents the default (STOP's) cannot hide it; CSS :active alone often
    // shows nothing on a phone's touch
    document.addEventListener("pointerdown", function (e) { pressed(pressTarget(e.target)); }, true);
    document.addEventListener("pointerup", lift, true);
    document.addEventListener("pointercancel", lift, true);
    document.addEventListener("click", function (e) {  // Enter / Space: no finger to lift
      if (e.detail === 0 && pressTarget(e.target)) { pressed(pressTarget(e.target)); lift(); }
    }, true);
    var saved = null;
    try { saved = localStorage.getItem(KEY); } catch (e) { /* private mode */ }
    root().dataset.theme = pick(saved, window.matchMedia("(pointer: coarse)").matches);
    document.addEventListener("DOMContentLoaded", syncBar);  // the page's own :root is read by then
  }
  return { KEY: KEY, PRESS_MS: PRESS_MS, pressTarget: pressTarget, releaseDelay: releaseDelay, pick: pick, current: current, flip: flip, colour: colour };
})();
if (typeof window !== "undefined") window.cqTheme = cqTheme;
if (typeof module !== "undefined") module.exports = cqTheme;
