const epoch = (value) => {
  const milliseconds = value instanceof Date ? value.getTime() : Date.parse(value);
  if (!Number.isFinite(milliseconds)) throw new Error('Planner returned an invalid time');
  return Math.floor(milliseconds / 1000);
};

// connections() returns scheduled Date values plus delays in seconds. The watch
// receives expected epoch seconds; it must not add the delay a second time.
export function normalizeConnection(connection) {
  const rides = connection.legs.filter((leg) => leg.type === 'ride');
  if (!rides.length) throw new Error('Planner returned a journey without a train');
  const first = rides[0];
  const delay = Number(connection.departureDelay ?? first.from.delay ?? 0);
  const arrivalDelay = Number(connection.arrivalDelay ?? rides.at(-1).to.delay ?? 0);
  if (!Number.isFinite(delay) || !Number.isFinite(arrivalDelay)) throw new Error('Planner returned an invalid delay');
  // Layovers: where each train but the last ends, i.e. the stations you change at.
  const via = rides.slice(0, -1).map((ride) => {
    const station = ride.to.station;
    return String(station?.name ?? station ?? '').trim();
  }).filter(Boolean);
  return {
    via,
    departure: epoch(connection.departure) + delay,
    arrival: epoch(connection.arrival) + arrivalDelay,
    delay,
    platform: String(first.from.platform ?? '?'),
    train: String(first.vehicle?.name ?? 'Train'),
    transfers: Math.max(0, Number(connection.transfers ?? rides.length - 1)),
    canceled: Boolean(connection.canceled || rides.some((ride) => ride.canceled || ride.from.canceled || ride.to.canceled))
  };
}

export function parseRoute(route) {
  const from = String(route.from || '').trim();
  const to = String(route.to || '').trim();
  if (!from || !to || from.length > 100 || to.length > 100) {
    throw Object.assign(new Error('Enter both station names (up to 100 characters)'), { status: 400 });
  }
  if (from.toLowerCase() === to.toLowerCase()) {
    throw Object.assign(new Error('Choose two different stations'), { status: 400 });
  }
  return { from, to, limit: 6, language: 'nl', maxWalkMeters: 0 };
}
