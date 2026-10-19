/* Tank Logo page - the deciding code (Tank Logo LLD §21.4): plain functions, no DOM, so Node can test them
   (node --test firmware/esp32/web/test/logo.test.js). pack_web.py puts this file into /logo in place of <!--CQ:LOGO_JS-->. */
var LogoCore = (function () {
  "use strict";

  // ---------- colours (§21.3): the colouring only colours, the tank's check decides what is right ----------
  var EXTRA = { END: "flow", UNTIL: "flow" };  // parts of TO ... END and WAIT UNTIL
  var TEXT_AFTER = { SAY: 1, PLAY: 1 };       // [ ... ] after these is text or notes, not words

  // word name or alias (upper case) -> group, and the preset names (LEFT, GREEN, HORN, ...)
  function wordIndex(words, presets) {
    var idx = { groups: {}, presets: {} };
    words.forEach(function (w) {
      w.name.split(" ").forEach(function (part) { if (!idx.groups[part]) idx.groups[part] = w.group; });
      (w.aliases || []).forEach(function (a) { idx.groups[a] = w.group; });
    });
    Object.keys(presets || {}).forEach(function (k) {
      presets[k].forEach(function (p) { idx.presets[String(p).toUpperCase()] = 1; });
    });
    Object.keys(EXTRA).forEach(function (k) { idx.groups[k] = EXTRA[k]; });
    return idx;
  }

  // the names after TO in the text (your own words, known before any check)
  function ownWords(text) {
    var own = {}, re = /(^|[\s\[\]])TO\s+([^\s\[\]();:"]+)/gi, m;
    while ((m = re.exec(text)) !== null) own[m[2].toUpperCase()] = 1;
    return own;
  }

  // text -> [{from, to, cls}] over the whole text; cls: g-move ... g-maths, num, own, inp, str, cmt, op, bad, sp
  function colour(text, idx, extraOwn) {
    var own = ownWords(text), spans = [], i = 0, n = text.length, textDepth = 0, lastWord = "", m;
    Object.keys(extraOwn || {}).forEach(function (k) { own[k.toUpperCase()] = 1; });
    function push(to, cls) { spans.push({ from: i, to: to, cls: cls }); i = to; }
    while (i < n) {
      var rest = text.slice(i), c = text[i];
      if ((m = /^\s+/.exec(rest))) { push(i + m[0].length, "sp"); continue; }
      if (c === ";") { var e = text.indexOf("\n", i); push(e < 0 ? n : e, "cmt"); continue; }
      if (c === "[") { if (textDepth > 0 || TEXT_AFTER[lastWord]) textDepth++; push(i + 1, "op"); lastWord = ""; continue; }
      if (c === "]") { if (textDepth > 0) textDepth--; push(i + 1, "op"); continue; }
      if (textDepth > 0) { m = /^[^\s\[\]]+/.exec(rest); push(i + m[0].length, "str"); continue; }
      if ((m = /^\d+(\.\d+)?/.exec(rest))) { push(i + m[0].length, "num"); lastWord = ""; continue; }
      if ((m = /^[+\-*\/=<>()]+/.exec(rest))) { push(i + m[0].length, "op"); continue; }
      m = /^[^\s\[\]()+\-*\/=<>;]+/.exec(rest);
      var tok = m[0], up = tok.toUpperCase(), cls;
      if (tok[0] === ":" || tok[0] === "?") cls = "inp";
      else if (tok[0] === '"') cls = "str";
      else if (idx.groups[up]) cls = "g-" + idx.groups[up];
      else if (own[up]) cls = "own";
      else if (idx.presets[up]) cls = "num";
      else cls = "bad";
      lastWord = up;
      push(i + tok.length, cls);
    }
    return spans;
  }

  // ---------- inserting a word (§14, §21): the example value selected, REPEAT with the cursor inside ----------
  // "|" = the cursor, "{...}" = the selection
  var TEMPLATES = {
    REPEAT: "REPEAT 4 [ | ]", FOREVER: "FOREVER [ | ]", IF: "IF {OBSTACLE?} [  ]",
    IFELSE: "IFELSE {DISTANCE < 30} [  ] [  ]", WHILE: "WHILE {NOT OBSTACLE?} [  ]",
    "WAIT UNTIL": "WAIT UNTIL {DISTANCE < 20}", TO: "TO {SHAPE}\n  \nEND", FOREACH: "FOREACH [ {50 100} ] [ FD ? ]"
  };

  function template(word) {
    if (TEMPLATES[word.name]) return TEMPLATES[word.name];
    var ex = word.example || "";
    if (word.kind !== "command" || ex.indexOf(word.name + " ") !== 0) return word.name + "|";
    var after = ex.slice(word.name.length + 1), b = after.indexOf("[ ");
    if (b >= 0 && after.lastIndexOf(" ]") === after.length - 2) {
      return word.name + " " + after.slice(0, b + 2) + "{" + after.slice(b + 2, after.length - 2) + "} ]";
    }
    return word.name + " {" + after + "}";
  }

  // where the word goes: a new line for a statement after other text (same indent), a space after other text
  function joiner(text, at, statement) {
    var before = text.slice(0, at), lineStart = before.lastIndexOf("\n") + 1, line = before.slice(lineStart);
    if (/^\s*$/.test(line)) return "";
    if (/\[\s*$/.test(line)) return /\s$/.test(line) ? "" : " ";
    if (statement) return "\n" + /^\s*/.exec(line)[0];
    return /\s$/.test(line) ? "" : " ";
  }

  // -> {text, selStart, selEnd}
  function insertWord(text, selStart, selEnd, word) {
    var t = template(word), statement = word.kind === "command" || word.kind === "control";
    var pre = joiner(text, selStart, statement), cur = t.indexOf("|"), a, b, body;
    if (cur >= 0) { body = t.replace("|", ""); a = b = cur; } else {
      a = t.indexOf("{"); body = t.replace("{", "").replace("}", ""); b = t.indexOf("}") - 1;
    }
    var head = text.slice(0, selStart);
    if (pre.charAt(0) === "\n") head = head.replace(/[ \t]+$/, "");  // no spaces left at the end of the line
    var out = head + pre + body + text.slice(selEnd), at = head.length + pre.length;
    return { text: out, selStart: at + a, selEnd: at + b };
  }

  // ---------- drafts in this browser (§21.8) ----------
  var DRAFT_KEY = "cq.logo.draft.";

  function Drafts(storage) { this.s = storage || null; }
  Drafts.prototype.keep = function (name, text, saved) {
    try {
      if (!this.s) return;
      if (text === saved) this.s.removeItem(DRAFT_KEY + name);
      else this.s.setItem(DRAFT_KEY + name, JSON.stringify({ text: text, saved: saved }));
    } catch (e) { this.s = null; }  // private mode / full: drafts off, the page still works
  };
  Drafts.prototype.restore = function (name) {
    try {
      var d = this.s ? JSON.parse(this.s.getItem(DRAFT_KEY + name) || "null") : null;
      return d && typeof d.text === "string" && typeof d.saved === "string" ? d : null;
    } catch (e) { return null; }
  };
  Drafts.prototype.clear = function (name) { try { if (this.s) this.s.removeItem(DRAFT_KEY + name); } catch (e) { this.s = null; } };
  Drafts.prototype.unsaved = function (name) { return this.restore(name) !== null; };

  // ---------- path view (§21.7): Logo axes, x right, y forward (up), right turns positive ----------
  var GRID_STEPS = [5, 10, 25, 50, 100, 250, 500];

  // points [{x, y}] in cm (the start 0, 0 always shown) -> {scale px/cm, ox, oy, grid cm}; screen y = oy - y * scale
  function fitView(points, w, h, margin) {
    var x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    points.forEach(function (p) { x0 = Math.min(x0, p.x); x1 = Math.max(x1, p.x); y0 = Math.min(y0, p.y); y1 = Math.max(y1, p.y); });
    var sx = Math.max(x1 - x0, 50), sy = Math.max(y1 - y0, 50);  // at least 50 cm across: a tiny path is not blown up
    var scale = Math.min((w - 2 * margin) / sx, (h - 2 * margin) / sy);  // the same scale both ways (1:1)
    var grid = GRID_STEPS.filter(function (g) { return g * scale >= 12; })[0] || 500;
    return { scale: scale, ox: w / 2 - ((x0 + x1) / 2) * scale, oy: h / 2 + ((y0 + y1) / 2) * scale, grid: grid };
  }

  function toScreen(v, x, y) { return { x: v.ox + x * v.scale, y: v.oy - y * v.scale }; }

  // the check's path [[x_mm, y_mm, heading, line, uncertain], ...] -> runs {solid, pts:[{x, y}] cm, endH}
  function plannedRuns(path) {
    var runs = [], cur = null;
    (path || []).forEach(function (p) {
      var solid = !p[4], pt = { x: p[0] / 10, y: p[1] / 10 };
      if (!cur || cur.solid !== solid) {
        cur = { solid: solid, pts: cur ? [cur.pts[cur.pts.length - 1]] : [] };
        runs.push(cur);
      }
      cur.pts.push(pt);
      cur.endH = p[2];  // heading at the run's last point (the arrow at the end of the plan)
    });
    return runs;
  }

  // ---------- checking as you type (§21.5) ----------
  function CheckScheduler(delayMs, retryMs) {
    this.delay = delayMs; this.retry = retryMs; this.edit = 0; this.dueAt = -1; this.inflight = 0;
  }
  CheckScheduler.prototype.edited = function (now) { this.edit++; this.dueAt = now + this.delay; return this.edit; };
  CheckScheduler.prototype.now = function (now) { this.dueAt = now; };  // check at once (CHECK_AGAIN, page shown)
  // -> the edit number to check now, or 0
  CheckScheduler.prototype.due = function (now) {
    if (this.inflight || this.dueAt < 0 || now < this.dueAt) return 0;
    this.dueAt = -1; this.inflight = this.edit;
    return this.edit;
  };
  // the reply for edit e: true = it is for the current text (show it), false = stale (drop it)
  CheckScheduler.prototype.replied = function (e) { this.inflight = 0; return e === this.edit; };
  // busy (503) or no answer: try again a little later, unless the text changed meanwhile (already due)
  CheckScheduler.prototype.busy = function (e, now) {
    this.inflight = 0;
    if (e === this.edit && this.dueAt < 0) this.dueAt = now + this.retry;
  };

  // ---------- one /logo request at a time (bridge: one worker job at a time, a second gets 503 busy) ----------
  // send(method, path, body) -> Promise of {status, data}: data = parsed JSON, the text, or null.
  // A 503 is tried again after retryMs, at most `tries` times; then it is given back as {status: 503}.
  function RequestQueue(fetchFn, later, retryMs, tries) {
    this.f = fetchFn; this.later = later; this.retry = retryMs; this.tries = tries; this.q = []; this.busy = false;
  }
  RequestQueue.prototype.send = function (method, path, body) {
    var self = this;
    return new Promise(function (resolve) { self.q.push({ m: method, p: path, b: body, left: self.tries, done: resolve }); self.next(); });
  };
  RequestQueue.prototype.next = function () {
    var self = this, job = this.q[0];
    if (this.busy || !job) return;
    this.busy = true;
    var opts = { method: job.m };
    if (job.b !== undefined) { opts.body = job.b; opts.headers = { "Content-Type": "text/plain" }; }
    function finish(res) { self.q.shift(); self.busy = false; job.done(res); self.next(); }
    this.f(job.p, opts).then(function (r) {
      if (r.status === 503 && job.left > 1) {
        job.left--; self.busy = false;
        var later = self.later;  // called on its own: a browser's setTimeout refuses another `this`
        later(function () { self.next(); }, self.retry);
        return null;
      }
      var json = (r.headers && r.headers.get && r.headers.get("Content-Type") || "").indexOf("json") >= 0;
      return (json ? r.json() : r.text()).then(function (d) { finish({ status: r.status, data: d }); },
        function () { finish({ status: r.status, data: null }); });
    }).catch(function () { finish({ status: 0, data: null }); });  // no link
  };

  // ---------- run bar (§21.6) ----------
  // st: editing, checking, ready, mistakes, starting, running, stepping, ended; other = another device's program
  function runBarStart() {
    return { st: "editing", prog: 0, ok: false, step: false, again: false, mine: false, other: false,
             line: 0, round: 0, end: null, msg: "", act: null };
  }

  var NACK_TEXT = {  // the rest use the bridge's own message
    BUSY: "The tank is busy (another program may be running) - try again.",
    NOT_OWNER: "Someone else is driving the tank."
  };

  function copy(s) { var o = {}; Object.keys(s).forEach(function (k) { o[k] = s[k]; }); o.act = null; return o; }

  function canRun(s) { return (s.st === "ready" || (s.st === "ended" && s.ok)) && !s.other && s.prog > 0; }
  function locked(s) { return s.st === "starting" || s.st === "running" || s.st === "stepping"; }
  function ours(s) { return s.mine || s.st === "starting"; }

  // (state, event) -> new state; state.act = what the page must do now: "run", "check", "step", "stop" or null
  function runBar(s0, ev) {
    var s = copy(s0);
    switch (ev.e) {
      case "edit": if (!locked(s)) { s.st = "editing"; s.prog = 0; s.ok = false; s.msg = ""; } break;
      case "checking": if (!locked(s)) s.st = "checking"; break;
      case "checked":
        if (locked(s) && !s.again) break;
        s.prog = ev.prog; s.ok = ev.ok; s.st = ev.ok ? "ready" : "mistakes";
        if (s.again) { s.again = s.ok; if (s.ok && !s.other) { s.st = "starting"; s.act = "run"; } else s.again = false; }
        break;
      case "run":
        if (!canRun(s)) break;
        s.st = "starting"; s.step = !!ev.step; s.again = false; s.end = null; s.msg = ""; s.line = 0; s.act = "run";
        break;
      case "ack": if (s.st === "starting") { s.st = "running"; s.mine = true; s.again = false; } break;
      case "nack":
        if (s.st !== "starting") break;
        if (ev.reason === "CHECK_AGAIN" && !s.again) { s.again = true; s.st = "checking"; s.act = "check"; break; }
        s.st = s.ok ? "ready" : "mistakes"; s.again = false; s.msg = NACK_TEXT[ev.reason] || ev.message || ev.reason;
        break;
      case "started": case "pose": if (!ours(s)) s.other = true; break;
      case "line":  // sent at each step-mode pause (the line changed) and with each move, light or sound
        if (!ours(s)) { s.other = true; break; }
        if (s.mine) s.st = s.step && ev.line !== s.line ? "stepping" : "running";
        s.line = ev.line; s.round = ev.round;
        break;
      case "step": if (s.st === "stepping") { s.st = "running"; s.act = "step"; } break;
      case "stop": s.act = "stop"; break;
      case "ended":
        if (!s.mine) { s.other = false; break; }
        s.st = "ended"; s.mine = false; s.end = { reason: ev.reason, line: ev.line, msg: ev.msg };
        break;
      case "hidden": case "closed":  // the bridge ends a program whose page went away (7046, bridge R11)
        if (s.mine || s.st === "starting") {
          s.st = "ended"; s.mine = false;
          s.end = { reason: 7046, line: s.line, msg: "Stopped: the page was hidden or the link dropped." };
        }
        s.other = false;
        break;
      default: break;
    }
    return s;
  }

  function endText(end) {
    if (!end) return "";
    return end.reason === "finished" ? "Done" : "Ended (" + end.reason + ")" + (end.line ? " on line " + end.line : "") + ": " + end.msg;
  }

  return {
    wordIndex: wordIndex, ownWords: ownWords, colour: colour, insertWord: insertWord, Drafts: Drafts,
    fitView: fitView, toScreen: toScreen, plannedRuns: plannedRuns, CheckScheduler: CheckScheduler, RequestQueue: RequestQueue,
    runBarStart: runBarStart, runBar: runBar, canRun: canRun, locked: locked, endText: endText, NACK_TEXT: NACK_TEXT
  };
})();
if (typeof module !== "undefined") module.exports = LogoCore;
