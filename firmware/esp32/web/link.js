/* cqLink, the one link every page uses to the tank (bridge LLD R31, design-notes §210) - the controller of the pages'
   model / view / controller. It says hello with the page's name, pings each second, closes a link silent for 4 s,
   reconnects doubling to 5 s, holds no link while the page is hidden (§77), asks for the WiFi strength every 10 s,
   hands every message to the page's TankModel and then to the page, and stops through a socket of its own when the
   link is down. env (WebSocket, timers, clock, document, url, stopOnce) is for the node tests. */
var cqLink = (function () {
  "use strict";
  var PING_MS = 1000, SILENT_MS = 4000, MAX_RETRY_MS = 5000, WIFI_EVERY = 10;  // WiFi: every 10 pings

  function browserEnv() {
    return { WebSocket: WebSocket, setTimeout: setTimeout.bind(window), clearTimeout: clearTimeout.bind(window),
      setInterval: setInterval.bind(window), clearInterval: clearInterval.bind(window), now: Date.now, doc: document,
      url: "ws://" + location.host + "/ws", stopOnce: function (url, done) { cqBar.stopOnce(url, done); } };
  }

  // opts: client (the hello name), model, onMessage(m), onOpen(), onClose(), onHide(), retryMs (first wait), env
  function open(opts) {
    var env = opts.env || browserEnv(), first = opts.retryMs || 500, retry = first;
    var ws = null, up = false, lastRx = 0, pingTimer = 0, retryTimer = 0, pings = 0;
    function send(msg) { if (ws && up) { ws.send(JSON.stringify(msg)); return true; } return false; }
    // the link is gone (closed, silent, failed to open or the page hidden): the model and the page are told
    function down() {
      var s = ws;
      up = false; ws = null;
      env.clearInterval(pingTimer); pingTimer = 0;
      if (s) { s.onopen = s.onmessage = s.onclose = s.onerror = null; try { s.close(); } catch (e) { /* already closed */ } }
      if (opts.model) opts.model.linkDown();
      if (opts.onClose) opts.onClose();
    }
    function later() {
      if (env.doc.hidden) return;
      env.clearTimeout(retryTimer);
      retryTimer = env.setTimeout(connect, retry);
      retry = Math.min(retry * 2, MAX_RETRY_MS);
    }
    function connect() {
      if (env.doc.hidden || ws) return;  // never two sockets
      env.clearTimeout(retryTimer);
      ws = new env.WebSocket(env.url);
      ws.onopen = function () {
        up = true; retry = first; lastRx = env.now(); pings = 0;
        send({ t: "hello", client: opts.client, proto: 1 });
        send({ t: "wifi_status" });
        if (opts.model) opts.model.linkUp();
        if (opts.onOpen) opts.onOpen();
        pingTimer = env.setInterval(function () {
          if (env.now() - lastRx > SILENT_MS) { down(); later(); return; }  // the browser's own close may take long
          send({ t: "ping", t0: env.now() });
          if (++pings >= WIFI_EVERY) { pings = 0; send({ t: "wifi_status" }); }
        }, PING_MS);
      };
      ws.onmessage = function (ev) {
        var m;
        lastRx = env.now();
        try { m = JSON.parse(ev.data); } catch (e) { return; }
        if (opts.model) opts.model.apply(m);
        if (opts.onMessage) opts.onMessage(m);
      };
      ws.onclose = function () { down(); later(); };
      ws.onerror = function () { var s = ws; try { if (s) s.close(); } catch (e) { /* closing */ } };
    }
    env.doc.addEventListener("visibilitychange", function () {
      if (env.doc.hidden) {
        if (opts.onHide) opts.onHide();  // the drive page's STOP goes out before the close (§201)
        env.clearTimeout(retryTimer);
        if (ws) down();
      } else { retry = first; connect(); }
    });
    // STOP: on the link when it is up; else through a socket opened for the press, and said
    function stop(msgs, say) {
      if (up) { for (var i = 0; i < msgs.length; i++) send(msgs[i]); return; }
      if (say) say("Stopping…", true);
      env.stopOnce(env.url, function (ok) { if (say) say(ok ? "Stopped" : "Couldn't reach the tank - use another STOP", ok); });
    }
    connect();
    return { send: send, isOpen: function () { return up; }, stop: stop };
  }
  return { open: open };
})();
if (typeof window !== "undefined") window.cqLink = cqLink;
if (typeof module !== "undefined") module.exports = cqLink;
