import assert from "node:assert/strict";
import { SHIP_CORE_ID } from "../content/behavior_packs/bedrock_aeronautics_bp/scripts/ship_scan.js";
import { shouldQueueCoreInteraction } from "../content/behavior_packs/bedrock_aeronautics_bp/scripts/interaction_gate.js";

assert.equal(shouldQueueCoreInteraction(SHIP_CORE_ID, true, false), true);
assert.equal(shouldQueueCoreInteraction(SHIP_CORE_ID, false, false), false);
assert.equal(shouldQueueCoreInteraction(SHIP_CORE_ID, true, true), false);
assert.equal(shouldQueueCoreInteraction("minecraft:stone", true, false), false);

let queued = false;
assert.equal(shouldQueueCoreInteraction(SHIP_CORE_ID, true, queued), true);
queued = true;
assert.equal(
  shouldQueueCoreInteraction(SHIP_CORE_ID, true, queued),
  false,
  "a duplicate event from the same press must not run twice"
);
queued = false;
assert.equal(
  shouldQueueCoreInteraction(SHIP_CORE_ID, true, queued),
  true,
  "a later tap must be accepted after the queued interaction finishes"
);

console.log(
  "Ship Core interaction gate passed; hold=false; duplicate=false; reentry=true"
);
