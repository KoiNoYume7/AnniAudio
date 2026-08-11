// ── Shared utilities for the mixer GUI ──

export function holdKey(...parts) {
  return parts.join('-')
}

export function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
}

export function clamp(v, min, max) {
  return Math.min(Math.max(v, min), max)
}

export function formatPercent(v) {
  return Math.round(v) + '%'
}

// Peak/RMS in the API is linear (0..1, typically). Convert to a dB-ish meter
// percentage where -60 dB is 0% and 0 dB is 100%.
export function meterPct(v) {
  if (!v || v <= 0.00001) return 0
  const db = 20 * Math.log10(Math.min(v, 1))
  return clamp((db + 60) / 60 * 100, 0, 100)
}

// Linear fader position (0..1) to dB-like perceived value, then to percent.
// This makes the fader feel natural: the bottom half is quiet, the top half
// is the usable loud range. Not used everywhere; the API expects percent.
export function percentToFaderPos(pct) {
  return pct
}

export function debounce(fn, ms) {
  let t
  return (...args) => {
    clearTimeout(t)
    t = setTimeout(() => fn(...args), ms)
  }
}
