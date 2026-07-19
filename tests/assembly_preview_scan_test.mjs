import assert from "node:assert/strict";
import {
  AERO_ENGINE_ID,
  HELM_ID,
  SHIP_CORE_ID,
  positionKey,
  scanConnectedBlocks,
} from "../content/behavior_packs/bedrock_aeronautics_bp/scripts/ship_scan.js";

function position(x, y, z) {
  return { x, y, z };
}

function block(typeId) {
  return { typeId, isAir: false, isLiquid: false };
}

const air = Object.freeze({
  typeId: "minecraft:air",
  isAir: true,
  isLiquid: false,
});

function reader(entries, undefinedKeys = new Set()) {
  const blocks = new Map(
    entries.map(([location, value]) => [positionKey(location), value])
  );
  return (location) => {
    const key = positionKey(location);
    if (undefinedKeys.has(key)) return undefined;
    return blocks.get(key) ?? air;
  };
}

const core = position(10, 70, -4);
const validBlocks = [
  [core, block(SHIP_CORE_ID)],
  [position(11, 70, -4), block(HELM_ID)],
  [position(9, 70, -4), block(AERO_ENGINE_ID)],
  [position(10, 71, -4), block("minecraft:iron_block")],
  [position(100, 100, 100), block("minecraft:gold_block")],
];

const assembled = scanConnectedBlocks(core, reader(validBlocks));
assert.equal(assembled.status, "ready");
assert.equal(assembled.ready, true);
assert.equal(assembled.blocks.length, 4, "disconnected block must be ignored");
assert.equal(assembled.totalMassKilograms, 15);
assert.equal(assembled.helmCount, 1);
assert.equal(assembled.engineCount, 1);
assert.deepEqual(assembled.dimensions, { x: 3, y: 2, z: 1 });
assert.ok(assembled.exposedFaces.length > 0);
assert.ok(Math.abs(assembled.centerOfMassLocalMeters.x - 5.5 / 15) < 1e-9);
assert.ok(Math.abs(assembled.centerOfMassLocalMeters.y - 8.5 / 15) < 1e-9);
assert.ok(Math.abs(assembled.centerOfMassLocalMeters.z - 0.5) < 1e-9);

const reversed = scanConnectedBlocks(core, reader([...validBlocks].reverse()));
assert.deepEqual(reversed.blocks, assembled.blocks, "scan output must be deterministic");

const missingEngine = scanConnectedBlocks(
  core,
  reader([
    [core, block(SHIP_CORE_ID)],
    [position(11, 70, -4), block(HELM_ID)],
  ])
);
assert.equal(missingEngine.status, "missing_engine");

const twoCores = scanConnectedBlocks(
  core,
  reader([
    [core, block(SHIP_CORE_ID)],
    [position(11, 70, -4), block(SHIP_CORE_ID)],
    [position(10, 71, -4), block(HELM_ID)],
    [position(9, 70, -4), block(AERO_ENGINE_ID)],
  ])
);
assert.equal(twoCores.status, "multiple_connected_cores");

const liquidIgnored = scanConnectedBlocks(
  core,
  reader([
    [core, block(SHIP_CORE_ID)],
    [position(11, 70, -4), block(HELM_ID)],
    [position(9, 70, -4), block(AERO_ENGINE_ID)],
    [position(10, 71, -4), { typeId: "minecraft:water", isAir: false, isLiquid: true }],
  ])
);
assert.equal(liquidIgnored.status, "ready");
assert.equal(liquidIgnored.blocks.length, 3);

const unloaded = scanConnectedBlocks(
  core,
  reader(validBlocks, new Set([positionKey(position(10, 69, -4))]))
);
assert.equal(unloaded.status, "unloaded_boundary");

const longLine = [[core, block(SHIP_CORE_ID)]];
for (let x = 11; x <= 20; x += 1) {
  longLine.push([position(x, 70, -4), block(x === 11 ? HELM_ID : AERO_ENGINE_ID)]);
}
const bounded = scanConnectedBlocks(core, reader(longLine), {
  maximumBlockCount: 2048,
  maximumSpanBlocks: 4,
});
assert.equal(bounded.status, "bounds_limit_exceeded");

const manyBlocks = [[core, block(SHIP_CORE_ID)]];
for (let x = 11; x <= 20; x += 1) {
  manyBlocks.push([position(x, 70, -4), block(x === 11 ? HELM_ID : AERO_ENGINE_ID)]);
}
const countLimited = scanConnectedBlocks(core, reader(manyBlocks), {
  maximumBlockCount: 3,
  maximumSpanBlocks: 64,
});
assert.equal(countLimited.status, "block_limit_exceeded");

console.log(
  "assembly preview scan passed; connected=4; deterministic=true; " +
    "surface_faces=true; limits=true"
);
