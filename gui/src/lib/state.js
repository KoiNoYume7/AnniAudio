// ── Shared state, helpers, and optimistic holds ──

export const state = {
  port: 8850,
  status: 'disconnected',
  data: null,
  endpoints: [],
  applications: [],
  scenes: [],
  listeners: [],
  holds: new Map()
}

export function addListener(fn) {
  state.listeners.push(fn)
}

export function removeListener(fn) {
  state.listeners = state.listeners.filter(f => f !== fn)
}

function notify() {
  for (const fn of state.listeners) fn(state)
}

export function setStatus(status) {
  if (state.status === status) return
  state.status = status
  notify()
}

export function setData(data) {
  state.data = data
  notify()
}

export function setAux(key, value) {
  state[key] = value
  notify()
}

// Hold a key to a local value for `ms` so incoming WebSocket values do not
// overwrite faders/inputs the user is currently interacting with.
export function hold(key, value, ms = 500) {
  state.holds.set(key, { until: performance.now() + ms, value })
}

export function isHeld(key) {
  const h = state.holds.get(key)
  if (!h) return undefined
  if (performance.now() > h.until) {
    state.holds.delete(key)
    return undefined
  }
  return h.value
}

export function getHeld(key, fallback) {
  const v = isHeld(key)
  return v !== undefined ? v : fallback
}

export function clearExpiredHolds() {
  const now = performance.now()
  for (const [key, h] of state.holds) {
    if (now > h.until) state.holds.delete(key)
  }
}

export function getInput(id) {
  if (!state.data) return null
  return state.data.inputs.find(i => i.id === id) || null
}

export function getGroup(id) {
  if (!state.data) return null
  return state.data.groups.find(g => g.id === id) || null
}

export function getOutput(name) {
  if (!state.data) return null
  return state.data.outputs.find(o => o.name === name) || null
}
