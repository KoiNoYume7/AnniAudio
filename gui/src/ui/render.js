// ── Render the matrix into the DOM ──
import { state } from '../lib/state.js'
import { buildInputs, buildGroups, buildOutputs } from './panels.js'

const els = {}

export function initRender() {
  els.status = document.getElementById('status')
  els.inputsList = document.getElementById('inputs-list')
  els.groupsList = document.getElementById('groups-list')
  els.outputsList = document.getElementById('outputs-list')
  els.inputCount = document.getElementById('input-count')
  els.groupCount = document.getElementById('group-count')
  els.outputCount = document.getElementById('output-count')
}

export function render() {
  if (!els.status) return
  els.status.textContent = state.status
  els.status.className = 'status ' + state.status

  if (!state.data) {
    els.inputsList.innerHTML = '<div class="empty">Waiting for mixer...</div>'
    els.groupsList.innerHTML = '<div class="empty">Waiting for mixer...</div>'
    els.outputsList.innerHTML = '<div class="empty">Waiting for mixer...</div>'
    updateCounts()
    return
  }

  els.inputsList.innerHTML = buildInputs()
  els.groupsList.innerHTML = buildGroups()
  els.outputsList.innerHTML = buildOutputs()
  updateCounts()
}

function updateCounts() {
  if (!state.data) return
  els.inputCount.textContent = (state.data.inputs || []).length
  els.groupCount.textContent = (state.data.groups || []).length
  els.outputCount.textContent = (state.data.outputs || []).length
}
