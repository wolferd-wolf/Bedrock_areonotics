import { system, world } from "@minecraft/server";
import {
  AERO_ENGINE_ID,
  HELM_ID,
  SHIP_CORE_ID,
  scanConnectedBlocks,
} from "./ship_scan.js";
import {
  isAeronauticsControlBlock,
  shouldQueueAeronauticsInteraction,
} from "./interaction_gate.js";
import {
  createFlightProxyState,
  hasFlightProxyLiftAuthority,
  requestFlightProxyReturn,
  stepFlightProxy,
} from "./flight_proxy.js";

const PREVIEW_DURATION_TICKS = 240;
const PREVIEW_REFRESH_TICKS = 12;
const PREVIEW_UPDATE_TICKS = 4;
const FLIGHT_RENDER_TICKS = 4;
const MAX_DISTANCE_SQUARED = 24 * 24;
const MAX_SURFACE_PARTICLES = 180;
const MAX_SPECIAL_COMPONENTS = 24;
const MAX_BOUNDARY_PARTICLES = 220;

const PARTICLES = Object.freeze({
  valid: "aeronautics:assembly_cyan",
  invalid: "aeronautics:assembly_red",
  helm: "aeronautics:assembly_amber",
  engine: "aeronautics:assembly_orange",
  flight: "aeronautics:flight_blue",
});

const sessions = new Map();
const confirmedShips = new Map();
const activeFlights = new Map();
const queuedControlInteractions = new Set();

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

function renderBoundary(session, effect, verticalOffset = 0) {
  const { minimum, maximum } = session.snapshot.bounds;
  const low = {
    x: minimum.x - 0.03,
    y: minimum.y - 0.03 + verticalOffset,
    z: minimum.z - 0.03,
  };
  const high = {
    x: maximum.x + 1.03,
    y: maximum.y + 1.03 + verticalOffset,
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

function renderExposedFaces(session, effect, verticalOffset = 0) {
  const faces = session.snapshot.exposedFaces;
  const stride = Math.max(1, Math.ceil(faces.length / MAX_SURFACE_PARTICLES));
  for (let index = 0; index < faces.length; index += stride) {
    const face = faces[index];
    spawnParticle(session.dimension, effect, {
      x: face.x,
      y: face.y + verticalOffset,
      z: face.z,
    });
  }
}

function renderComponentHalo(dimension, effect, position, verticalOffset = 0) {
  const center = {
    x: position.x + 0.5,
    y: position.y + 0.5 + verticalOffset,
    z: position.z + 0.5,
  };
  spawnParticle(dimension, effect, { x: center.x, y: center.y + 0.56, z: center.z });
  spawnParticle(dimension, effect, { x: center.x + 0.42, y: center.y, z: center.z });
  spawnParticle(dimension, effect, { x: center.x - 0.42, y: center.y, z: center.z });
  spawnParticle(dimension, effect, { x: center.x, y: center.y, z: center.z + 0.42 });
  spawnParticle(dimension, effect, { x: center.x, y: center.y, z: center.z - 0.42 });
}

function renderSpecialComponents(session, verticalOffset = 0) {
  let rendered = 0;
  for (const block of session.snapshot.blocks) {
    let effect;
    if (block.typeId === HELM_ID) effect = PARTICLES.helm;
    if (block.typeId === AERO_ENGINE_ID) effect = PARTICLES.engine;
    if (block.typeId === SHIP_CORE_ID && !samePosition(block.position, session.corePosition)) {
      effect = PARTICLES.invalid;
    }
    if (effect === undefined) continue;
    renderComponentHalo(
      session.dimension,
      effect,
      block.position,
      verticalOffset
    );
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

function renderFlightProxy(flight) {
  const verticalOffset = flight.state.altitudeMeters;
  renderBoundary(flight, PARTICLES.flight, verticalOffset);
  renderExposedFaces(flight, PARTICLES.flight, verticalOffset);
  renderSpecialComponents(flight, verticalOffset);
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
    schema: 2,
    dimension: session.dimension.id,
    core: session.corePosition,
    blocks: snapshot.blocks.length,
    massKilograms: Math.round(snapshot.totalMassKilograms),
    helms: snapshot.helmCount,
    engines: snapshot.engineCount,
    dimensions: snapshot.dimensions,
    capability: "anchored_vertical_flight_proxy_v1",
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
  confirmedShips.set(session.player.id, {
    player: session.player,
    dimension: session.dimension,
    corePosition: session.corePosition,
    snapshot: session.snapshot,
  });
  try {
    session.player.onScreenDisplay.setTitle("§aAssembly confirmed", {
      fadeInDuration: 2,
      stayDuration: 50,
      fadeOutDuration: 8,
      subtitle: `${session.snapshot.blocks.length} blocks ready for lift test`,
    });
    session.player.sendMessage(
      "§a[Bedrock Aeronautics] Assembly confirmed. " +
        "Tap a connected Helm to engage the anchored 4 m lift proxy."
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
  if (activeFlights.has(player.id)) {
    player.onScreenDisplay.setActionBar(
      "§9FLIGHT PROXY ACTIVE §7| §eSneak to return"
    );
    return;
  }
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

function processHelmInteraction(player, block) {
  const active = activeFlights.get(player.id);
  if (active !== undefined) {
    if (player.isSneaking) {
      active.state = requestFlightProxyReturn(active.state);
    }
    player.onScreenDisplay.setActionBar(
      "§9FLIGHT PROXY ACTIVE §7| §eSneak to return"
    );
    return;
  }

  const confirmed = confirmedShips.get(player.id);
  const belongsToConfirmedShip =
    confirmed !== undefined &&
    confirmed.dimension.id === block.dimension.id &&
    confirmed.snapshot.blocks.some(
      (candidate) =>
        candidate.typeId === HELM_ID &&
        samePosition(candidate.position, block.location)
    );
  if (!belongsToConfirmedShip) {
    player.sendMessage(
      "§e[Bedrock Aeronautics] Confirm this ship at its Ship Core before using the Helm."
    );
    return;
  }
  if (player.isSneaking) {
    player.onScreenDisplay.setActionBar(
      "§7Stand normally and tap the Helm to engage"
    );
    return;
  }
  if (
    !hasFlightProxyLiftAuthority(
      confirmed.snapshot.totalMassKilograms,
      confirmed.snapshot.engineCount
    )
  ) {
    player.sendMessage(
      "§c[Bedrock Aeronautics] Insufficient Aero Engine thrust for this assembly mass."
    );
    return;
  }

  const flight = {
    ...confirmed,
    helmPosition: { ...block.location },
    state: createFlightProxyState(
      confirmed.snapshot.totalMassKilograms,
      confirmed.snapshot.engineCount
    ),
    nextRenderTick: system.currentTick,
  };
  activeFlights.set(player.id, flight);
  try {
    player.onScreenDisplay.setTitle("§9Lift proxy engaged", {
      fadeInDuration: 2,
      stayDuration: 50,
      fadeOutDuration: 8,
      subtitle: "Pilot anchored • target altitude 4 m • sneak to return",
    });
    player.sendMessage(
      "§9[Bedrock Aeronautics] Fixed-step flight proxy active. " +
        "The blue assembly outline is the moving reference frame. Sneak to land."
    );
  } catch {
    activeFlights.delete(player.id);
  }
}

function processControlInteraction(player, block) {
  if (block.typeId === SHIP_CORE_ID) {
    processCoreInteraction(player, block);
  } else if (block.typeId === HELM_ID) {
    processHelmInteraction(player, block);
  }
}

function queueAeronauticsInteraction(event) {
  if (!isAeronauticsControlBlock(event.block.typeId)) return;

  // A custom solid block has no vanilla use action. Capture the press before
  // Bedrock decides whether it was a successful vanilla interaction.
  event.cancel = true;

  const playerId = event.player.id;
  if (
    !shouldQueueAeronauticsInteraction(
      event.block.typeId,
      event.isFirstEvent,
      queuedControlInteractions.has(playerId)
    )
  ) {
    return;
  }

  const player = event.player;
  const dimension = event.block.dimension;
  const blockPosition = { ...event.block.location };
  const blockTypeId = event.block.typeId;
  queuedControlInteractions.add(playerId);

  // Before-event callbacks use restricted execution. Re-read the block and do
  // scanning, UI, particles, and persistence safely on the next server tick.
  system.run(() => {
    queuedControlInteractions.delete(playerId);
    try {
      const controlBlock = dimension.getBlock(blockPosition);
      if (
        controlBlock === undefined ||
        controlBlock.typeId !== blockTypeId
      ) {
        return;
      }
      processControlInteraction(player, controlBlock);
    } catch {
      // The block may have been removed or the player may have left.
    }
  });
}

function flightStatusText(flight) {
  const altitude = flight.state.altitudeMeters.toFixed(2);
  const velocity = flight.state.velocityMetersPerSecond.toFixed(2);
  const phase = flight.state.phase.toUpperCase();
  return (
    `§9FLIGHT PROXY ${phase} §7| §f${altitude} m §7| ` +
    `§f${velocity} m/s §7| §eSneak: return`
  );
}

function updateFlights() {
  for (const [playerId, flight] of activeFlights) {
    try {
      if (flight.player.dimension.id !== flight.dimension.id) {
        activeFlights.delete(playerId);
        continue;
      }
      if (
        flight.player.isSneaking &&
        flight.state.phase !== "returning"
      ) {
        flight.state = requestFlightProxyReturn(flight.state);
        flight.player.sendMessage(
          "§e[Bedrock Aeronautics] Return commanded. Holding pilot at the Helm until landing."
        );
      }

      flight.state = stepFlightProxy(flight.state);
      flight.player.teleport(
        {
          x: flight.helmPosition.x + 0.5,
          y:
            flight.helmPosition.y +
            1.05 +
            flight.state.altitudeMeters,
          z: flight.helmPosition.z + 0.5,
        },
        {
          dimension: flight.dimension,
          checkForBlocks: false,
          keepVelocity: false,
        }
      );
      flight.player.onScreenDisplay.setActionBar(flightStatusText(flight));

      if (system.currentTick >= flight.nextRenderTick) {
        renderFlightProxy(flight);
        flight.nextRenderTick = system.currentTick + FLIGHT_RENDER_TICKS;
      }

      if (flight.state.phase === "idle") {
        activeFlights.delete(playerId);
        flight.player.onScreenDisplay.setTitle("§aFlight proxy landed", {
          fadeInDuration: 2,
          stayDuration: 40,
          fadeOutDuration: 8,
          subtitle: "Anchored lift/hover/return cycle complete",
        });
        flight.player.sendMessage(
          "§a[Bedrock Aeronautics] Flight proxy landed. Tap the Helm to run it again."
        );
      }
    } catch {
      activeFlights.delete(playerId);
    }
  }
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

world.beforeEvents.playerInteractWithBlock.subscribe(queueAeronauticsInteraction);
system.runInterval(updatePreviews, PREVIEW_UPDATE_TICKS);
system.runInterval(updateFlights, 1);
