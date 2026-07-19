export const FLIGHT_PROXY_FIXED_STEP_SECONDS = 0.05;
export const FLIGHT_PROXY_TARGET_ALTITUDE_METERS = 4.0;
export const FLIGHT_PROXY_GRAVITY_METERS_PER_SECOND_SQUARED = 9.81;
export const FLIGHT_PROXY_ENGINE_THRUST_NEWTONS = 25000.0;

const POSITION_GAIN = 4.5;
const VELOCITY_GAIN = 4.0;
const MAX_UPWARD_ACCELERATION = 6.0;
const HOVER_POSITION_TOLERANCE = 0.05;
const HOVER_VELOCITY_TOLERANCE = 0.08;

function clamp(value, minimum, maximum) {
  return Math.min(maximum, Math.max(minimum, value));
}

export function hasFlightProxyLiftAuthority(massKilograms, engineCount) {
  return (
    Number.isFinite(massKilograms) &&
    massKilograms > 0 &&
    Number.isInteger(engineCount) &&
    engineCount > 0 &&
    engineCount * FLIGHT_PROXY_ENGINE_THRUST_NEWTONS >
      massKilograms *
        FLIGHT_PROXY_GRAVITY_METERS_PER_SECOND_SQUARED *
        1.05
  );
}

export function createFlightProxyState(massKilograms, engineCount) {
  if (!hasFlightProxyLiftAuthority(massKilograms, engineCount)) {
    throw new RangeError("confirmed assembly does not have enough lift authority");
  }
  return Object.freeze({
    phase: "ascending",
    massKilograms,
    engineCount,
    altitudeMeters: 0,
    velocityMetersPerSecond: 0,
    simulationStep: 0,
  });
}

export function requestFlightProxyReturn(state) {
  if (state.phase === "idle" || state.phase === "returning") return state;
  return Object.freeze({ ...state, phase: "returning" });
}

export function stepFlightProxy(state) {
  if (state.phase === "idle") return state;

  const targetAltitude =
    state.phase === "returning" ? 0 : FLIGHT_PROXY_TARGET_ALTITUDE_METERS;
  const positionError = targetAltitude - state.altitudeMeters;
  const commandedThrustNewtons =
    state.massKilograms *
    (FLIGHT_PROXY_GRAVITY_METERS_PER_SECOND_SQUARED +
      POSITION_GAIN * positionError -
      VELOCITY_GAIN * state.velocityMetersPerSecond);
  const maximumThrustNewtons =
    state.engineCount * FLIGHT_PROXY_ENGINE_THRUST_NEWTONS;
  const thrustNewtons = clamp(
    commandedThrustNewtons,
    0,
    maximumThrustNewtons
  );
  const accelerationMetersPerSecondSquared = clamp(
    thrustNewtons / state.massKilograms -
      FLIGHT_PROXY_GRAVITY_METERS_PER_SECOND_SQUARED,
    -FLIGHT_PROXY_GRAVITY_METERS_PER_SECOND_SQUARED,
    MAX_UPWARD_ACCELERATION
  );

  let velocityMetersPerSecond =
    state.velocityMetersPerSecond +
    accelerationMetersPerSecondSquared * FLIGHT_PROXY_FIXED_STEP_SECONDS;
  let altitudeMeters =
    state.altitudeMeters +
    velocityMetersPerSecond * FLIGHT_PROXY_FIXED_STEP_SECONDS;
  let phase = state.phase;

  if (phase === "returning" && altitudeMeters <= 0) {
    altitudeMeters = 0;
    velocityMetersPerSecond = 0;
    phase = "idle";
  } else if (
    phase !== "returning" &&
    Math.abs(FLIGHT_PROXY_TARGET_ALTITUDE_METERS - altitudeMeters) <=
      HOVER_POSITION_TOLERANCE &&
    Math.abs(velocityMetersPerSecond) <= HOVER_VELOCITY_TOLERANCE
  ) {
    phase = "hovering";
  }

  return Object.freeze({
    ...state,
    phase,
    altitudeMeters,
    velocityMetersPerSecond,
    simulationStep: state.simulationStep + 1,
  });
}
