import { system, world } from "@minecraft/server";
import {
  AERO_ENGINE_ID,
  HELM_ID,
  SHIP_CORE_ID,
  scanConnectedBlocks,
} from "./ship_scan.js";
import { shouldQueueCoreInteraction } from "./interaction_gate.js";

const PREVIEW_DURATION_TICKS = 240;
const PREVIEW_REFRESH_TICKS = 12;
const PREVIEW_UPDATE_TICKS = 4;
const MAX_DISTANCE_SQUARED = 24 * 24;
const MAX_SURFACE_PARTICLES = 180;
const MAX_SPECIAL_COMPONENTS = 24;
const MAX_BOUNDARY_PARTICLES = 220;

const PARTICLES = Object.freeze({
  valid: "aeronautics:assembly_cyan",
  invalid: "aeronautics:assembly_red",
  helm: "aeronautics:assembly_amber",
  engine: "aeronautics:assembly_orange",
});

const sessions = new Map();
const queuedCoreInteractions = new Set();

function samePosition(left, right) {
  return left.x === right.x && left.y === right.y && left.z === right.z;
}

function readBlockSnapshot(dimension, position) {
  try {
    const block = dimension.getBlock(position);
    if (block === undefined) return undefined;
    return {
      typeId: block.typeId,
      isAir: block.isAir,
      isLiquid: block.isLiquid,
    };
  } catch {
    return undefined;
  }
}

function scan(dimension, corePosition) {
  return scanConnectedBlocks(
    corePosition,
    (position) => readBlockSnapshot(dimension, position)
  );
}

function spawnParticle(dimension, effect, location) {
  try {
    dimension.spawnParticle(effect, location);
  } catch {
    // Preview rendering is best-effort at unloaded world edges.
  }
}

function emitSegment(dimension, effect, start, end, step) {
  const dx = end.x - start.x;
  const dy = end.y - start.y;
  const dz = end.z - start.z;
  const length = Math.sqrt(dx * dx + dy * dy + dz * dz);
  const segments = Math.max(1, Math.ceil(length / step));
  for (let index = 0; index <= segments; index += 1) {
    const amount = index / segments;
    spawnParticle(dimension, effect, {
      x: start.x + dx * amount,
      y: start.y + dy * amount,
      z: start.z + dz * amount,
    });
  }
}

function renderBoundary(session, effect) {
  const { minimum, maximum } = session.snapshot.bounds;
  const low = {
    x: minimum.x - 0.03,
    y: minimum.y - 0.03,
    z: minimum.z - 0.03,
  };
  const high = {
    x: maximum.x + 1.03,
    y: maximum.y + 1.03,
    z: maximum.z + 1.03,
  };
  const sizeX = high.x - low.x;
  const sizeY = high.y - low.y;
  const sizeZ = high.z - low.z;
  const totalEdgeLength = 4 * (sizeX + sizeY + sizeZ);
  const step = Math.max(0.5, totalEdgeLength / MAX_BOUNDARY_PARTICLES);
  const { dimension } = session;

  for (const y of [low.y, high.y]) {
    for (const z of [low.z, high.z]) {
      emitSegment(
        dimension,
        effect,
        { x: low.x, y, z },
        { x: high.x, y, z },
        step
      );
    }
  }
  for (const x of [low.x, high.x]) {
    for (const z of [low.z, high.z]) {
      emitSegment(
        dimension,
        effect,
        { x, y: low.y, z },
        { x, y: high.y, z },
        step
      );
    }
  }
  for (const x of [low.x, high.x]) {
    for (const y of [low.y, high.y]) {
      emitSegment(
        dimension,
        effect,
        { x, y, z: low.z },
        { x, y, z: high.z },
        step
      );
    }
  }
}

function renderExposedFaces(session, effect) {
  const faces = session.snapshot.exposedFaces;
  const stride = Math.max(1, Math.ceil(faces.length / MAX_SURFACE_PARTICLES));
  for (let index = 0; index < faces.length; index += stride) {
    spawnParticle(session.dimension, effect, faces[index]);
  }
}

function renderComponentHalo(dimension, effect, position) {
  const center = {
    x: position.x + 0.5,
    y: position.y + 0.5,
    z: position.z + 0.5,
  };
  spawnParticle(dimension, effect, { x: center.x, y: center.y + 0.56, z: center.z });
  spawnParticle(dimension, effect, { x: center.x + 0.42, y: center.y, z: center.z });
  spawnParticle(dimension, effect, { x: center.x - 0.42, y: center.y, z: center.z });
  spawnParticle(dimension, effect, { x: center.x, y: center.y, z: center.z + 0.42 });
  spawnParticle(dimension, effect, { x: center.x, y: center.y, z: center.z - 0.42 });
}

function renderSpecialComponents(session) {
  let rendered = 0;
  for (const block of session.snapshot.blocks) {
    let effect;
    if (block.typeId === HELM_ID) effect = PARTICLES.helm;
    if (block.typeId === AERO_ENGINE_ID) effect = PARTICLES.engine;
    if (block.typeId === SHIP_CORE_ID && !samePosition(block.position, session.corePosition)) {
      effect = PARTICLES.invalid;
    }
    if (effect === undefined) continue;
    renderComponentHalo(session.dimension, effect, block.position);
    rendered += 1;
    if (rendered >= MAX_SPECIAL_COMPONENTS) break;
  }
}

function renderPreview(session) {
  if (session.snapshot.blocks.length === 0) return;
  const effect = session.snapshot.ready ? PARTICLES.valid : PARTICLES.invalid;
  renderBoundary(session, effect);
  renderExposedFaces(session, effect);
  renderSpecialComponents(session);
}

function dimensionsText(snapshot) {
  return `${snapshot.dimensions.x}×${snapshot.dimensions.y}×${snapshot.dimensions.z}`;
}

function summaryText(snapshot) {
  const mass = Math.round(snapshot.totalMassKilograms);
  if (!snapshot.ready) {
    return `§cASSEMBLY INVALID §7| §f${snapshot.statusMessage} §7| §f${snapshot.blocks.length} blocks`;
  }
  return (
    `§bASSEMBLY PREVIEW §7| §f${snapshot.blocks.length} blocks §7| ` +
    `§f${mass} kg §7| §6${snapshot.engineCount} engine(s) §7| ` +
    "§eTap core: confirm §7| §eSneak-tap: cancel"
  );
}

function showPreviewStarted(player, snapshot) {
  try {
    const title = snapshot.ready ? "§bShip detected" : "§cInvalid ship";
    player.onScreenDisplay.setTitle(title, {
      fadeInDuration: 2,
      stayDuration: 45,
      fadeOutDuration: 8,
      subtitle: snapshot.ready
        ? `${snapshot.blocks.length} blocks • ${dimensionsText(snapshot)}`
        : snapshot.statusMessage,
    });
    player.sendMessage(
      snapshot.ready
        ? `§b[Bedrock Aeronautics] §fPreview: ${snapshot.blocks.length} blocks, ` +
            `${Math.round(snapshot.totalMassKilograms)} kg, ` +
            `${snapshot.helmCount} helm(s), ${snapshot.engineCount} engine(s), ` +
            `${dimensionsText(snapshot)}. Tap the core again to confirm.`
        : `§c[Bedrock Aeronautics] ${snapshot.statusMessage}.`
    );
    player.onScreenDisplay.setActionBar(summaryText(snapshot));
  } catch {
    // The player may have left during the scan.
  }
}

function cancelPreview(playerId, message) {
  const session = sessions.get(playerId);
  if (session === undefined) return;
  sessions.delete(playerId);
  try {
    session.player.onScreenDisplay.setActionBar(message);
  } catch {
    // The player may already be gone.
  }
}

function persistConfirmation(session) {
  const snapshot = session.snapshot;
  const record = {
    schema: 1,
    dimension: session.dimension.id,
    core: session.corePosition,
    blocks: snapshot.blocks.length,
    massKilograms: Math.round(snapshot.totalMassKilograms),
    helms: snapshot.helmCount,
    engines: snapshot.engineCount,
    dimensions: snapshot.dimensions,
  };
  try {
    world.setDynamicProperty(
      "aeronautics:confirmed_assembly",
      JSON.stringify(record)
    );
  } catch {
    // Confirmation remains visible even if persistence is unavailable.
  }
}

function confirmPreview(session) {
  persistConfirmation(session);
  sessions.delete(session.player.id);
  try {
    session.player.onScreenDisplay.setTitle("§aAssembly confirmed", {
      fadeInDuration: 2,
      stayDuration: 50,
      fadeOutDuration: 8,
      subtitle: `${session.snapshot.blocks.length} blocks ready for movement integration`,
    });
    session.player.sendMessage(
      "§a[Bedrock Aeronautics] Assembly confirmed. " +
        "This version saves the core and summary; physical movement comes next."
    );
  } catch {
    // The player may have left while confirming.
  }
}

function beginPreview(player, coreBlock) {
  const corePosition = { ...coreBlock.location };
  const snapshot = scan(coreBlock.dimension, corePosition);
  const session = {
    player,
    dimension: coreBlock.dimension,
    corePosition,
    snapshot,
    expiresTick: system.currentTick + PREVIEW_DURATION_TICKS,
    nextRenderTick: system.currentTick,
  };
  sessions.set(player.id, session);
  showPreviewStarted(player, snapshot);
  renderPreview(session);
  session.nextRenderTick = system.currentTick + PREVIEW_REFRESH_TICKS;
}

function processCoreInteraction(player, block) {
  const existing = sessions.get(player.id);

  if (player.isSneaking) {
    cancelPreview(player.id, "§7Assembly preview canceled");
    return;
  }

  if (
    existing !== undefined &&
    existing.dimension.id === block.dimension.id &&
    samePosition(existing.corePosition, block.location)
  ) {
    if (existing.snapshot.ready) {
      confirmPreview(existing);
    } else {
      beginPreview(player, block);
    }
    return;
  }

  beginPreview(player, block);
}

function queueCoreInteraction(event) {
  if (event.block.typeId !== SHIP_CORE_ID) return;

  // A custom solid block has no vanilla use action. Capture the press before
  // Bedrock decides whether it was a successful vanilla interaction.
  event.cancel = true;

  const playerId = event.player.id;
  if (
    !shouldQueueCoreInteraction(
      event.block.typeId,
      event.isFirstEvent,
      queuedCoreInteractions.has(playerId)
    )
  ) {
    return;
  }

  const player = event.player;
  const dimension = event.block.dimension;
  const corePosition = { ...event.block.location };
  queuedCoreInteractions.add(playerId);

  // Before-event callbacks use restricted execution. Re-read the block and do
  // scanning, UI, particles, and persistence safely on the next server tick.
  system.run(() => {
    queuedCoreInteractions.delete(playerId);
    try {
      const coreBlock = dimension.getBlock(corePosition);
      if (coreBlock === undefined || coreBlock.typeId !== SHIP_CORE_ID) return;
      processCoreInteraction(player, coreBlock);
    } catch {
      // The block may have been removed or the player may have left.
    }
  });
}

function updatePreviews() {
  for (const [playerId, session] of sessions) {
    try {
      if (system.currentTick >= session.expiresTick) {
        cancelPreview(playerId, "§7Assembly preview expired");
        continue;
      }
      if (session.player.dimension.id !== session.dimension.id) {
        cancelPreview(playerId, "§7Assembly preview canceled: dimension changed");
        continue;
      }

      const location = session.player.location;
      const dx = location.x - (session.corePosition.x + 0.5);
      const dy = location.y - (session.corePosition.y + 0.5);
      const dz = location.z - (session.corePosition.z + 0.5);
      if (dx * dx + dy * dy + dz * dz > MAX_DISTANCE_SQUARED) {
        cancelPreview(playerId, "§7Assembly preview canceled: too far from core");
        continue;
      }

      session.player.onScreenDisplay.setActionBar(summaryText(session.snapshot));
      if (system.currentTick >= session.nextRenderTick) {
        renderPreview(session);
        session.nextRenderTick = system.currentTick + PREVIEW_REFRESH_TICKS;
      }
    } catch {
      sessions.delete(playerId);
    }
  }
}

world.beforeEvents.playerInteractWithBlock.subscribe(queueCoreInteraction);
system.runInterval(updatePreviews, PREVIEW_UPDATE_TICKS);
