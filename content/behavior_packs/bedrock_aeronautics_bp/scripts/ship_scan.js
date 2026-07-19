export const SHIP_CORE_ID = "aeronautics:ship_core";
export const HELM_ID = "aeronautics:helm";
export const AERO_ENGINE_ID = "aeronautics:aero_engine";

export const DEFAULT_SCAN_CONFIG = Object.freeze({
  maximumBlockCount: 2048,
  maximumSpanBlocks: 64,
});

const NEIGHBORS = Object.freeze([
  Object.freeze({ x: 1, y: 0, z: 0 }),
  Object.freeze({ x: -1, y: 0, z: 0 }),
  Object.freeze({ x: 0, y: 1, z: 0 }),
  Object.freeze({ x: 0, y: -1, z: 0 }),
  Object.freeze({ x: 0, y: 0, z: 1 }),
  Object.freeze({ x: 0, y: 0, z: -1 }),
]);

export function positionKey(position) {
  return `${position.x},${position.y},${position.z}`;
}

function comparePositions(left, right) {
  return left.x - right.x || left.y - right.y || left.z - right.z;
}

function add(left, right) {
  return {
    x: left.x + right.x,
    y: left.y + right.y,
    z: left.z + right.z,
  };
}

function isCandidate(block) {
  return block !== undefined && !block.isAir && !block.isLiquid;
}

function blockMass(typeId) {
  switch (typeId) {
    case SHIP_CORE_ID:
      return 8;
    case HELM_ID:
      return 2;
    case AERO_ENGINE_ID:
      return 4;
    default:
      return 1;
  }
}

function span(bounds) {
  return {
    x: bounds.maximum.x - bounds.minimum.x + 1,
    y: bounds.maximum.y - bounds.minimum.y + 1,
    z: bounds.maximum.z - bounds.minimum.z + 1,
  };
}

function statusMessage(status) {
  switch (status) {
    case "ready":
      return "Ready — tap the Ship Core again to confirm";
    case "selected_block_is_not_core":
      return "The selected block is not a Ship Core";
    case "block_limit_exceeded":
      return "More than 2,048 connected blocks or the ship touches terrain";
    case "bounds_limit_exceeded":
      return "The connected structure exceeds 64 blocks on one axis";
    case "unloaded_boundary":
      return "A connected edge reaches an unloaded chunk";
    case "multiple_connected_cores":
      return "Only one connected Ship Core is allowed";
    case "missing_helm":
      return "Add at least one connected Helm";
    case "missing_engine":
      return "Add at least one connected Aero Engine";
    default:
      return "Assembly preview failed";
  }
}

function finalize(start, blocks, unloadedBoundary, forcedStatus) {
  const sortedBlocks = [...blocks].sort((left, right) =>
    comparePositions(left.position, right.position)
  );
  const occupied = new Set(sortedBlocks.map((block) => positionKey(block.position)));
  const exposedFaces = [];
  let totalMassKilograms = 0;
  let weightedX = 0;
  let weightedY = 0;
  let weightedZ = 0;
  let coreCount = 0;
  let helmCount = 0;
  let engineCount = 0;

  const bounds = {
    minimum: { x: start.x, y: start.y, z: start.z },
    maximum: { x: start.x, y: start.y, z: start.z },
  };

  for (const block of sortedBlocks) {
    const { position, typeId, massKilograms } = block;
    bounds.minimum.x = Math.min(bounds.minimum.x, position.x);
    bounds.minimum.y = Math.min(bounds.minimum.y, position.y);
    bounds.minimum.z = Math.min(bounds.minimum.z, position.z);
    bounds.maximum.x = Math.max(bounds.maximum.x, position.x);
    bounds.maximum.y = Math.max(bounds.maximum.y, position.y);
    bounds.maximum.z = Math.max(bounds.maximum.z, position.z);

    totalMassKilograms += massKilograms;
    weightedX += (position.x - start.x + 0.5) * massKilograms;
    weightedY += (position.y - start.y + 0.5) * massKilograms;
    weightedZ += (position.z - start.z + 0.5) * massKilograms;

    if (typeId === SHIP_CORE_ID) coreCount += 1;
    if (typeId === HELM_ID) helmCount += 1;
    if (typeId === AERO_ENGINE_ID) engineCount += 1;

    for (const normal of NEIGHBORS) {
      if (!occupied.has(positionKey(add(position, normal)))) {
        exposedFaces.push({
          x: position.x + 0.5 + normal.x * 0.505,
          y: position.y + 0.5 + normal.y * 0.505,
          z: position.z + 0.5 + normal.z * 0.505,
        });
      }
    }
  }

  let status = forcedStatus;
  if (status === undefined && unloadedBoundary) status = "unloaded_boundary";
  if (status === undefined && coreCount !== 1) status = "multiple_connected_cores";
  if (status === undefined && helmCount === 0) status = "missing_helm";
  if (status === undefined && engineCount === 0) status = "missing_engine";
  if (status === undefined) status = "ready";

  const dimensions = span(bounds);
  const inverseMass = totalMassKilograms > 0 ? 1 / totalMassKilograms : 0;
  return {
    status,
    statusMessage: statusMessage(status),
    ready: status === "ready",
    corePosition: { ...start },
    blocks: sortedBlocks,
    exposedFaces,
    bounds,
    dimensions,
    totalMassKilograms,
    centerOfMassLocalMeters: {
      x: weightedX * inverseMass,
      y: weightedY * inverseMass,
      z: weightedZ * inverseMass,
    },
    coreCount,
    helmCount,
    engineCount,
    unloadedBoundary,
  };
}

export function scanConnectedBlocks(
  start,
  readBlock,
  config = DEFAULT_SCAN_CONFIG
) {
  const startBlock = readBlock(start);
  if (startBlock === undefined || startBlock.typeId !== SHIP_CORE_ID) {
    return finalize(start, [], startBlock === undefined, "selected_block_is_not_core");
  }

  const queue = [{ ...start }];
  const visited = new Set([positionKey(start)]);
  const blocks = [];
  let head = 0;
  let unloadedBoundary = false;
  let forcedStatus;

  const bounds = {
    minimum: { ...start },
    maximum: { ...start },
  };

  while (head < queue.length) {
    const position = queue[head];
    head += 1;
    const block = readBlock(position);
    if (!isCandidate(block)) continue;

    if (blocks.length >= config.maximumBlockCount) {
      forcedStatus = "block_limit_exceeded";
      break;
    }

    const massKilograms = blockMass(block.typeId);
    blocks.push({ position: { ...position }, typeId: block.typeId, massKilograms });

    bounds.minimum.x = Math.min(bounds.minimum.x, position.x);
    bounds.minimum.y = Math.min(bounds.minimum.y, position.y);
    bounds.minimum.z = Math.min(bounds.minimum.z, position.z);
    bounds.maximum.x = Math.max(bounds.maximum.x, position.x);
    bounds.maximum.y = Math.max(bounds.maximum.y, position.y);
    bounds.maximum.z = Math.max(bounds.maximum.z, position.z);
    const dimensions = span(bounds);
    if (
      dimensions.x > config.maximumSpanBlocks ||
      dimensions.y > config.maximumSpanBlocks ||
      dimensions.z > config.maximumSpanBlocks
    ) {
      forcedStatus = "bounds_limit_exceeded";
      break;
    }

    for (const offset of NEIGHBORS) {
      const neighbor = add(position, offset);
      const key = positionKey(neighbor);
      if (visited.has(key)) continue;
      visited.add(key);

      const neighborBlock = readBlock(neighbor);
      if (neighborBlock === undefined) {
        unloadedBoundary = true;
        continue;
      }
      if (isCandidate(neighborBlock)) queue.push(neighbor);
    }
  }

  return finalize(start, blocks, unloadedBoundary, forcedStatus);
}
