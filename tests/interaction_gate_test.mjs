import assert from "node:assert/strict";
import {
  HELM_ID,
  SHIP_CORE_ID,
} from "../content/behavior_packs/bedrock_aeronautics_bp/scripts/ship_scan.js";
import {
  isAeronauticsControlBlock,
  shouldQueueAeronauticsInteraction,
} from "../content/behavior_packs/bedrock_aeronautics_bp/scripts/interaction_gate.js";

assert.equal(isAeronauticsControlBlock(SHIP_CORE_ID), true);
assert.equal(isAeronauticsControlBlock(HELM_ID), true);
assert.equal(isAeronauticsControlBlock("minecraft:stone"), false);
assert.equal(
  shouldQueueAeronauticsInteraction(SHIP_CORE_ID, true, false),
  true
);
assert.equal(shouldQueueAeronauticsInteraction(HELM_ID, true, false), true);
assert.equal(shouldQueueAeronauticsInteraction(HELM_ID, false, false), false);
assert.equal(shouldQueueAeronauticsInteraction(HELM_ID, true, true), false);

let queued = false;
assert.equal(
  shouldQueueAeronauticsInteraction(SHIP_CORE_ID, true, queued),
  true
);
queued = true;
assert.equal(
  shouldQueueAeronauticsInteraction(SHIP_CORE_ID, true, queued),
  false,
  "a duplicate event from the same press must not run twice"
);
queued = false;
assert.equal(
  shouldQueueAeronauticsInteraction(SHIP_CORE_ID, true, queued),
  true,
  "a later tap must be accepted after the queued interaction finishes"
);

console.log(
  "flight controls interaction gate passed; core=true; helm=true; " +
    "hold=false; duplicate=false; reentry=true"
);
