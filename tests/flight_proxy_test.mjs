import assert from "node:assert/strict";
import {
  FLIGHT_PROXY_TARGET_ALTITUDE_METERS,
  createFlightProxyState,
  hasFlightProxyLiftAuthority,
  requestFlightProxyReturn,
  stepFlightProxy,
} from "../content/behavior_packs/bedrock_aeronautics_bp/scripts/flight_proxy.js";

assert.equal(hasFlightProxyLiftAuthority(1000, 1), true);
assert.equal(hasFlightProxyLiftAuthority(10000, 1), false);
assert.throws(() => createFlightProxyState(10000, 1), RangeError);

let state = createFlightProxyState(1000, 1);
for (let step = 0; step < 800 && state.phase !== "hovering"; step += 1) {
  state = stepFlightProxy(state);
}
assert.equal(state.phase, "hovering");
assert.ok(
  Math.abs(state.altitudeMeters - FLIGHT_PROXY_TARGET_ALTITUDE_METERS) <= 0.05
);
assert.ok(Math.abs(state.velocityMetersPerSecond) <= 0.08);

const beforeReturnStep = state.simulationStep;
state = requestFlightProxyReturn(state);
for (let step = 0; step < 800 && state.phase !== "idle"; step += 1) {
  state = stepFlightProxy(state);
}
assert.equal(state.phase, "idle");
assert.equal(state.altitudeMeters, 0);
assert.equal(state.velocityMetersPerSecond, 0);
assert.ok(state.simulationStep > beforeReturnStep);

console.log(
  "anchored flight proxy passed; lift=true; hover=4m; return=landed; fixed_step=20hz"
);
