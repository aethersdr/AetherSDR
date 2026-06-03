# ulanzistudio-plugin-sdk-node

<p align="start">
   <strong>English</strong> | <a href="./README.zh.md">简体中文</a>
</p>

## Introduction

The ulanzistudio-plugin-sdk encapsulates the WebSocket connection with the UlanziStudio and its related communication events. This simplifies the development process and enables developers to communicate with the UlanziStudio through simple event calls, allowing them to focus more on the development of plugin functions.

> Current version is developed according to the **Ulanzi JS Plugin Development Protocol - V2.1.2**.

For `manifest.json` configuration reference, see **[manifest.md](https://github.com/UlanziTechnology/UlanziDeckPlugin-SDK/blob/main/manifest.md)**.

---

## File Directory

```
ulanzi-api/
├── libs/
│   ├── constants.js   // Frozen event-name constants (Events.*) used throughout the SDK
│   ├── randomPort.js  // Generates a random port for a self-hosted main service and writes ws-port.js for PropertyInspector pages
│   ├── utils.js       // Helper utilities: plugin path, system type detection, JSON parsing, etc.
│   └── ulanziApi.js   // Main SDK class. Encapsulates all UlanziStudio events and WebSocket connection.
├── apiTypes.d.ts      // TypeScript type definitions for IDE autocompletion
└── index.js           // Package entry point — exports UlanziApi, Utils, RandomPort
```

---

## Instructions & Conventions

1. **Main service** (`app.js`) stays connected to the UlanziStudio at all times. It implements the plugin's core logic, receives param changes from actions, and updates icon states.

2. **Action / PropertyInspector** (`inspector.html`) is destroyed when the user switches buttons. Keep it lightweight — only use it to send/receive configuration params.

3. Plugin package naming: `com.ulanzi.{pluginName}.ulanziPlugin`

4. The **main service UUID** must have exactly **4** dot-separated segments:
   `com.ulanzi.ulanzistudio.{pluginName}`

5. An **action UUID** must have **more than 4** segments to be distinguished from the main service:
   `com.ulanzi.ulanzistudio.{pluginName}.{actionName}`

6. When a PropertyInspector page needs to bypass UlanziStudio mediation and connect directly to the plugin's own Node.js main service, use `RandomPort` to generate a random listening port for that service and write it to `ws-port.js` for the PropertyInspector page to read. Plugins that communicate only through UlanziStudio's standard event flow do not need it. See [2. Generate Random Port](#2-generate-random-port).

7. Use `Utils.getPluginPath()` to get the plugin root directory path — it handles differences between local Node and the host's packaged Node environment. See [3. Get Plugin Root Path](#3-get-plugin-root-path).

---

## How to Use

### Special Parameter: `context`

Because the same action can be assigned to multiple keys, the SDK generates a unique **`context`** string per key instance and appends it to every received message.

- **Format:** `uuid + '___' + key + '___' + actionid`
- **Encode:** `$UD.encodeContext(msg)` → returns a context string
- **Decode:** `$UD.decodeContext(context)` → returns `{ uuid, key, actionid }`
- For the `clear` event, `context` is spliced into each item of the `param` array. Iterate over `message.param` to retrieve individual contexts.

---

### 1. Install

```bash
npm install ws
```

Copy the `ulanzi-api` folder into your plugin's runtime directory, then import from it:

```js
import UlanziApi, { Utils, RandomPort } from './ulanzi-api/index.js';
```

---

### 2. Generate Random Port

`RandomPort` is intended for architectures where the PropertyInspector page needs to **connect to the main service by itself**. For example, the main service may start a local WebSocket / HTTP service, and the PropertyInspector page may bypass UlanziStudio mediation to exchange connection state, account information, real-time lists, or other temporary data directly with that service. In this case, call `getPort()` once when the main service starts, then use the returned port to start your own service. `RandomPort` also writes the port to `ws-port.js` in the plugin root directory, so the PropertyInspector HTML page can include that file, read `window.__port`, and connect to the service.

If the PropertyInspector and main service synchronize parameters entirely through UlanziStudio's standard events, you do not need `RandomPort`.

```js
import { RandomPort } from './ulanzi-api/index.js';
import { WebSocketServer } from 'ws';

const randomPort = new RandomPort();
const port = randomPort.getPort(); // generates port and writes ws-port.js

// The main service starts a local service on this port for the PropertyInspector page to connect to.
const wss = new WebSocketServer({ host: '127.0.0.1', port });
wss.on('connection', socket => {
  socket.send(JSON.stringify({ type: 'ready' }));
});
```

In the PropertyInspector HTML, include the generated file before connecting:

```html
<script src="../../ws-port.js"></script>
<script>
  const socket = new WebSocket(`ws://127.0.0.1:${window.__port}`);
</script>
```

Notes:

- `RandomPort` only generates a port and writes `ws-port.js`; it does not create a WebSocket / HTTP service automatically.
- Call `getPort()` once during main service startup, then let the PropertyInspector page connect after your service is listening.
- The default range is the dynamic port range `49152`-`65535`, which helps reduce port conflicts between plugins. To restrict the range, pass `minPort` and `maxPort` to the constructor.

---

### 3. Get Plugin Root Path

`Utils.getPluginPath()` returns the absolute path to the plugin root directory (the folder ending with `ulanziPlugin`). Compatible with Windows and macOS.

```js
import { Utils } from './ulanzi-api/index.js';

const pluginPath = Utils.getPluginPath();
console.log('Plugin root:', pluginPath);

// Example: read a local config file
import { promises as fs } from 'fs';
const config = JSON.parse(await fs.readFile(`${pluginPath}/config.json`, 'utf8'));
```

---

### 4. Connect to UlanziStudio

Connection parameters are read from `process.argv` when launched by the host application:
- `process.argv[2]` → address (default `127.0.0.1`)
- `process.argv[3]` → port (default `3906`)
- `process.argv[4]` → language (default `en`)

```js
import UlanziApi from './ulanzi-api/index.js';

const $UD = new UlanziApi();

// Connect — argv params take precedence over defaults
$UD.connect('com.ulanzi.ulanzistudio.myplugin');

$UD.onConnected(conn => {
  console.log('Connected');
});

$UD.onAdd(message => {
  // Action assigned to a key; message.context is the unique key identifier
  const context = message.context;
});

$UD.onParamFromApp(message => {
  // Host pushed saved params; use message.param
});

$UD.onClear(message => {
  // message.param is an array; context is in each item
  if (message.param) {
    for (const item of message.param) {
      console.log('cleared context:', item.context);
    }
  }
});
```

---

## Receive Events (UlanziStudio → Plugin)

### Connection Events

```js
$UD.onConnected(conn => {})   // WebSocket connected successfully
$UD.onClose(conn => {})       // WebSocket connection closed
$UD.onError(conn => {})       // WebSocket error
```

### Button / Key Events

```js
// Action was added to a key; message.param contains saved settings
$UD.onAdd(message => {})

// Key was triggered (single click confirmed); main entry point for plugin logic
$UD.onRun(message => {})

// Key press started (fires before run; use for long-press detection)
$UD.onKeyDown(message => {})

// Key press released
$UD.onKeyUp(message => {})

// Action active state changed; message.active = true/false
$UD.onSetActive(message => {})

// Action removed from one or more keys; message.param is an array, each item has .context
$UD.onClear(message => {})
```

### Dial / Encoder Events

```js
$UD.onDialDown(message => {})         // Dial pressed
$UD.onDialUp(message => {})           // Dial released
$UD.onDialRotate(message => {})       // Any rotation; message.rotateEvent = 'left' | 'right' | 'hold-left' | 'hold-right'
$UD.onDialRotateLeft(message => {})       // Rotated left (not held)
$UD.onDialRotateRight(message => {})      // Rotated right (not held)
$UD.onDialRotateHoldLeft(message => {})   // Rotated left while pressed
$UD.onDialRotateHoldRight(message => {}) // Rotated right while pressed
```

### Param / Config Events

```js
// Host pushed params to the plugin when a key is configured
$UD.onParamFromApp(message => {})

// Host forwarded params sent by the plugin (paramfromplugin echo)
$UD.onParamFromPlugin(message => {})
```

### Settings Events

```js
// Triggered after getSettings() or setSettings(); message.settings contains saved data
$UD.onDidReceiveSettings(message => {})

// Triggered after getGlobalSettings() or setGlobalSettings()
$UD.onDidReceiveGlobalSettings(message => {})
```

### Cross-Page Communication Events

```js
// Main service: receives data sent by PropertyInspector via sendToPlugin()
$UD.onSendToPlugin(message => {})

// PropertyInspector: receives data sent by main service via sendToPropertyInspector()
$UD.onSendToPropertyInspector(message => {})
```

### Dialog Result

```js
// Result of selectFileDialog() or selectFolderDialog(); message.path is the selected path
$UD.onSelectdialog(message => {})
```

---

## Send Events (Plugin → UlanziStudio)

### Set Button Icon

```js
/**
 * Use a state index defined in manifest.json States array
 * @param {string} context  Required | Unique key for the target button
 * @param {number} state    Required | Index into the States array
 * @param {string} text     Optional | Text to overlay on the icon
 */
$UD.setStateIcon(context, state, text)

/**
 * Use a custom image (base64)
 * @param {string} context  Required
 * @param {string} data     Required | Base64-encoded image (PNG/JPG/SVG)
 * @param {string} text     Optional
 */
$UD.setBaseDataIcon(context, data, text)

/**
 * Use a local image file path
 * @param {string} context  Required
 * @param {string} path     Required | Relative path from plugin root
 * @param {string} text     Optional
 */
$UD.setPathIcon(context, path, text)

/**
 * Use a custom animated GIF (base64)
 * @param {string} context  Required
 * @param {string} gifdata  Required | Base64-encoded GIF data
 * @param {string} text     Optional
 */
$UD.setGifDataIcon(context, gifdata, text)

/**
 * Use a local GIF file path
 * @param {string} context  Required
 * @param {string} gifpath  Required | Relative path from plugin root
 * @param {string} text     Optional
 */
$UD.setGifPathIcon(context, gifpath, text)
```

### Send Parameters

```js
/**
 * Send config params to the host (main service → host → PropertyInspector, or reverse)
 * @param {object} settings  Required
 * @param {string} context   Required when called from main service
 */
$UD.sendParamFromPlugin(settings, context)

/**
 * Main service → PropertyInspector: pass-through data (not saved by host)
 * @param {object} settings  Required
 * @param {string} context   Required | Target action's context
 */
$UD.sendToPropertyInspector(settings, context)

/**
 * PropertyInspector → main service: pass-through data (not saved by host)
 * @param {object} settings  Required
 */
$UD.sendToPlugin(settings)
```

### Settings Persistence

```js
/**
 * Save action-specific settings. Triggers didReceiveSettings on both ends.
 * Note: settings are NOT saved when the action is inactive.
 * @param {object} settings  Required
 * @param {string} context   Required when called from main service
 */
$UD.setSettings(settings, context)

/**
 * Request saved action settings. Response arrives via onDidReceiveSettings.
 * @param {string} context   Required when called from main service
 */
$UD.getSettings(context)

/**
 * Save plugin-wide global settings. Triggers didReceiveGlobalSettings on all connected pages.
 * @param {object} settings  Required
 * @param {string} context   Optional
 */
$UD.setGlobalSettings(settings, context)

/**
 * Request global settings. Response arrives via onDidReceiveGlobalSettings.
 * @param {string} context   Optional
 */
$UD.getGlobalSettings(context)
```

### System Functions

```js
/**
 * Show a toast notification on the UlanziStudio host application
 * @param {string} msg  Required
 */
$UD.toast(msg)

/**
 * Show an error indicator on the button (brief animation)
 * @param {string} context  Required when called from main service
 */
$UD.showAlert(context)

/**
 * Write a message to the plugin log file
 * Log path: ~/AppData/Roaming/Ulanzi/UlanziStudio/logs/{mainServiceUUID}.log
 * @param {string} msg    Required
 * @param {string} level  Optional | 'info' | 'debug' | 'warn' | 'error' (default: 'info')
 */
$UD.logMessage(msg, level)

/**
 * Trigger an OS-level hotkey
 * Mac: Use ^, ⌘, ⌥, ⇧ as modifiers (e.g. '⌘C')
 * Windows: Use Ctrl+C style (e.g. 'Ctrl+C')
 * @param {string} key  Required
 */
$UD.hotkey(key)

/**
 * Open a URL in the system browser
 * @param {string}  url    Required | Cannot include query params; pass them via `param`
 * @param {boolean} local  Optional | true if local file path
 * @param {object}  param  Optional | Query params
 */
$UD.openUrl(url, local, param)

/**
 * Open a local HTML file as a popup window
 * Close from inside by calling window.close()
 * @param {string} url    Required | Local HTML path (no query params; use `param`)
 * @param {number} width  Optional | Default 200
 * @param {number} height Optional | Default 200
 * @param {number} x      Optional | Window x position; centered if omitted
 * @param {number} y      Optional | Window y position; centered if omitted
 * @param {object} param  Optional | Params passed to the HTML file
 */
$UD.openView(url, width, height, x, y, param)

/**
 * Open a file picker dialog
 * @param {string} filter  Optional | e.g. 'image(*.jpg *.png *.gif)' or 'file(*.txt *.json)'
 * Result is returned via onSelectdialog
 */
$UD.selectFileDialog(filter)

/**
 * Open a folder picker dialog
 * Result is returned via onSelectdialog
 */
$UD.selectFolderDialog()
```

---

## Utils API

`Utils` is a singleton exported from `index.js`.

```js
/**
 * Get the plugin root directory path (the folder ending with *.ulanziPlugin)
 * Compatible with Windows and macOS
 * @returns {string}
 */
Utils.getPluginPath()

/**
 * Get the current operating system type
 * @returns {'windows' | 'mac'}
 */
Utils.getSystemType()

/**
 * Normalize a language code to a supported locale string
 * e.g. 'zh-CN' → 'zh_CN', 'en-US' → 'en'
 * @param {string} ln
 * @returns {string}
 */
Utils.adaptLanguage(ln)

/**
 * Safely parse a JSON string; returns false on failure
 * @param {string} jsonString
 * @returns {object|false}
 */
Utils.parseJson(jsonString)

/**
 * Debounce a function call
 * @param {function} fn
 * @param {number}   wait  Delay in ms (default: 150)
 * @returns {function}
 */
Utils.debounce(fn, wait)

/**
 * Get a nested property value using a dot-separated key path
 * Supports array notation: 'list[0].name'
 */
Utils.getProperty(obj, dotSeparatedKeys, defaultValue)
```

---

## Debugging

Launch the host application with the following flags to enable debugging.

**Available flags:**

| Flag | Description |
|------|-------------|
| `--log` | Write logs to file |
| `--logLevel` | Set log verbosity |
| `--pluginLoad` | Enable plugin load hook |
| `--webRemoteDebug` | Enable WebView remote debugging for HTML plugins. Default port 9292 — open `localhost:9292` in the browser to debug all loaded HTML plugins |
| `--webRemotePort=<port>` | Override WebView debug port, e.g. `--webRemotePort=9292` |
| `--nodeRemoteDebug` | Enable remote debugging for Node.js plugins. Requires `"Inspect": "--inspect=[host:port]"` in the plugin's `manifest.json`. Default address `127.0.0.1:9229`. Open `chrome://inspect` in Chrome; for non-default ports, add the port under Discover network targets |
| `--doubleClick` | Enable double-click detection |

**Windows:**

Right-click the Ulanzi Studio shortcut → Properties → append flags to the end of the **Target** field:
```
"C:\...\Ulanzi Studio.exe" --log --webRemoteDebug
```

**macOS:**
```bash
open /Applications/Ulanzi\ Studio.app --args --log --webRemoteDebug
```

> Note: the `open` command may prevent the app from obtaining Accessibility permissions, which can disable hotkey functionality. Use `./UlanziStudio` directly if hotkeys are not working.
