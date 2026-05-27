# ulanzistudio-plugin-sdk html版本

<p align="start">
   <a href="./README.md">English</a> | <strong>简体中文</strong>
</p>

## 简介

我们依据插件开发协议，封装了与上位机的 WebSocket 连接及相关的通信事件。这样简化了开发流程，使开发者仅需通过简单的事件调用即可实现与上位机的通信，从而能更专注于插件功能的开发。

> 当前版本根据 **Ulanzi JS 插件开发协议 - V2.1.2** 编写。

`manifest.json` 配置字段详细说明，请参阅 **[manifest.zh.md](https://github.com/UlanziTechnology/UlanziDeckPlugin-SDK/blob/main/manifest.zh.md)**。

---

## 文件介绍

```
libs/
├── css/
│   └── uspi.css          // 通用样式，按照约定格式编写 HTML 即可生效
├── js/
│   ├── constants.js      // 上位机事件名常量（Events.*），整个 SDK 中使用
│   ├── eventEmitter.js   // 轻量级发布/订阅事件系统，支持通配符
│   ├── timers.js         // 用 Web Worker 替换 window.setTimeout/setInterval，避免阻塞 UI 线程
│   ├── utils.js          // 工具方法：表单数据、Canvas/图片绘制、HTTP 请求、本地化等
│   └── ulanziApi.js  // SDK 主类，封装了所有上位机事件、WebSocket 连接与本地化处理
└── assets/
    └── *.svg             // uspi.css 所需的图标资源，无需修改
```

---

## 说明与约定

1. **主服务**（如 `app.html`）始终与上位机保持连接，负责实现插件的核心功能：接收 action 配置变更、更新 icon 状态等。

2. **配置项页面 / PropertyInspector**（如 `inspector.html`）在用户切换功能按键后会被销毁，不适合做功能逻辑处理，主要用于发送和同步配置参数。

3. 插件包命名规范：`com.ulanzi.{插件名}.ulanziPlugin`

4. **主服务 UUID** 必须由恰好 **4** 个点分隔段组成：
   `com.ulanzi.ulanzistudio.{插件名}`

5. **action UUID** 必须超过 4 段，以便与主服务区分：
   `com.ulanzi.ulanzistudio.{插件名}.{actionName}`

6. 本地化 JSON 文件放在**插件根目录**（与 `libs/` 同级）。支持的文件名：
   `zh_CN.json` `zh_HK.json` `en.json` `ja_JP.json` `de_DE.json` `ko_KR.json` `pt_PT.json` `es_ES.json`

7. `uspi.css` 中已引用上位机内置的开源字体**思源黑体（Source Han Sans SC）**。在 `app.html` 中使用 Canvas 绘制 icon 时，请统一使用 `'Source Han Sans SC'`。

8. 上位机背景颜色为 `#1e1f22`（已在 `uspi.css` 中设为 `--uspi-bodybg`）。若自定义 action 背景色，建议与此保持一致，避免视觉突兀。

9. `controller` URL 参数表示设备类型：`Keypad`（普通按键）或 `Encoder`（旋钮）。连接后可通过 `$UD.controller` 读取。

---

## 使用步骤

### 特殊参数 `context`

由于同一个 action 可以被配置到多个按键上，SDK 会为每个按键实例生成一个唯一的 **`context`** 字符串，并随每条接收到的消息一同传递。

- **格式：** `uuid + '___' + key + '___' + actionid`
- **编码：** `$UD.encodeContext(msg)` → 返回 context 字符串
- **解码：** `$UD.decodeContext(context)` → 返回 `{ uuid, key, actionid }`
- `clear` 事件的 `param` 是数组形式，context 拼接在每个数组元素中，处理时需循环遍历 `message.param`。

---

### 1. 引入 SDK 文件

必须按以下**顺序**引入：

```html
<script src="../../libs/js/constants.js"></script>
<script src="../../libs/js/eventEmitter.js"></script>
<script src="../../libs/js/timers.js"></script>
<script src="../../libs/js/utils.js"></script>
<script src="../../libs/js/ulanziApi.js"></script>
```

加载完成后，全局变量 `$UD` 和 `Utils` 即可使用。

---

### 2. 适配 `uspi.css` 的 HTML 结构

用 `.uspi-wrapper` 包裹整个配置项页面，以启用自动本地化。使用以下类名约定来应用通用样式：

```html
<link rel="stylesheet" href="../../libs/css/uspi.css">

<div class="uspi-wrapper">
  <form id="property-inspector">

    <div class="uspi-item">
      <!-- data-localize 第一种方式：SDK 以元素文本内容为 key 查找翻译 -->
      <div class="uspi-item-label" data-localize>Name</div>
      <input type="text" class="uspi-item-value" name="name" value="">
    </div>

    <div class="uspi-item">
      <div class="uspi-item-label" data-localize>Color</div>
      <select class="uspi-item-value" name="color">
        <!-- data-localize 第二种方式：SDK 以属性值为 key 查找翻译 -->
        <option value="blue" data-localize="Blue">Blue</option>
        <option value="green" data-localize="Green">Green</option>
      </select>
    </div>

  </form>
</div>
```

使用 `Utils.getFormValue(form)` 读取表单数据，`Utils.setFormValue(jsn, form)` 回填表单数据。

---

### 3. 连接上位机

```js
// 连接上位机，连接参数（port、address、uuid、key、language、controller、device）从 URL 参数中自动读取
$UD.connect('com.ulanzi.ulanzistudio.myplugin.myaction');

$UD.onConnected(conn => {
  // WebSocket 已连接，可在此处渲染动态 UI
});

$UD.onAdd(message => {
  // action 被配置到按键，加载已保存的参数回填表单
  Utils.setFormValue(message.param, '#property-inspector');
});

$UD.onParamFromApp(message => {
  // 主服务推送了更新的参数到本配置项页面
  Utils.setFormValue(message.param, '#property-inspector');
});

// 表单变化时发送参数给主服务
document.querySelector('#property-inspector').addEventListener('change', () => {
  const params = Utils.getFormValue('#property-inspector');
  $UD.sendParamFromPlugin(params);
});
```

---

### 4. 本地化 JSON 格式

本地化 JSON 文件可以包含两类内容：

- 顶层 manifest 字段：使用 `Name`、`Description`，以及与 `manifest.json` 中 action 顺序一致的 `Actions` 数组，用于本地化插件和 action 在上位机中的显示名称与提示文案。
- 自定义 UI 文案：使用 `Localization` 对象存放配置项页面或普通页面文案，其中每个 key 需要与 `data-localize` 引用的文本或属性值一致。

**zh_CN.json：**

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

**en.json：**

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

也可在 JavaScript 中手动调用翻译：
```js
const label = $UD.t('Message'); // 返回翻译后的字符串，找不到时返回 key 本身
```

---

## 接收事件（上位机 → 插件）

### 连接事件

```js
$UD.onConnected(conn => {})   // WebSocket 连接成功
$UD.onClose(conn => {})       // WebSocket 连接断开
$UD.onError(conn => {})       // WebSocket 发生错误
```

### 按键事件

```js
// action 被拖拽到按键（新增）；message.param 包含已保存的设置
$UD.onAdd(message => {})

// 按键被触发执行（单次点击确认）；插件逻辑的主入口
$UD.onRun(message => {})

// 按键按下（在 run 之前触发，可用于长按检测）
$UD.onKeyDown(message => {})

// 按键松开
$UD.onKeyUp(message => {})

// action 激活状态改变；message.active = true / false
$UD.onSetActive(message => {})

// action 从一个或多个按键上被移除；message.param 是数组，每项包含 .context
$UD.onClear(message => {})
```

### 旋钮（Encoder）事件

```js
$UD.onDialDown(message => {})         // 旋钮按下
$UD.onDialUp(message => {})           // 旋钮松开
$UD.onDialRotate(message => {})       // 旋钮旋转（任意方向）；message.rotateEvent = 'left' | 'right' | 'hold-left' | 'hold-right'
$UD.onDialRotateLeft(message => {})       // 向左旋转（未按住）
$UD.onDialRotateRight(message => {})      // 向右旋转（未按住）
$UD.onDialRotateHoldLeft(message => {})   // 按住旋钮并向左旋转
$UD.onDialRotateHoldRight(message => {}) // 按住旋钮并向右旋转
```

### 参数 / 配置事件

```js
// 上位机在配置项页面打开时推送参数
$UD.onParamFromApp(message => {})

// 上位机转发插件发送的参数（paramfromplugin 回响）
$UD.onParamFromPlugin(message => {})
```

### 设置持久化事件

```js
// 在 getSettings() 或 setSettings() 之后触发；message.settings 包含保存的数据
$UD.onDidReceiveSettings(message => {})

// 在 getGlobalSettings() 或 setGlobalSettings() 之后触发
$UD.onDidReceiveGlobalSettings(message => {})
```

### 跨页面通信事件

```js
// 配置项页面：接收主服务通过 sendToPropertyInspector() 发来的透传数据
$UD.onSendToPropertyInspector(message => {})

// 主服务：接收配置项页面通过 sendToPlugin() 发来的透传数据
$UD.onSendToPlugin(message => {})
```

### 对话框结果

```js
// selectFileDialog() 或 selectFolderDialog() 的结果；message.path 为用户选择的路径
$UD.onSelectdialog(message => {})
```

---

## 发送事件（插件 → 上位机）

### 设置按键图标

```js
/**
 * 使用 manifest.json 的 States 数组中定义的图标编号
 * @param {string} context  必传 | 目标按键的唯一值
 * @param {number} state    必传 | States 数组索引
 * @param {string} text     可选 | 叠加在图标上的文字
 */
$UD.setStateIcon(context, state, text)

/**
 * 使用自定义图片（base64）
 * @param {string} context  必传
 * @param {string} data     必传 | base64 编码的图片（PNG/JPG/SVG）
 * @param {string} text     可选
 */
$UD.setBaseDataIcon(context, data, text)

/**
 * 使用本地图片文件路径
 * @param {string} context  必传
 * @param {string} path     必传 | 相对于插件根目录的路径（如 './icons/foo.png'）
 * @param {string} text     可选
 */
$UD.setPathIcon(context, path, text)

/**
 * 使用自定义 GIF 动图（base64）
 * @param {string} context  必传
 * @param {string} gifdata  必传 | base64 编码的 GIF 数据
 * @param {string} text     可选
 */
$UD.setGifDataIcon(context, gifdata, text)

/**
 * 使用本地 GIF 文件路径
 * @param {string} context  必传
 * @param {string} gifpath  必传 | 相对于插件根目录的路径
 * @param {string} text     可选
 */
$UD.setGifPathIcon(context, gifpath, text)
```

### 发送参数

```js
/**
 * 向上位机发送配置参数（配置项页面→上位机→主服务，或主服务→上位机→配置项页面）
 * @param {object} settings  必传 | 配置参数对象
 * @param {string} context   由配置项页面发出时可不传；由主服务发出时必传
 */
$UD.sendParamFromPlugin(settings, context)

/**
 * 主服务→配置项页面：透传数据（上位机不保存）
 * @param {object} settings  必传
 * @param {string} context   必传 | 目标 action 的 context
 */
$UD.sendToPropertyInspector(settings, context)

/**
 * 配置项页面→主服务：透传数据（上位机不保存）
 * @param {object} settings  必传
 */
$UD.sendToPlugin(settings)
```

### 设置持久化

```js
/**
 * 主动向上位机保存 action 参数；上位机接收后会向两端触发 didReceiveSettings 事件。
 * 注意：action 处于非激活状态时不会保存参数。
 * @param {object} settings  必传
 * @param {string} context   由配置项页面发出时可不传；由主服务发出时必传
 */
$UD.setSettings(settings, context)

/**
 * 请求上位机下发已保存的 action 参数；响应通过 onDidReceiveSettings 接收。
 * @param {string} context   由配置项页面发出时可不传；由主服务发出时必传
 */
$UD.getSettings(context)

/**
 * 保存插件全局参数；上位机接收后向所有连接端触发 didReceiveGlobalSettings 事件。
 * 使用插件主服务 UUID（忽略 actionId）。
 * @param {object} settings  必传
 * @param {string} context   可选
 */
$UD.setGlobalSettings(settings, context)

/**
 * 请求上位机下发全局参数；响应通过 onDidReceiveGlobalSettings 接收。
 * @param {string} context   可选
 */
$UD.getGlobalSettings(context)
```

### 系统功能

```js
/**
 * 在上位机应用上弹出 Toast 消息提示
 * @param {string} msg  必传
 */
$UD.toast(msg)

/**
 * 在按键上显示错误提示动画
 * @param {string} context  由配置项页面发出时可不传；由主服务发出时必传
 */
$UD.showAlert(context)

/**
 * 将消息写入插件日志文件
 * 日志路径：~/AppData/Roaming/Ulanzi/UlanziDeck/logs/{主服务UUID}.log
 * @param {string} msg    必传
 * @param {string} level  可选 | 'info' | 'debug' | 'warn' | 'error'（默认 'info'）
 */
$UD.logMessage(msg, level)

/**
 * 请求上位机触发系统快捷键
 * Mac：使用 ^、⌘、⌥、⇧ 作为修饰键（如 '⌘C'）
 * Windows：使用 Ctrl+C 风格（如 'Ctrl+C'）
 * @param {string} key  必传
 */
$UD.hotkey(key)

/**
 * 请求上位机用系统浏览器打开 URL
 * @param {string}  url    必传 | 路径不能带查询参数，需要传参请使用 param
 * @param {boolean} local  可选 | 本地文件路径时为 true
 * @param {object}  param  可选 | 传递给 URL 的查询参数
 */
$UD.openUrl(url, local, param)

/**
 * 请求上位机以弹窗形式显示一个本地 HTML 文件
 * 弹窗内部调用 window.close() 可关闭弹窗
 * @param {string} url    必传 | 本地 HTML 路径（不能带查询参数，使用 param 传参）
 * @param {number} width  可选 | 默认 200
 * @param {number} height 可选 | 默认 200
 * @param {number} x      可选 | 窗口 x 坐标，不传默认居中
 * @param {number} y      可选 | 窗口 y 坐标，不传默认居中
 * @param {object} param  可选 | 传递给 HTML 文件的参数
 */
$UD.openView(url, width, height, x, y, param)

/**
 * 请求上位机弹出文件选择对话框
 * @param {string} filter  可选 | 文件类型过滤，如 'image(*.jpg *.png *.gif)' 或 'file(*.txt *.json)'
 * 选择结果通过 onSelectdialog 接收
 */
$UD.selectFileDialog(filter)

/**
 * 请求上位机弹出文件夹选择对话框
 * 选择结果通过 onSelectdialog 接收
 */
$UD.selectFolderDialog()
```

---

## Utils 工具 API

`Utils` 是加载 `utils.js` 后自动创建的全局实例。

### 表单工具

```js
/**
 * 读取所有具名表单控件的值，返回纯对象
 * @param {Element|string} form  表单元素或 CSS 选择器
 * @returns {object}
 */
Utils.getFormValue(form)

/**
 * 将纯对象的值按控件 name 属性回填到表单
 * @param {object}         jsn   数据对象
 * @param {Element|string} form  表单元素或 CSS 选择器
 */
Utils.setFormValue(jsn, form)

/**
 * 防抖：在一定延迟后执行函数
 * @param {function} fn    需要防抖的函数
 * @param {number}   wait  延迟时间，单位 ms（默认 150）
 * @returns {function}
 */
Utils.debounce(fn, wait)
```

### Canvas / 图片工具

```js
/**
 * 将图片绘制到 Canvas 并返回 base64 PNG 字符串
 * @param {string}            url           图片 URL 或 Data URL
 * @param {number}            width         默认 196
 * @param {number}            height        默认 196
 * @param {HTMLCanvasElement} inCanvas      可选，复用已有 Canvas
 * @param {boolean}           returnCanvas  为 true 时返回 Canvas 而非 base64
 * @returns {Promise<string|HTMLCanvasElement>}
 */
Utils.drawImage(url, width, height, inCanvas, returnCanvas)

/**
 * 裁剪图片的指定区域并返回 base64 PNG 字符串
 * @param {string}  url           图片 URL
 * @param {number}  offsetX       在源图上的裁剪起点 X
 * @param {number}  offsetY       在源图上的裁剪起点 Y
 * @param {number}  width         输出宽度（默认 196）
 * @param {number}  height        输出高度（默认 196）
 * @param {HTMLCanvasElement} inCanvas
 * @param {boolean} returnCanvas
 * @returns {Promise<string|HTMLCanvasElement>}
 */
Utils.cropImage(url, offsetX, offsetY, width, height, inCanvas, returnCanvas)

/**
 * 在 Canvas 上绘制居中文字，返回 base64 PNG 字符串
 * @param {string}  text        主要文字
 * @param {string}  stroke      文字颜色（默认 '#fff'）
 * @param {string}  background  背景色或 'transparent'（默认 '#000'）
 * @param {number}  wh          Canvas 边长，单位 px（默认 196，正方形）
 * @param {string}  textLabel   可选，绘制在左上角的小标签文字
 * @param {HTMLCanvasElement} inCanvas  可选，复用已有 Canvas
 * @returns {string}  base64 PNG
 */
Utils.drawText(text, stroke, background, wh, textLabel, inCanvas)

/**
 * 加载图片，返回 Promise，resolve 值为 { url, status, img }
 * status 为 'ok' 或 'error'
 */
Utils.loadImagePromise(url)

/**
 * 将浏览器 File 对象转换为 base64 Data URL
 * @param {File} file
 * @returns {Promise<string>}
 */
Utils.htmlFileToBase64(file)
```

### HTTP 工具

```js
/**
 * 简单的 GET 请求，自动附加防缓存时间戳
 * @param {string} url
 * @param {object} param  查询参数（数组会展开为多个同名参数）
 * @returns {Promise<string>}  原始响应文本
 */
Utils.getData(url, param)

/**
 * 带超时的 fetch 封装，支持 GET/POST/PUT/DELETE
 * @param {string} url
 * @param {object} param    GET 时为查询参数，其他方法时为请求体
 * @param {string} method   'GET' | 'POST' | 'PUT' | 'DELETE'（默认 'GET'）
 * @param {object} headers  额外的请求头
 * @returns {Promise<object>}  解析后的 JSON 响应
 */
Utils.fetchData(url, param, method, headers)

/**
 * 通过 XHR 加载并解析 JSON 文件
 * @param {string} path
 * @returns {Promise<object>}
 */
Utils.readJson(path)
```

### 其他工具

```js
/**
 * 从 window.location.search 中获取指定 URL 查询参数的值
 * @param {string} param
 * @returns {string|null}
 */
Utils.getQueryParams(param)

/**
 * 获取插件根目录路径（以 *.ulanziPlugin 结尾的目录）
 * @returns {string}
 */
Utils.getPluginPath()

/**
 * 安全解析 JSON 字符串，解析失败返回 false
 * @param {string} jsonString
 * @returns {object|false}
 */
Utils.parseJson(jsonString)

/**
 * 使用点分隔路径读取对象的嵌套属性
 * 支持数组语法：'list[0].name'
 */
Utils.getProperty(obj, dotSeparatedKeys, defaultValue)
```

---

## 调试

启动上位机应用时可附加以下参数开启调试。

**可用参数：**

| 参数 | 说明 |
|------|------|
| `--log` | 将日志写入文件 |
| `--logLevel` | 设置日志级别 |
| `--pluginLoad` | 开启插件加载钩子 |
| `--webRemoteDebug` | 启用 HTML 插件的 WebView 远程调试，默认端口 9292，通过浏览器打开 `localhost:9292` 进行调试，可调试所有已加载的 HTML 插件 |
| `--webRemotePort=<端口>` | 自定义 WebView 调试端口，例：`--webRemotePort=9292` |
| `--nodeRemoteDebug` | 启用 Node.js 插件远程调试，需在插件 `manifest.json` 中配置 `"Inspect": "--inspect=[host:port]"`，默认地址 `127.0.0.1:9229`；在 Chrome 中打开 `chrome://inspect`，非默认端口时需在 Discover network targets 中手动添加对应端口 |
| `--doubleClick` | 启用双击检测 |

**Windows：**

右键 Ulanzi Studio 快捷方式 → 属性 → 在**目标**栏末尾追加启动参数：
```
"C:\...\Ulanzi Studio.exe" --log --webRemoteDebug
```

**macOS：**
```bash
open /Applications/Ulanzi\ Studio.app --args --log ---webRemoteDebug 
```

> 注意：使用 `open` 命令启动时，应用可能无法获得系统辅助功能权限，会导致快捷键功能失效。若遇此问题，请改用 `./UlanziStudio` 直接启动。
