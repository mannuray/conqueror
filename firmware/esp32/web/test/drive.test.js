// DriveModel (bridge LLD R31, design-notes §201 / §210): the drive page's safety rules, tested here for the first time
// outside a browser (hide_stop_check.js covered them by hand). Run: node --test firmware/esp32/web/test/drive.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const D = require("../drive.js");

const known = (owner, ownerClient) => ({ driving: false, ownerKnown: true, owner, ownerClient, you: "phone-1" });

test("§201: hiding or closing stops the tank only when this page might be driving", () => {
  assert.equal(D.mightDrive(known("human", "phone-2")), false);   // another phone drives
  assert.equal(D.mightDrive(known("human", "phone-15")), false);  // a program from another client
  assert.equal(D.mightDrive(known("ir", "")), false);             // the IR remote
  assert.equal(D.mightDrive(known("human", "phone-1")), true);    // this page
  assert.equal(D.mightDrive(known("none", "")), true);            // nobody
  assert.equal(D.mightDrive({ driving: false, ownerKnown: false, owner: "none", ownerClient: "", you: "" }), true);  // not known yet
  assert.equal(D.mightDrive(Object.assign(known("human", "phone-2"), { driving: true })), true);  // a stick held / the drive timer
});

test("R6: the driver's STOP or a drive 0 / 0 frees the Uno's stall / fault latch, nothing else", () => {
  assert.equal(D.releasesLatch({ t: "stop" }), true);
  assert.equal(D.releasesLatch({ t: "stop", id: 3 }), true);
  assert.equal(D.releasesLatch({ t: "drive", v: 0, w: 0 }), true);
  assert.equal(D.releasesLatch({ t: "drive", v: 0.1, w: 0 }), false);
  assert.equal(D.releasesLatch({ t: "drive", v: 0, w: -0.2 }), false);
  assert.equal(D.releasesLatch({ t: "ping", t0: 1 }), false);
});
