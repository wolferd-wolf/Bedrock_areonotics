import { HELM_ID, SHIP_CORE_ID } from "./ship_scan.js";

export function isAeronauticsControlBlock(blockTypeId) {
  return blockTypeId === SHIP_CORE_ID || blockTypeId === HELM_ID;
}

export function shouldQueueAeronauticsInteraction(
  blockTypeId,
  isFirstEvent,
  isAlreadyQueued
) {
  return (
    isAeronauticsControlBlock(blockTypeId) &&
    isFirstEvent === true &&
    isAlreadyQueued === false
  );
}

export const shouldQueueCoreInteraction = shouldQueueAeronauticsInteraction;
