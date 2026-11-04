/* One top bar on every page (bridge LLD R29, design-notes §208): the look, the one menu and STOP at the top right.
   pack_web.py puts this into each page with bar.html and bar.css (the CQ:BAR marker). Each page keeps its own STOP
   handler on #stop; a page with no socket of its own (Help) stops through stopOnce(). Opening the menu sends the window
   event "cqmenu" so a page can close its own sheets. */
var cqBar = (function () {
  "use strict";
  var PAGES = [
    { path: "/", name: "DRIVE", line: "drive and look around", icon: "joystick" },
    { path: "/logo", name: "LOGO", line: "write and run programs", icon: "code" },
    { path: "/ai", name: "AI", line: "find, explore, teach it a word", icon: "sparkles" },
    { path: "/settings", name: "SETTINGS", line: "WiFi · tank · tune · log", icon: "settings" },
    { path: "/help", name: "HELP", line: "how-to and facts", icon: "circle-help" }
  ];
  var STOP_WAIT_MS = 2000;

  function here(path) {
    for (var i = 0; i < PAGES.length; i++) if (PAGES[i].path === path) return PAGES[i];
    return null;
  }

  function el(id) { return document.getElementById(id); }
  function isOpen() { return typeof document !== "undefined" && !!el("cqMenu") && !el("cqMenu").hidden; }

  function show(on) {
    el("cqMenu").hidden = !on; el("cqDim").hidden = !on;
    el("menuBtn").setAttribute("aria-expanded", on ? "true" : "false");
    document.documentElement.classList.toggle("cq-open", on);
  }
  function open() {
    if (isOpen()) return;
    show(true);
    el("cqMenu").querySelector("a").focus();  // the keyboard starts in the menu
    window.dispatchEvent(new Event("cqmenu"));
  }
  function close() {
    if (!isOpen()) return;
    var inside = el("cqMenu").contains(document.activeElement);
    show(false);
    if (inside) el("menuBtn").focus();  // back where it came from
  }

  // STOP through a socket opened only for this press (the bridge takes 4 clients): hello, stop, logo_stop; done(true)
  // on the ack of the stop itself (id 1: the body has stopped; a logo_stop ack or a nack is not enough), done(false) on
  // an error, a close or 2 s without it; the socket is closed every time.
  function stopOnce(url, done, opts) {
    var o = opts || {};
    var Ws = o.WebSocket || WebSocket, after = o.setTimeout || setTimeout, cancel = o.clearTimeout || clearTimeout;
    var finished = false, ws = null, timer = null;
    function end(ok) {
      if (finished) return;
      finished = true;
      if (timer !== null) cancel(timer);
      try { if (ws) ws.close(); } catch (e) { /* already closed */ }
      done(ok);
    }
    timer = after(function () { timer = null; end(false); }, STOP_WAIT_MS);
    try { ws = new Ws(url); } catch (e) { end(false); return; }
    ws.onopen = function () {
      try {
        ws.send(JSON.stringify({ t: "hello", client: "help", proto: 1 }));
        ws.send(JSON.stringify({ t: "stop", id: 1 }));
        ws.send(JSON.stringify({ t: "logo_stop", id: 2 }));
      } catch (e) { end(false); }
    };
    ws.onmessage = function (ev) {
      var m = null;
      try { m = JSON.parse(ev.data); } catch (e) { return; }
      if (m && m.t === "ack" && m.id === 1) end(true);
    };
    ws.onerror = function () { end(false); };
    ws.onclose = function () { end(false); };
  }

  if (typeof document !== "undefined" && el("cqMenu")) {
    // the menu and its dim layer live at the end of <body>, outside any glass or sticky box of the page
    document.body.appendChild(el("cqDim"));
    document.body.appendChild(el("cqMenu"));
    var cur = here(location.pathname);
    if (cur) {
      var links = el("cqMenu").querySelectorAll("a[data-page]");
      for (var i = 0; i < links.length; i++) if (links[i].getAttribute("data-page") === cur.path) links[i].setAttribute("aria-current", "page");
    }
    el("menuBtn").addEventListener("click", function () { if (isOpen()) close(); else open(); });
    el("cqDim").addEventListener("click", close);
    el("stop").addEventListener("click", close);  // STOP's answer (a toast, a line) must not sit under the dim layer
    window.addEventListener("pageshow", close);   // Back on iOS restores the page as it was left: never with the menu open
    el("lookBtn").addEventListener("click", function () { cqTheme.flip(); });  // the look, for every page (R28)
    // Esc closes the menu; the event goes on, so a page's own Esc (e.g. /logo's stop) still runs
    document.addEventListener("keydown", function (e) { if (e.key === "Escape") close(); });
  }
  return { PAGES: PAGES, here: here, open: open, close: close, isOpen: isOpen, stopOnce: stopOnce };
})();
if (typeof window !== "undefined") window.cqBar = cqBar;
if (typeof module !== "undefined") module.exports = cqBar;
