/* DriveModel (bridge LLD R31, design-notes §201, §210): the drive page's safety rules, no screen code, tested in node.
   Put into the drive page at <!--CQ:DRIVE_JS--> by pack_web.py, as logo.js is into /logo. */
var DriveModel = (function () {
  "use strict";
  // §201: hiding or closing the page sends the automatic STOP only when this page might be driving - a stick held or the
  // drive timer running, or no one else known to be driving; a page that only watches another device, a program or
  // the IR remote leaves their drive alone (the red STOP button still stops everything, §72).
  function mightDrive(s) {
    if (s.driving) return true;
    return !s.ownerKnown || s.owner === "none" || (s.owner === "human" && s.ownerClient === s.you);
  }
  // R6: the driver's STOP or a drive 0 / 0 is exactly what frees the Uno's stall / fault latch
  function releasesLatch(cmd) { return cmd.t === "stop" || (cmd.t === "drive" && cmd.v === 0 && cmd.w === 0); }
  return { mightDrive: mightDrive, releasesLatch: releasesLatch };
})();
if (typeof module !== "undefined") module.exports = DriveModel;
