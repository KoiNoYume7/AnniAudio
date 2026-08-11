// ── API client and state poller for the mixer ──
const API_HOST = '127.0.0.1'

let pollTimer = null

export function apiUrl(port, path) {
  return `http://${API_HOST}:${port}${path}`
}

export async function api(port, method, path, body = null) {
  const url = apiUrl(port, path)
  const opts = { method, headers: {} }
  if (body) {
    opts.headers['Content-Type'] = 'application/json'
    opts.body = JSON.stringify(body)
  }
  const res = await fetch(url, opts)
  if (!res.ok) {
    let msg = res.statusText
    try {
      const j = await res.json()
      if (j.error) msg = j.error
    } catch {}
    throw new Error(msg)
  }
  return res.status === 204 ? null : res.json()
}

export function connect(port, onMessage, onStatus) {
  disconnect()
  onStatus('connecting')

  let connected = false
  let failures = 0

  async function tick() {
    try {
      const state = await api(port, 'GET', '/api/state')
      failures = 0
      if (!connected) {
        connected = true
        onStatus('connected')
      }
      onMessage(state)
    } catch (err) {
      failures++
      console.log('[poll] retry', failures)
      if (connected && failures > 2) {
        connected = false
        onStatus('disconnected')
      } else if (!connected && failures > 4) {
        onStatus('error')
      }
    }
  }

  tick()
  pollTimer = setInterval(tick, 500)
}

export function disconnect() {
  if (!pollTimer) return
  clearInterval(pollTimer)
  pollTimer = null
}

export async function fetchEndpoints(port) {
  const data = await api(port, 'GET', '/api/endpoints')
  return data.endpoints || []
}

export async function fetchApplications(port) {
  const data = await api(port, 'GET', '/api/applications')
  return data.applications || []
}

export async function fetchScenes(port) {
  const data = await api(port, 'GET', '/api/scenes')
  return data.scenes || []
}
