// ── AnniAudio mixer GUI entry point ──
import * as api from './lib/api.js'
import { state, setStatus, setData, setAux, hold, getGroup, getOutput } from './lib/state.js'
import { initRender, render } from './ui/render.js'
import { addGroupModal, addOutputModal, addInputModal } from './ui/modals.js'
import { clamp, formatPercent, holdKey } from './lib/util.js'

const portInput = document.getElementById('port')
const btnReconnect = document.getElementById('btn-reconnect')
const btnAddOutput = document.getElementById('btn-add-output')
const btnAddGroup = document.getElementById('btn-add-group')
const btnAddOutputRail = document.getElementById('btn-add-output-rail')
const btnAddGroupRail = document.getElementById('btn-add-group-rail')
const btnAddInput = document.getElementById('btn-add-input')

let currentPort = parseInt(portInput.value, 10) || 8850
let refreshInterval = null

function onError(err) {
  console.error('[gui]', err)
  setStatus('error')
  render()
}

async function refreshAux() {
  try {
    const [eps, apps, scs] = await Promise.allSettled([
      api.fetchEndpoints(currentPort),
      api.fetchApplications(currentPort),
      api.fetchScenes(currentPort)
    ])
    if (eps.status === 'fulfilled') setAux('endpoints', eps.value)
    if (apps.status === 'fulfilled') setAux('applications', apps.value)
    if (scs.status === 'fulfilled') setAux('scenes', scs.value)
    render()
  } catch (err) {
    console.error('[aux]', err)
  }
}

function start(port) {
  currentPort = port
  if (refreshInterval) clearInterval(refreshInterval)
  api.disconnect()
  api.connect(port, onState, onStatus)
  setStatus('connecting')
  render()
  refreshAux()
  refreshInterval = setInterval(refreshAux, 3000)
}

function onState(data) {
  setData(data)
  render()
}

function onStatus(status) {
  setStatus(status)
  render()
}

async function apiCall(method, path, body) {
  try {
    return await api.api(currentPort, method, path, body)
  } catch (err) {
    onError(err)
    throw err
  }
}

// ── Event handlers ──
function onInput(e) {
  const t = e.target
  const action = t.dataset.action
  if (!action) return

  if (action === 'group-volume') {
    const val = parseFloat(t.value)
    t.nextElementSibling.textContent = formatPercent(val)
    hold(holdKey('group', t.dataset.groupId, 'volume'), val, 400)
    return
  }

  if (action === 'output-master') {
    const val = parseFloat(t.value)
    t.nextElementSibling.textContent = formatPercent(val)
    hold(holdKey('output', t.dataset.outputName, 'master'), val, 400)
    return
  }

  if (action === 'group-send') {
    const val = parseFloat(t.value)
    t.nextElementSibling.textContent = formatPercent(val)
    hold(holdKey('group', t.dataset.groupId, 'send', t.dataset.outputName), val, 400)
    return
  }

  if (action === 'input-azimuth') {
    hold(holdKey('input', t.dataset.inputId, 'azimuth'), parseFloat(t.value), 1000)
    return
  }

  if (action === 'input-elevation') {
    hold(holdKey('input', t.dataset.inputId, 'elevation'), parseFloat(t.value), 1000)
    return
  }

  if (action === 'input-name' || action === 'group-name') {
    const key = action === 'input-name'
      ? holdKey('input', t.dataset.inputId, 'name')
      : holdKey('group', t.dataset.groupId, 'name')
    hold(key, t.value, 1000)
  }
}

async function onChange(e) {
  const t = e.target
  const action = t.dataset.action
  if (!action) return

  const inputId = parseInt(t.dataset.inputId, 10)
  const groupId = parseInt(t.dataset.groupId, 10)

  try {
    if (action === 'group-volume') {
      const val = clamp(parseFloat(t.value), 0, 200)
      hold(holdKey('group', groupId, 'volume'), val, 500)
      await apiCall('PATCH', `/api/groups/${groupId}`, { volume: val })
      return
    }

    if (action === 'group-send') {
      const output = t.dataset.outputName
      const val = clamp(parseFloat(t.value), 0, 200)
      hold(holdKey('group', groupId, 'send', output), val, 500)
      await apiCall('PATCH', `/api/groups/${groupId}`, { outputGains: { [output]: val } })
      return
    }

    if (action === 'output-master') {
      const name = t.dataset.outputName
      const val = clamp(parseFloat(t.value), 0, 200)
      hold(holdKey('output', name, 'master'), val, 500)
      await apiCall('POST', '/api/outputs/master', { name, volume: val })
      return
    }

    if (action === 'input-name') {
      const val = t.value.trim()
      if (!val) return
      hold(holdKey('input', inputId, 'name'), val, 500)
      await apiCall('PATCH', `/api/inputs/${inputId}`, { name: val })
      return
    }

    if (action === 'group-name') {
      const val = t.value.trim()
      if (!val) return
      hold(holdKey('group', groupId, 'name'), val, 500)
      await apiCall('PATCH', `/api/groups/${groupId}`, { name: val })
      return
    }

    if (action === 'input-azimuth' || action === 'input-elevation') {
      // Direction is sent by the Aim button; no-op on blur.
      return
    }

    if (action === 'group-add-input') {
      const id = parseInt(t.value, 10)
      t.value = ''
      if (!id) return
      const g = getGroup(groupId)
      if (!g) return
      const ids = [...(g.inputIds || []), id]
      await apiCall('PATCH', `/api/groups/${groupId}`, { inputIds: ids })
      return
    }

    if (action === 'group-add-output') {
      const name = t.value
      t.value = ''
      if (!name) return
      const g = getGroup(groupId)
      if (!g) return
      const ids = [...(g.outputIds || []), name]
      await apiCall('PATCH', `/api/groups/${groupId}`, { outputIds: ids })
      return
    }
  } catch (err) {
    onError(err)
  }
}

async function onClick(e) {
  const t = e.target.closest('[data-action]')
  if (!t) return

  const action = t.dataset.action
  const inputId = parseInt(t.dataset.inputId, 10)
  const groupId = parseInt(t.dataset.groupId, 10)
  const outputName = t.dataset.outputName

  try {
    if (action === 'input-denoise') {
      const checked = t.checked
      hold(holdKey('input', inputId, 'denoise'), checked, 500)
      await apiCall('PATCH', `/api/inputs/${inputId}`, { denoise: checked })
      return
    }

    if (action === 'input-eq') {
      const checked = t.checked
      const eq = checked ? 'voice' : ''
      hold(holdKey('input', inputId, 'eq'), eq, 500)
      await apiCall('PATCH', `/api/inputs/${inputId}`, { eqPreset: eq })
      return
    }

    if (action === 'input-spatial') {
      const checked = t.checked
      hold(holdKey('input', inputId, 'spatial'), checked, 500)
      await apiCall('PATCH', `/api/inputs/${inputId}`, { spatial: checked })
      return
    }

    if (action === 'input-aim') {
      const azInput = document.querySelector(`input[data-action="input-azimuth"][data-input-id="${inputId}"]`)
      const elInput = document.querySelector(`input[data-action="input-elevation"][data-input-id="${inputId}"]`)
      const az = azInput ? parseFloat(azInput.value) : 0
      const el = elInput ? parseFloat(elInput.value) : 0
      hold(holdKey('input', inputId, 'azimuth'), az, 500)
      hold(holdKey('input', inputId, 'elevation'), el, 500)
      await apiCall('POST', `/api/inputs/${inputId}/direction`, { azimuth: az, elevation: el })
      return
    }

    if (action === 'input-delete') {
      if (!confirm('Delete this input?')) return
      await apiCall('DELETE', `/api/inputs/${inputId}`)
      return
    }

    if (action === 'group-mute') {
      const g = getGroup(groupId)
      if (!g) return
      const muted = !g.muted
      hold(holdKey('group', groupId, 'muted'), muted, 500)
      await apiCall('PATCH', `/api/groups/${groupId}`, { muted })
      return
    }

    if (action === 'group-remove-input') {
      const removeId = parseInt(t.dataset.inputId, 10)
      const g = getGroup(groupId)
      if (!g) return
      const ids = (g.inputIds || []).filter(id => id !== removeId)
      await apiCall('PATCH', `/api/groups/${groupId}`, { inputIds: ids })
      return
    }

    if (action === 'group-remove-output') {
      const removeName = t.dataset.outputName
      const g = getGroup(groupId)
      if (!g) return
      const ids = (g.outputIds || []).filter(n => n !== removeName)
      await apiCall('PATCH', `/api/groups/${groupId}`, { outputIds: ids })
      return
    }

    if (action === 'group-delete') {
      if (!confirm('Delete this group?')) return
      await apiCall('DELETE', `/api/groups/${groupId}`)
      return
    }

    if (action === 'output-mute') {
      const o = getOutput(outputName)
      if (!o) return
      const muted = !o.muted
      hold(holdKey('output', outputName, 'muted'), muted, 500)
      await apiCall('POST', '/api/outputs/master', { name: outputName, muted })
      return
    }

    if (action === 'output-delete') {
      if (!confirm('Remove this output?')) return
      await apiCall('DELETE', '/api/outputs', { name: outputName })
      return
    }

    if (action === 'add-output') {
      openAddOutput()
      return
    }

    if (action === 'add-group') {
      openAddGroup()
      return
    }

    if (action === 'add-input') {
      openAddInput()
      return
    }
  } catch (err) {
    onError(err)
  }
}

function openAddGroup() {
  addGroupModal(async (body) => {
    try {
      await apiCall('POST', '/api/groups', body)
    } catch (err) {
      onError(err)
    }
  })
}

function openAddOutput() {
  addOutputModal(state.endpoints || [], async (body) => {
    try {
      await apiCall('POST', '/api/outputs', body)
    } catch (err) {
      onError(err)
    }
  })
}

function openAddInput() {
  addInputModal(state.endpoints || [], state.applications || [], async (body) => {
    try {
      await apiCall('POST', '/api/inputs', body)
    } catch (err) {
      onError(err)
    }
  })
}

// ── Top-level buttons don't have data-action, so wire directly ──
function init() {
  initRender()

  document.body.addEventListener('input', onInput)
  document.body.addEventListener('change', onChange)
  document.body.addEventListener('click', onClick)

  portInput.addEventListener('change', () => {
    start(parseInt(portInput.value, 10) || 8850)
  })

  btnReconnect.addEventListener('click', () => {
    start(parseInt(portInput.value, 10) || 8850)
  })

  btnAddOutput.addEventListener('click', openAddOutput)
  btnAddGroup.addEventListener('click', openAddGroup)
  btnAddOutputRail.addEventListener('click', openAddOutput)
  btnAddGroupRail.addEventListener('click', openAddGroup)
  btnAddInput.addEventListener('click', openAddInput)

  start(currentPort)
}

init()
