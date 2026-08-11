// ── AnniAudio mixer GUI — Electron main process ──
const { app, BrowserWindow } = require('electron')
const { pathToFileURL } = require('node:url')
const path = require('node:path')

const DEV_MODE = process.env.NODE_ENV === 'development'

let mainWindow

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1400,
    height: 900,
    minWidth: 900,
    minHeight: 600,
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      webSecurity: false
    },
    title: 'AnniAudio Mixer',
    show: false
  })

  const indexPath = path.join(__dirname, 'index.html')
  mainWindow.loadURL(pathToFileURL(indexPath).toString())

  mainWindow.once('ready-to-show', () => mainWindow.show())

  mainWindow.webContents.on('console-message', (e, level, message, line, sourceId) => {
    const prefix = ['debug', 'info', 'warn', 'error'][level] || 'log'
    console.log(`[renderer:${prefix}]`, message)
  })

  if (DEV_MODE) mainWindow.webContents.openDevTools()
}

app.whenReady().then(createWindow)

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit()
})

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow()
})
