// ── Modal helpers for add/rename/picker dialogs ──
import { escapeHtml } from '../lib/util.js'

const overlay = document.getElementById('modal')
const titleEl = document.getElementById('modal-title')
const contentEl = document.getElementById('modal-content')
const cancelBtn = document.getElementById('modal-cancel')
const okBtn = document.getElementById('modal-ok')

let currentOk = null
let currentPicker = null
let currentTypeChange = null

export function closeModal() {
  overlay.classList.remove('open')
  currentOk = null
  currentPicker = null
  currentTypeChange = null
}

function openModal(title, html, onOk = null) {
  titleEl.textContent = title
  contentEl.innerHTML = html
  currentOk = onOk
  currentPicker = null
  currentTypeChange = null
  overlay.classList.add('open')
}

cancelBtn.addEventListener('click', closeModal)

okBtn.addEventListener('click', () => {
  if (currentOk) currentOk()
})

overlay.addEventListener('click', (e) => {
  if (e.target === overlay) closeModal()
})

contentEl.addEventListener('click', (e) => {
  const row = e.target.closest('.picker-row')
  if (row && currentPicker) {
    currentPicker(row.dataset.name)
    closeModal()
    return
  }
})

contentEl.addEventListener('change', (e) => {
  if (e.target.id === 'modal-input-type' && currentTypeChange) {
    currentTypeChange(e.target.value)
  }
})

export function addGroupModal(onAdd) {
  openModal('Add group', `
    <div class="modal-row">
      <label>Name</label>
      <input id="modal-group-name" type="text" value="New cable" placeholder="e.g. Music">
    </div>
    <div class="modal-row">
      <label>Colour (optional)</label>
      <input id="modal-group-color" type="text" value="#3b82f6" placeholder="#3b82f6">
    </div>
    <div class="modal-row">
      <label>Cable render endpoint (optional)</label>
      <input id="modal-group-cable" type="text" value="" placeholder="e.g. CABLE (VB-Audio Virtual Cable)">
    </div>
  `, () => {
    const name = document.getElementById('modal-group-name').value.trim()
    if (!name) return
    onAdd({
      name,
      color: document.getElementById('modal-group-color').value.trim() || '#3b82f6',
      cable: document.getElementById('modal-group-cable').value.trim()
    })
    closeModal()
  })
}

export function addOutputModal(endpoints, onAdd) {
  const renderers = endpoints.filter(e => e.isRender).map(e => `
    <div class="picker-row" data-name="${escapeHtml(e.name)}">
      <span>${escapeHtml(e.name)}</span>
      <span class="sub">${e.isDefault ? 'default' : ''}</span>
    </div>
  `).join('')

  openModal('Add output', `
    <div class="picker-list" id="modal-output-list">
      ${renderers || '<div class="empty">No render endpoints found</div>'}
    </div>
  `)

  currentPicker = (name) => onAdd({ name })
}

export function addInputModal(endpoints, applications, onAdd) {
  const deviceOpts = endpoints.map(e => {
    const kind = e.isRender ? '[loopback] ' : ''
    return `<option value="${escapeHtml(e.name)}">${kind}${escapeHtml(e.name)}</option>`
  }).join('')
  const appOpts = applications.map(a => `<option value="${a.processId}">${escapeHtml(a.displayName || a.name)} (${escapeHtml(a.name)})</option>`).join('')

  openModal('Add input', `
    <div class="modal-row">
      <label>Name</label>
      <input id="modal-input-name" type="text" value="" placeholder="e.g. Microphone">
    </div>
    <div class="modal-row">
      <label>Type</label>
      <select id="modal-input-type">
        <option value="device">Device</option>
        <option value="application">Application</option>
      </select>
    </div>
    <div class="modal-row" id="modal-device-row">
      <label>Source device</label>
      <select id="modal-input-device">${deviceOpts}</select>
    </div>
    <div class="modal-row" id="modal-app-row" style="display:none">
      <label>Application</label>
      <select id="modal-input-app">${appOpts}</select>
    </div>
    <div class="modal-row">
      <label><input type="checkbox" id="modal-input-denoise"> RNNoise suppression</label>
    </div>
    <div class="modal-row">
      <label><input type="checkbox" id="modal-input-eq"> Voice EQ preset</label>
    </div>
    <div class="modal-row">
      <label><input type="checkbox" id="modal-input-spatial"> HRTF spatial audio</label>
    </div>
  `, () => {
    const type = document.getElementById('modal-input-type').value
    const name = document.getElementById('modal-input-name').value.trim()
    if (!name) {
      document.getElementById('modal-input-name').focus()
      return
    }
    const source = type === 'device'
      ? document.getElementById('modal-input-device').value
      : document.getElementById('modal-input-app').value
    if (!source) {
      alert('Select a source')
      return
    }
    onAdd({
      name,
      type,
      source,
      denoise: document.getElementById('modal-input-denoise').checked,
      eqPreset: document.getElementById('modal-input-eq').checked ? 'voice' : '',
      spatial: document.getElementById('modal-input-spatial').checked
    })
    closeModal()
  })

  currentTypeChange = (type) => {
    const isDevice = type === 'device'
    document.getElementById('modal-device-row').style.display = isDevice ? '' : 'none'
    document.getElementById('modal-app-row').style.display = isDevice ? 'none' : ''
  }
}
