// ── Build HTML for the three matrix panels ──
import { state, getGroup, getHeld } from '../lib/state.js'
import { escapeHtml, formatPercent, meterPct, holdKey } from '../lib/util.js'

export function buildInputs() {
  if (!state.data) return '<div class="empty">Not connected</div>'
  const inputs = state.data.inputs || []
  if (!inputs.length) return '<div class="empty">No inputs yet. Add a microphone, app, or loopback.</div>'

  const rows = inputs.map(input => {
    const denoise = getHeld(holdKey('input', input.id, 'denoise'), input.denoise)
    const eqPreset = getHeld(holdKey('input', input.id, 'eq'), input.eqPreset || '')
    const spatial = getHeld(holdKey('input', input.id, 'spatial'), input.spatial)
    const name = getHeld(holdKey('input', input.id, 'name'), input.name)
    const azimuth = getHeld(holdKey('input', input.id, 'azimuth'), input.azimuth)
    const elevation = getHeld(holdKey('input', input.id, 'elevation'), input.elevation)

    const tags = []
    if (denoise) tags.push('<span class="tag on">NS</span>')
    if (eqPreset) tags.push('<span class="tag on">EQ</span>')
    if (spatial) tags.push('<span class="tag on">3D</span>')

    const spatialFields = spatial ? `
      <div class="dsp-row">
        <label>Azimuth <input type="number" data-action="input-azimuth" data-input-id="${input.id}" value="${Math.round(azimuth)}" step="1"></label>
        <label>Elevation <input type="number" data-action="input-elevation" data-input-id="${input.id}" value="${Math.round(elevation)}" step="1"></label>
        <button data-action="input-aim" data-input-id="${input.id}">Aim</button>
      </div>
    ` : ''

    return `
      <div class="card input-card" data-input-id="${input.id}">
        <div class="card-header">
          <input class="card-title" data-action="input-name" data-input-id="${input.id}" value="${escapeHtml(name)}" title="Rename">
          <button class="btn-icon danger" data-action="input-delete" data-input-id="${input.id}" title="Delete input">×</button>
        </div>
        <div class="card-meta">${escapeHtml(input.type)} · ${escapeHtml(input.source)}</div>
        <div class="card-body" style="flex-direction: row; align-items: center; gap: 12px;">
          ${renderMeter(input.peak || 0, input.rms || 0)}
          <div style="flex: 1; display: flex; flex-direction: column; gap: 8px;">
            <div class="dsp-row">
              <label><input type="checkbox" data-action="input-denoise" data-input-id="${input.id}" ${denoise ? 'checked' : ''}> NS</label>
              <label><input type="checkbox" data-action="input-eq" data-input-id="${input.id}" ${eqPreset ? 'checked' : ''}> EQ</label>
              <label><input type="checkbox" data-action="input-spatial" data-input-id="${input.id}" ${spatial ? 'checked' : ''}> 3D</label>
            </div>
            ${spatialFields}
          </div>
        </div>
      </div>
    `
  }).join('')

  return rows
}

export function buildGroups() {
  if (!state.data) return '<div class="empty">Not connected</div>'
  const groups = state.data.groups || []
  if (!groups.length) return '<div class="empty">No groups yet. Add a virtual cable/mix bus.</div>'

  const inputOptions = (state.data.inputs || []).map(i => `<option value="${i.id}">${escapeHtml(i.name)}</option>`).join('')
  const outputOptions = (state.data.outputs || []).map(o => `<option value="${escapeHtml(o.name)}">${escapeHtml(o.name)}</option>`).join('')

  const rows = groups.map(group => {
    const name = getHeld(holdKey('group', group.id, 'name'), group.name)
    const volume = getHeld(holdKey('group', group.id, 'volume'), group.volume)
    const muted = getHeld(holdKey('group', group.id, 'muted'), group.muted)

    const inputRows = (group.inputIds || []).map(inputId => {
      const input = state.data.inputs.find(i => i.id === inputId)
      if (!input) return ''
      return `
        <div class="input-row" data-input-id="${input.id}">
          <span class="name" title="${escapeHtml(input.source)}">${escapeHtml(input.name)}</span>
          <span class="tags">
            ${input.denoise ? '<span class="tag on">NS</span>' : ''}
            ${input.eqPreset ? '<span class="tag on">EQ</span>' : ''}
            ${input.spatial ? '<span class="tag on">3D</span>' : ''}
          </span>
          <button class="btn-icon danger" data-action="group-remove-input" data-group-id="${group.id}" data-input-id="${input.id}" title="Remove from group">×</button>
        </div>
      `
    }).join('')

    const sendRows = (group.outputIds || []).map(outputName => {
      const baseGain = (group.outputGains && group.outputGains[outputName]) || 100
      const gain = getHeld(holdKey('group', group.id, 'send', outputName), baseGain)
      return `
        <div class="send-row" data-output-name="${escapeHtml(outputName)}">
          <span class="send-name">${escapeHtml(outputName)}</span>
          <div class="fader-row">
            <input type="range" min="0" max="200" value="${gain}" data-action="group-send" data-group-id="${group.id}" data-output-name="${escapeHtml(outputName)}">
            <span class="fader-val">${formatPercent(gain)}</span>
          </div>
          <button class="btn-icon danger" data-action="group-remove-output" data-group-id="${group.id}" data-output-name="${escapeHtml(outputName)}" title="Disconnect output">×</button>
        </div>
      `
    }).join('')

    return `
      <div class="card group-card" data-group-id="${group.id}">
        <div class="card-header">
          <input class="card-title" data-action="group-name" data-group-id="${group.id}" value="${escapeHtml(name)}" title="Rename">
          <button class="btn-icon danger" data-action="group-delete" data-group-id="${group.id}" title="Delete group">×</button>
        </div>
        <div class="card-meta" style="display:flex;gap:8px;align-items:center;">
          <span>${escapeHtml(group.color || '#3b82f6')}</span>
          <span>Cable: ${escapeHtml(group.cable || 'none')}</span>
        </div>
        <div class="card-body">
          <div class="fader-row">
            <input type="range" min="0" max="200" value="${volume}" data-action="group-volume" data-group-id="${group.id}">
            <span class="fader-val">${formatPercent(volume)}</span>
            <button class="btn-mute ${muted ? 'muted' : ''}" data-action="group-mute" data-group-id="${group.id}">${muted ? 'Unmute' : 'Mute'}</button>
          </div>
          ${renderMeter(group.peak || 0, group.rms || 0)}
          <div class="section-title" style="margin-top:4px;">Inputs</div>
          ${inputRows || '<div class="empty" style="padding:8px;font-size:11px;">No inputs</div>'}
          <select data-action="group-add-input" data-group-id="${group.id}">
            <option value="">+ add input</option>
            ${inputOptions}
          </select>
          <div class="section-title" style="margin-top:4px;">Sends</div>
          ${sendRows || '<div class="empty" style="padding:8px;font-size:11px;">No outputs</div>'}
          <select data-action="group-add-output" data-group-id="${group.id}">
            <option value="">+ add output send</option>
            ${outputOptions}
          </select>
        </div>
      </div>
    `
  }).join('')

  return rows
}

export function buildOutputs() {
  if (!state.data) return '<div class="empty">Not connected</div>'
  const outputs = state.data.outputs || []
  if (!outputs.length) return '<div class="empty">No outputs yet. Add a render device.</div>'

  const rows = outputs.map(output => {
    const master = getHeld(holdKey('output', output.name, 'master'), output.master)
    const muted = getHeld(holdKey('output', output.name, 'muted'), output.muted)
    const groupNames = (output.groupIds || []).map(id => {
      const g = state.data.groups.find(x => x.id === id)
      return g ? escapeHtml(g.name) : `id:${id}`
    }).join(', ')

    return `
      <div class="card output-card" data-output-name="${escapeHtml(output.name)}">
        <div class="card-header">
          <span class="card-title">${escapeHtml(output.name)}</span>
          <button class="btn-icon danger" data-action="output-delete" data-output-name="${escapeHtml(output.name)}" title="Remove output">×</button>
        </div>
        <div class="card-meta">Groups: ${groupNames || 'none'}</div>
        <div class="card-body">
          <div class="fader-row">
            <input type="range" min="0" max="200" value="${master}" data-action="output-master" data-output-name="${escapeHtml(output.name)}">
            <span class="fader-val">${formatPercent(master)}</span>
            <button class="btn-mute ${muted ? 'muted' : ''}" data-action="output-mute" data-output-name="${escapeHtml(output.name)}">${muted ? 'Unmute' : 'Mute'}</button>
          </div>
          ${renderMeter(output.masterPeak || 0, output.masterRms || 0)}
        </div>
      </div>
    `
  }).join('')

  return rows
}

function renderMeter(peak, rms) {
  const rmsPct = meterPct(rms)
  const peakPct = meterPct(peak)
  return `
    <div class="meter" title="peak ${(peak || 0).toFixed(4)}">
      <div class="meter-fill" style="height: ${rmsPct}%"></div>
      <div class="meter-peak" style="bottom: ${peakPct}%"></div>
    </div>
  `
}
