# ulanzistudio-plugin-sdk-html

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
libs/
├── css/
│   └── uspi.css          // Common styles. Follow the HTML conventions below to apply them automatically.
├── js/
│   ├── constants.js      // Frozen event-name constants (Events.*) used throughout the SDK
│   ├── eventEmitter.js   // Lightweight pub/sub event emitter with wildcard support
│   ├── timers.js         // Replaces window.setTimeout/setInterval with Web Worker-based versions to avoid UI thread blocking
│   ├── utils.js          // Helper utilities: form data, canvas/image drawing, HTTP requests, localization, etc.
│   └── ulanziApi.js  // Main SDK class. Encapsulates all UlanziStudio events, WebSocket connection, and localization.
└── assets/
    └── *.svg             // Icons required by uspi.css. Do not modify.
```

---

## Instructions & Conventions

1. **Main service** (`app.html`) stays connected to the UlanziStudio at all times. It implements the plugin's core logic, receives param changes from actions, and updates icon states.

2. **Action / PropertyInspector** (`inspector.html`) is destroyed when the user switches buttons. Keep it lightweight — only use it to send/receive configuration params.

3. Plugin package naming: `com.ulanzi.{pluginName}.ulanziPlugin`

4. The **main service UUID** must have exactly **4** dot-separated segments:
   `com.ulanzi.ulanzistudio.{pluginName}`

5. An **action UUID** must have **more than 4** segments to be distinguished from the main service:
   `com.ulanzi.ulanzistudio.{pluginName}.{actionName}`

6. Localization JSON files go in the **plugin root directory** (same level as `libs/`). Supported file names:
   `zh_CN.json` `zh_HK.json` `en.json` `ja_JP.json` `de_DE.json` `ko_KR.json` `pt_PT.json` `es_ES.json`

7. The built-in font **Source Han Sans SC** is referenced in `uspi.css`. Reference the same font in `app.html` when drawing icons on canvas.

8. The UlanziStudio background color is `#1e1f22` (set as `--uspi-bodybg` in `uspi.css`). Match this color when customizing action backgrounds.

9. The `controller` URL parameter indicates device type: `Keypad` (button) or `Encoder` (dial). Read it via `$UD.controller` after connecting.

---

## How to Use

### Special Parameter: `context`

Because the same action can be assigned to multiple keys, the SDK generates a unique **`context`** string per key instance and appends it to every received message.

- **Format:** `uuid + '___' + key + '___' + actionid`
- **Encode:** `$UD.encodeContext(msg)` → returns a context string
- **Decode:** `$UD.decodeContext(context)` → returns `{ uuid, key, actionid }`
- For the `clear` event, `context` is spliced into each item of the `param` array. Iterate over `message.param` to retrieve individual contexts.

---

### 1. Import SDK Files

The files must be included **in the following order**:

```html
<script src="../../libs/js/constants.js"></script>
<script src="../../libs/js/eventEmitter.js"></script>
<script src="../../libs/js/timers.js"></script>
<script src="../../libs/js/utils.js"></script>
<script src="../../libs/js/ulanziApi.js"></script>
```

After loading, the global `$UD` instance and `Utils` instance are available.

---

### 2. HTML Structure for `uspi.css`

Wrap the entire inspector page with `.uspi-wrapper` to enable automatic localization. Use the class naming convention below to apply common styles:

```html
<link rel="stylesheet" href="../../libs/css/uspi.css">

<div class="uspi-wrapper">
  <form id="property-inspector">

    <div class="uspi-item">
      <!-- data-localize (method 1): SDK looks up the element's text content as the translation key -->
      <div class="uspi-item-label" data-localize>Name</div>
      <input type="text" class="uspi-item-value" name="name" value="">
    </div>

    <div class="uspi-item">
      <div class="uspi-item-label" data-localize>Color</div>
      <select class="uspi-item-value" name="color">
        <!-- data-localize (method 2): SDK looks up the attribute value as the translation key -->
        <option value="blue" data-localize="Blue">Blue</option>
        <option value="green" data-localize="Green">Green</option>
      </select>
    </div>

  </form>
</div>
```

Use `Utils.getFormValue(form)` to read form data and `Utils.setFormValue(jsn, form)` to populate it.

---

### 3. Connect to UlanziStudio

```js
// Connect — reads port, address, uuid, key, language, controller, device from URL params
$UD.connect('com.ulanzi.ulanzistudio.myplugin.myaction');

$UD.onConnected(conn => {
  // WebSocket is open; render dynamic UI here
});

$UD.onAdd(message => {
  // Action was assigned to a key; load saved params into form
  Utils.setFormValue(message.param, '#property-inspector');
});

$UD.onParamFromApp(message => {
  // Main service pushed updated params to this action page
  Utils.setFormValue(message.param, '#property-inspector');
});

// Send params to the main service when user changes form values
document.querySelector('#property-inspector').addEventListener('change', () => {
  const params = Utils.getFormValue('#property-inspector');
  $UD.sendParamFromPlugin(params);
});
```

---

### 4. Localization JSON Format

Localization JSON files can include two kinds of entries:

- Top-level manifest fields: use `Name`, `Description`, and an `Actions` array aligned by index with `manifest.json` to localize how the plugin and its actions appear in UlanziStudio.
- Custom UI strings: use the `Localization` object for Property Inspector or page text, where each key matches the text or attribute value referenced by `data-localize`.

**zh_CN.json:**

```json
{
  "Name": "我的插件",
  "Description": "我的插件描述",
  "Actions": [
    {
      "Name": "插件操作",
      "Tooltip": "执行插件操作"
    }
  ],
  "Localization": {
    "Message": "消息",
    "Save": "保存",
    "Hello": "你好"
  }
}
```

**en.json:**

```json
{
  "Name": "My Plugin",
  "Description": "My plugin description",
  "Actions": [
    {
      "Name": "Plugin Action",
      "Tooltip": "Run plugin action"
    }
  ],
  "Localization": {
    "Message": "Message",
    "Save": "Save",
    "Hello": "Hello"
  }
}
```

You can also translate strings manually in JavaScript:
```js
const label = $UD.t('Message'); // returns translated string, or the key itself if not found
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
// Action was added to a key (drag-and-drop); message.param contains saved settings
$UD.onAdd(message => {})

// Key was "run" (single click confirmed by the host); main entry point for plugin logic
$UD.onRun(message => {})

// Key press started (fires before run; use for long-press detection)
$UD.onKeyDown(message => {})

// Key press released
$UD.onKeyUp(message => {})

// Action active state changed; message.active = true/false
$UD.onSetActive(message => {})

// Action was removed from one or more keys; message.param is an array, each item has .context
$UD.onClear(message => {})
```

### Dial / Encoder Events

```js
$UD.onDialDown(message => {})         // Dial pressed
$UD.onDialUp(message => {})           // Dial released
$UD.onDialRotate(message => {})       // Any dial rotation; message.rotateEvent = 'left' | 'right' | 'hold-left' | 'hold-right'
$UD.onDialRotateLeft(message => {})       // Rotated left (not held)
$UD.onDialRotateRight(message => {})      // Rotated right (not held)
$UD.onDialRotateHoldLeft(message => {})   // Rotated left while pressed
$UD.onDialRotateHoldRight(message => {}) // Rotated right while pressed
```

### Param / Config Events

```js
// Host pushed params to action page (inspector) on open
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
// Action page: receives data sent by main service via sendToPropertyInspector()
$UD.onSendToPropertyInspector(message => {})

// Main service: receives data sent by action page via sendToPlugin()
$UD.onSendToPlugin(message => {})
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
 * @param {string} path     Required | Relative path from plugin root (e.g. './icons/foo.png')
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
 * Send config params from plugin (action page → host → main service, or main service → host → action page)
 * @param {object} settings  Required | Configuration params object
 * @param {string} context   Optional when called from action page; Required when called from main service
 */
$UD.sendParamFromPlugin(settings, context)

/**
 * Main service → action page: pass-through data (not saved by host)
 * @param {object} settings  Required
 * @param {string} context   Required | Target action's context
 */
$UD.sendToPropertyInspector(settings, context)

/**
 * Action page → main service: pass-through data (not saved by host)
 * @param {object} settings  Required
 */
$UD.sendToPlugin(settings)
```

### Settings Persistence

```js
/**
 * Save action-specific settings to the host. Triggers didReceiveSettings on both ends.
 * Note: settings are NOT saved when the action is inactive.
 * @param {object} settings  Required
 * @param {string} context   Optional from action page; Required from main service
 */
$UD.setSettings(settings, context)

/**
 * Request saved action settings from the host. Response arrives via onDidReceiveSettings.
 * @param {string} context   Optional from action page; Required from main service
 */
$UD.getSettings(context)

/**
 * Save plugin-wide global settings. Triggers didReceiveGlobalSettings on all connected pages.
 * Uses the plugin's main UUID (ignores actionId).
 * @param {object} settings  Required
 * @param {string} context   Optional
 */
$UD.setGlobalSettings(settings, context)

/**
 * Request global settings from the host. Response arrives via onDidReceiveGlobalSettings.
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
 * @param {string} context  Optional from action page; Required from main service
 */
$UD.showAlert(context)

/**
 * Write a message to the plugin log file
 * Log path: ~/AppData/Roaming/Ulanzi/UlanziDeck/logs/{mainServiceUUID}.log
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
 * @param {string} url    Required | Path cannot include query params; pass them via `param`
 * @param {boolean} local Optional | true if it is a local file path
 * @param {object} param  Optional | Query params passed to the URL
 */
$UD.openUrl(url, local, param)

/**
 * Open a local HTML file as a popup window
 * Close the popup from inside by calling window.close()
 * @param {string} url    Required | Local HTML path (no query params; use `param` instead)
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

`Utils` is a global instance available after loading `utils.js`.

### Form Utilities

```js
/**
 * Read all named form controls into a plain object
 * @param {Element|string} form  Form element or CSS selector
 * @returns {object}
 */
Utils.getFormValue(form)

/**
 * Populate form controls from a plain object (matches by control `name` attribute)
 * @param {object}         jsn   Data object
 * @param {Element|string} form  Form element or CSS selector
 */
Utils.setFormValue(jsn, form)

/**
 * Debounce a function call
 * @param {function} fn    Function to debounce
 * @param {number}   wait  Delay in ms (default: 150)
 * @returns {function}
 */
Utils.debounce(fn, wait)
```

### Canvas / Image Utilities

```js
/**
 * Draw an image onto a canvas and return a base64 PNG string
 * @param {string}            url           Image URL or data URL
 * @param {number}            width         Default 196
 * @param {number}            height        Default 196
 * @param {HTMLCanvasElement} inCanvas      Optional existing canvas to draw onto
 * @param {boolean}           returnCanvas  If true, return the canvas instead of base64
 * @returns {Promise<string|HTMLCanvasElement>}
 */
Utils.drawImage(url, width, height, inCanvas, returnCanvas)

/**
 * Crop a region of an image and return a base64 PNG string
 * @param {string}  url           Image URL
 * @param {number}  offsetX       Crop origin X in the source image
 * @param {number}  offsetY       Crop origin Y in the source image
 * @param {number}  width         Output width (default 196)
 * @param {number}  height        Output height (default 196)
 * @param {HTMLCanvasElement} inCanvas
 * @param {boolean} returnCanvas
 * @returns {Promise<string|HTMLCanvasElement>}
 */
Utils.cropImage(url, offsetX, offsetY, width, height, inCanvas, returnCanvas)

/**
 * Draw centered text onto a canvas and return a base64 PNG string
 * @param {string}  text        Main text
 * @param {string}  stroke      Text color (default '#fff')
 * @param {string}  background  Background color or 'transparent' (default '#000')
 * @param {number}  wh          Canvas size in px (default 196, square)
 * @param {string}  textLabel   Optional small label drawn in top-left corner
 * @param {HTMLCanvasElement} inCanvas  Optional existing canvas to draw onto
 * @returns {string}  Base64 PNG
 */
Utils.drawText(text, stroke, background, wh, textLabel, inCanvas)

/**
 * Load an image and return a Promise resolving to { url, status, img }
 * status is 'ok' on success or 'error' on failure
 */
Utils.loadImagePromise(url)

/**
 * Convert a browser File object to a base64 data URL
 * @param {File} file
 * @returns {Promise<string>}
 */
Utils.htmlFileToBase64(file)
```

### HTTP Utilities

```js
/**
 * Simple GET request with automatic cache-busting timestamp
 * @param {string} url
 * @param {object} param  Query params (arrays are expanded to repeated keys)
 * @returns {Promise<string>}  Raw response text
 */
Utils.getData(url, param)

/**
 * Fetch wrapper with timeout support
 * @param {string} url
 * @param {object} param    Query params (GET) or body (POST/PUT)
 * @param {string} method   'GET' | 'POST' | 'PUT' | 'DELETE' (default: 'GET')
 * @param {object} headers  Additional request headers
 * @returns {Promise<object>}  Parsed JSON response
 */
Utils.fetchData(url, param, method, headers)

/**
 * Load and parse a JSON file via XHR
 * @param {string} path
 * @returns {Promise<object>}
 */
Utils.readJson(path)
```

### Helper Utilities

```js
/**
 * Get a URL query parameter value from window.location.search
 * @param {string} param
 * @returns {string|null}
 */
Utils.getQueryParams(param)

/**
 * Get the plugin root directory path (ends at the folder named *.ulanziPlugin)
 * @returns {string}
 */
Utils.getPluginPath()

/**
 * Safely parse a JSON string; returns false on failure
 * @param {string} jsonString
 * @returns {object|false}
 */
Utils.parseJson(jsonString)

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
