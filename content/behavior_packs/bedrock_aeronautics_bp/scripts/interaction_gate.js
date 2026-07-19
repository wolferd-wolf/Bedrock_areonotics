import { SHIP_CORE_ID } from "./ship_scan.js";

export function shouldQueueCoreInteraction(
  blockTypeId,
  isFirstEvent,
  isAlreadyQueued
) {
  return (
    blockTypeId === SHIP_CORE_ID &&
    isFirstEvent === true &&
    isAlreadyQueued === false
  );
}
