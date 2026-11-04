/* The /ai page - the deciding code (bridge LLD R27.4, AI engine LLD §22): plain functions, no DOM, so Node can
   test them (node --test firmware/esp32/web/test/ai.test.js). pack_web.py puts this file into /ai in place of
   <!--CQ:AI_JS-->. */
var AiCore = (function () {
  "use strict";

  var PROTO = 1;  // the AI feed's version this page speaks (AI engine LLD §22.2)
  var PROTO_TEXT = "Please update the AI helper.";
  var NEVER = "person";  // never a thing to find (AI engine LLD §21.4)
  // Words for people, refused when teaching (design-notes §203; the engine's PERSON_WORDS) - a second check.
  var PERSON_WORDS = ("person people human humans kid kids child children baby babies toddler man men woman women boy boys" +
    " girl girls mum mummy mom mommy mother dad daddy father brother sister grandma grandpa granny me").split(" ");
  var MAX_NAMES = 8, MAX_BYTES = 24;
  var WORD_RE = /^[a-z][a-z -]*$/;

  function squeeze(s) { return String(s).toLowerCase().split(/\s+/).filter(Boolean).join(" "); }
  // A person word anywhere in the text (also used for names on the ROOM map, bridge LLD R32).
  function isPerson(text) {
    var parts = squeeze(text).replace(/-/g, " ").split(" ");
    for (var i = 0; i < parts.length; ++i) if (parts[i] === NEVER || PERSON_WORDS.indexOf(parts[i]) >= 0) return true;
    return false;
  }

  // One frame from the engine: a 4-byte big-endian header length, the header JSON, the camera JPEG, the depth JPEG.
  function splitFrame(buf) {
    if (buf.byteLength < 4) throw new Error("short frame");
    var n = new DataView(buf).getUint32(0);
    if (4 + n > buf.byteLength) throw new Error("short frame");
    var header = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 4, n)));
    var at = 4 + n, cam = header.cam | 0, depth = header.depth | 0;
    if (at + cam + depth > buf.byteLength) throw new Error("short frame");
    return {
      header: header,
      cam: new Uint8Array(buf, at, cam),
      depth: depth ? new Uint8Array(buf, at + cam, depth) : null
    };
  }

  // A box [word, score, x, y, w, h] (0..1000 of the picture) in canvas pixels.
  function boxRect(b, cw, ch) {
    return { word: b[0], score: b[1], x: Math.round(b[2] * cw / 1000), y: Math.round(b[3] * ch / 1000),
             w: Math.round(b[4] * cw / 1000), h: Math.round(b[5] * ch / 1000) };
  }

  // The buttons' one-line programs (Tank Logo LLD §24.4), run with logo_run on the tank.
  function num(v) { return String(Math.round(v * 10) / 10); }
  function findLine(word, areaOn, area) { return (areaOn ? "AREA " + num(area) + "  " : "") + 'FIND "' + word; }
  function wanderLine(s) { return "WANDER " + Math.round(s); }
  function exploreLine() { return "EXPLORE"; }

  // What the child typed: a word or one of its names runs as FIND; anything else is unknown (teach it).
  function typed(text, things) {
    var t = squeeze(text);
    for (var i = 0; t !== NEVER && i < things.length; ++i) {
      var th = things[i];
      if (th.word === t || (th.names || []).indexOf(t) >= 0) return { kind: "run", word: th.word, measured: th.measured !== false };
    }
    return { kind: "unknown", word: t };
  }
  function unknownText(word) { return "I don't know what a " + word + " is."; }

  // The engine's own rules (AI engine LLD §22.4), checked before asking: null = fine, else the reason.
  function teachCheck(word, names) {
    var w = squeeze(word), ns = (names || []).map(squeeze).filter(Boolean);
    var parts = [w].concat(ns).join(" ").split(" ");
    for (var p = 0; p < parts.length; ++p) if (PERSON_WORDS.indexOf(parts[p]) >= 0) return "I never look for people.";
    if (ns.length > MAX_NAMES) return "At most " + MAX_NAMES + " names.";
    var all = [w].concat(ns);
    for (var i = 0; i < all.length; ++i) {
      if (!WORD_RE.test(all[i])) return "Use letters, spaces and - only.";
      if (new TextEncoder().encode(all[i]).length > MAX_BYTES) return "That word is too long.";
    }
    return null;
  }

  // The phone's voice: each new sentence once; nothing before the first tap (iOS) or while muted.
  function Sayer(speak) { this.speak = speak; this.last = null; this.unlocked = false; this.muted = false; }
  Sayer.prototype.unlock = function () { this.unlocked = true; };
  Sayer.prototype.mute = function (on) { this.muted = !!on; };
  Sayer.prototype.say = function (text) {  // before the first tap nothing is kept: it is said after the tap
    if (!text || !this.unlocked || text === this.last) return;
    this.last = text;
    if (!this.muted) this.speak(text);
  };

  // Where the camera comes from (bridge LLD R27.4): NoAI, Engine (the feed) or TankStream (the tank's :81).
  function cameraState(state, ev) {
    if (ev === "feed_open") return "Engine";
    if (ev === "feed_closed" || ev === "ai_at_null") return state === "Engine" ? "NoAI" : state;
    if (ev === "stream_ok") return state === "NoAI" ? "TankStream" : state;
    return state;
  }

  function protoOk(n) { return n === PROTO; }

  // The voice: one on the device first (an online voice can be silent on a home network), the page's language first,
  // then any English one.
  function pickVoice(voices, lang) {
    var base = String(lang || "en").slice(0, 2);
    function find(ok) { for (var i = 0; i < voices.length; ++i) if (ok(voices[i])) return voices[i]; return null; }
    return find(function (v) { return v.localService && v.lang === lang; }) ||
      find(function (v) { return v.localService && v.lang.slice(0, 2) === base; }) ||
      find(function (v) { return v.localService && v.lang.slice(0, 2) === "en"; }) ||
      find(function (v) { return v.lang === lang; }) || find(function (v) { return v.lang.slice(0, 2) === "en"; }) ||
      voices[0] || null;
  }

  // An ai_at for the feed already in use changes nothing (final review I2: each unlock re-sends it).
  function sameFeed(url, proto, newUrl, newProto) { return !!url && url === newUrl && proto === newProto; }

  return {
    PROTO: PROTO, PROTO_TEXT: PROTO_TEXT, splitFrame: splitFrame, boxRect: boxRect, findLine: findLine,
    wanderLine: wanderLine, exploreLine: exploreLine, typed: typed, unknownText: unknownText, teachCheck: teachCheck,
    Sayer: Sayer, cameraState: cameraState, protoOk: protoOk, sameFeed: sameFeed, squeeze: squeeze,
    pickVoice: pickVoice, isPerson: isPerson
  };
})();
if (typeof module !== "undefined") module.exports = AiCore;
