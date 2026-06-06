# ulanzistudio-plugin-sdk-node

<p align="start">
   <a href="./README.md">English</a> | <strong>简体中文</strong>
</p>

## 简介

ulanzistudio-plugin-sdk 封装了与 UlanziStudio 上位机的 WebSocket 连接及相关通信事件，简化了开发流程。开发者只需通过简单的事件调用即可实现与上位机的通信，从而更专注于插件功能的开发。

> 当前版本根据 **Ulanzi JS 插件开发协议 - V2.1.2** 编写。

`manifest.json` 配置参考，请查看 **[manifest.zh.md](https://github.com/UlanziTechnology/UlanziDeckPlugin-SDK/blob/main/manifest.zh.md)**。

---

## 文件目录

```
ulanzi-api/
├── libs/
│   ├── constants.js   // 冻结的事件名常量（Events.*），供 SDK 内部使用
│   ├── randomPort.js  // 为自建主服务生成随机端口，并将 ws-port.js 写入插件根目录供配置项页面读取
│   ├── utils.js       // 工具方法：插件路径、系统类型检测、JSON 解析等
│   └── ulanziApi.js   // SDK 主类，封装所有 UlanziStudio 事件与 WebSocket 连接
├── apiTypes.d.ts      // TypeScript 类型定义，用于 IDE 自动补全
└── index.js           // 入口文件，导出 UlanziApi、Utils、RandomPort
```

---

## 说明与约定

1. **主服务**（`app.js`）始终与 UlanziStudio 保持连接，负责插件核心逻辑、接收 action 的参数变更并更新图标状态。

2. **Action / 配置项**（`inspector.html`）在用户切换按键后会被销毁，应保持轻量——仅用于发送和接收配置参数。

3. 插件包命名规则：`com.ulanzi.{插件名}.ulanziPlugin`

4. **主服务 UUID** 必须恰好包含 **4** 个以点分隔的段：
   `com.ulanzi.ulanzistudio.{插件名}`

5. **Action UUID** 必须包含 **超过 4** 个段，以与主服务区分：
   `com.ulanzi.ulanzistudio.{插件名}.{actionName}`

6. 当配置项页面需要绕过 UlanziStudio 中转，直接连接插件自己的 Node.js 主服务时，请通过 `RandomPort` 为主服务生成随机监听端口，并写入 `ws-port.js` 供配置项页面读取；普通通过 UlanziStudio 通信的插件不需要使用。详见 [2. 生成随机端口](#2-生成随机端口)。

7. 使用 `Utils.getPluginPath()` 获取插件根目录路径，可兼容本地 Node 环境与上位机打包的 Node 环境之间的差异。详见 [3. 获取插件根目录路径](#3-获取插件根目录路径)。

---

## 使用方法

### 特殊参数：`context`

由于同一个 action 可以被分配到多个按键上，SDK 会为每个按键实例生成一个唯一的 **`context`** 字符串，并附加到每条接收到的消息中。

- **格式：** `uuid + '___' + key + '___' + actionid`
- **编码：** `$UD.encodeContext(msg)` → 返回 context 字符串
- **解码：** `$UD.decodeContext(context)` → 返回 `{ uuid, key, actionid }`
- 对于 `clear` 事件，`context` 被拼接在 `param` 数组的每个元素中。处理时请遍历 `message.param` 逐项获取。

---

### 1. 安装

```bash
npm install ws
```

将 `ulanzi-api` 文件夹复制到插件运行目录，然后引入：

```js
import UlanziApi, { Utils, RandomPort } from './ulanzi-api/index.js';
```

---

### 2. 生成随机端口

`RandomPort` 适用于配置项页面需要**自己连接主服务**的场景：例如主服务在本地自建 WebSocket / HTTP 服务，配置项页面需要绕过 UlanziStudio 中转，直接与这个服务交换连接状态、账号信息、实时列表或其他临时数据。此时主服务启动时先调用一次 `getPort()`，用返回的端口启动自己的服务；`RandomPort` 会同时把端口写入插件根目录的 `ws-port.js`，配置项 HTML 页面引入该文件后即可读取 `window.__port` 并发起连接。

如果配置项与主服务之间的参数同步完全通过 UlanziStudio 的标准事件完成，则不需要使用 `RandomPort`。

```js
import { RandomPort } from './ulanzi-api/index.js';
import { WebSocketServer } from 'ws';

const randomPort = new RandomPort();
const port = randomPort.getPort(); // 生成端口并写入 ws-port.js

// 主服务使用这个端口自启动本地服务，供配置项页面直连
const wss = new WebSocketServer({ host: '127.0.0.1', port });
wss.on('connection', socket => {
  socket.send(JSON.stringify({ type: 'ready' }));
});
```

在配置项 HTML 中，连接前先引入生成的文件：

```html
<script src="../../ws-port.js"></script>
<script>
  const socket = new WebSocket(`ws://127.0.0.1:${window.__port}`);
</script>
```

注意事项：

- `RandomPort` 只负责生成端口并写入 `ws-port.js`，不会自动创建 WebSocket / HTTP 服务。
- 请在主服务启动阶段调用一次 `getPort()`，并在服务监听成功后再让配置项页面连接。
- 默认端口范围为动态端口区间 `49152`-`65535`，用于降低不同插件之间的端口冲突概率；如需限制范围，可通过构造函数传入 `minPort` 和 `maxPort`。

---

### 3. 获取插件根目录路径

`Utils.getPluginPath()` 返回插件根目录（以 `ulanziPlugin` 结尾的文件夹）的绝对路径，兼容 Windows 和 macOS。

```js
import { Utils } from './ulanzi-api/index.js';

const pluginPath = Utils.getPluginPath();
console.log('插件根目录：', pluginPath);

// 示例：读取本地配置文件
import { promises as fs } from 'fs';
const config = JSON.parse(await fs.readFile(`${pluginPath}/config.json`, 'utf8'));
```

---

### 4. 连接上位机

由上位机启动时，连接参数通过 `process.argv` 传入：
- `process.argv[2]` → 地址（默认 `127.0.0.1`）
- `process.argv[3]` → 端口（默认 `3906`）
- `process.argv[4]` → 语言（默认 `en`）

```js
import UlanziApi from './ulanzi-api/index.js';

const $UD = new UlanziApi();

// 连接，argv 参数优先于默认值
$UD.connect('com.ulanzi.ulanzistudio.myplugin');

$UD.onConnected(conn => {
  console.log('已连接');
});

$UD.onAdd(message => {
  // action 被分配到按键；message.context 是唯一的按键标识
  const context = message.context;
});

$UD.onParamFromApp(message => {
  // 上位机推送已保存的参数；使用 message.param
});

$UD.onClear(message => {
  // message.param 是数组，context 在每个元素中
  if (message.param) {
    for (const item of message.param) {
      console.log('已清除 context：', item.context);
    }
  }
});
```

---

## 接收事件（上位机 → 插件）

### 连接事件

```js
$UD.onConnected(conn => {})   // WebSocket 连接成功
$UD.onClose(conn => {})       // WebSocket 连接断开
$UD.onError(conn => {})       // WebSocket 错误
```

### 按键事件

```js
// action 被添加到按键；message.param 包含已保存的设置
$UD.onAdd(message => {})

// 按键触发（单击确认）；插件逻辑的主入口
$UD.onRun(message => {})

// 按键按下（在 run 之前触发；可用于长按检测）
$UD.onKeyDown(message => {})

// 按键释放
$UD.onKeyUp(message => {})

// action 激活状态变化；message.active = true/false
$UD.onSetActive(message => {})

// action 从一个或多个按键上移除；message.param 是数组，每项包含 .context
$UD.onClear(message => {})
```

### 旋钮 / 编码器事件

```js
$UD.onDialDown(message => {})             // 旋钮按下
$UD.onDialUp(message => {})               // 旋钮释放
$UD.onDialRotate(message => {})           // 任意旋转；message.rotateEvent = 'left' | 'right' | 'hold-left' | 'hold-right'
$UD.onDialRotateLeft(message => {})       // 向左旋转（未按住）
$UD.onDialRotateRight(message => {})      // 向右旋转（未按住）
$UD.onDialRotateHoldLeft(message => {})   // 按住向左旋转
$UD.onDialRotateHoldRight(message => {})  // 按住向右旋转
```

### 参数 / 配置事件

```js
// 按键配置时，上位机向插件推送参数
$UD.onParamFromApp(message => {})

// 上位机转发插件发送的参数（paramfromplugin 回调）
$UD.onParamFromPlugin(message => {})
```

### Settings 事件

```js
// 调用 getSettings() 或 setSettings() 后触发；message.settings 包含已保存的数据
$UD.onDidReceiveSettings(message => {})

// 调用 getGlobalSettings() 或 setGlobalSettings() 后触发
$UD.onDidReceiveGlobalSettings(message => {})
```

### 跨页面通信事件

```js
// 主服务：接收配置项通过 sendToPlugin() 发送的数据
$UD.onSendToPlugin(message => {})

// 配置项：接收主服务通过 sendToPropertyInspector() 发送的数据
$UD.onSendToPropertyInspector(message => {})
```

### 对话框结果

```js
// selectFileDialog() 或 selectFolderDialog() 的结果；message.path 为选中路径
$UD.onSelectdialog(message => {})
```

---

## 发送事件（插件 → 上位机）

### 设置按键图标

```js
/**
 * 使用 manifest.json States 数组中定义的状态编号
 * @param {string} context  必传 | 目标按键的唯一标识
 * @param {number} state    必传 | States 数组的索引
 * @param {string} text     可选 | 叠加在图标上的文字
 */
$UD.setStateIcon(context, state, text)

/**
 * 使用自定义图片（base64）
 * @param {string} context  必传
 * @param {string} data     必传 | Base64 编码的图片（PNG/JPG/SVG）
 * @param {string} text     可选
 */
$UD.setBaseDataIcon(context, data, text)

/**
 * 使用本地图片文件路径
 * @param {string} context  必传
 * @param {string} path     必传 | 相对插件根目录的路径
 * @param {string} text     可选
 */
$UD.setPathIcon(context, path, text)

/**
 * 使用自定义动图（base64）
 * @param {string} context  必传
 * @param {string} gifdata  必传 | Base64 编码的 GIF 数据
 * @param {string} text     可选
 */
$UD.setGifDataIcon(context, gifdata, text)

/**
 * 使用本地 GIF 文件路径
 * @param {string} context  必传
 * @param {string} gifpath  必传 | 相对插件根目录的路径
 * @param {string} text     可选
 */
$UD.setGifPathIcon(context, gifpath, text)
```

### 发送参数

```js
/**
 * 向上位机发送配置参数（主服务 → 上位机 → 配置项，或反向）
 * @param {object} settings  必传
 * @param {string} context   由主服务发出时必传
 */
$UD.sendParamFromPlugin(settings, context)

/**
 * 主服务 → 配置项：透传数据（不由上位机保存）
 * @param {object} settings  必传
 * @param {string} context   必传 | 目标 action 的 context
 */
$UD.sendToPropertyInspector(settings, context)

/**
 * 配置项 → 主服务：透传数据（不由上位机保存）
 * @param {object} settings  必传
 */
$UD.sendToPlugin(settings)
```

### Settings 持久化

```js
/**
 * 保存 action 级别的设置。两端均会触发 didReceiveSettings。
 * 注意：action 未激活时设置不会被保存。
 * @param {object} settings  必传
 * @param {string} context   由主服务发出时必传
 */
$UD.setSettings(settings, context)

/**
 * 请求已保存的 action 设置。响应通过 onDidReceiveSettings 返回。
 * @param {string} context   由主服务发出时必传
 */
$UD.getSettings(context)

/**
 * 保存插件全局设置。所有已连接页面均会触发 didReceiveGlobalSettings。
 * @param {object} settings  必传
 * @param {string} context   可选
 */
$UD.setGlobalSettings(settings, context)

/**
 * 请求全局设置。响应通过 onDidReceiveGlobalSettings 返回。
 * @param {string} context   可选
 */
$UD.getGlobalSettings(context)
```

### 系统功能

```js
/**
 * 在 UlanziStudio 上位机显示 Toast 提示
 * @param {string} msg  必传
 */
$UD.toast(msg)

/**
 * 在按键上显示错误指示（短暂动画）
 * @param {string} context  由主服务发出时必传
 */
$UD.showAlert(context)

/**
 * 向插件日志文件写入消息
 * 日志路径：~/AppData/Roaming/Ulanzi/UlanziStudio/logs/{主服务UUID}.log
 * @param {string} msg    必传
 * @param {string} level  可选 | 'info' | 'debug' | 'warn' | 'error'（默认：'info'）
 */
$UD.logMessage(msg, level)

/**
 * 触发系统级快捷键
 * Mac：使用 ^、⌘、⌥、⇧ 作为修饰键（如 '⌘C'）
 * Windows：使用 Ctrl+C 格式（如 'Ctrl+C'）
 * @param {string} key  必传
 */
$UD.hotkey(key)

/**
 * 用系统浏览器打开 URL
 * @param {string}  url    必传 | 不能包含查询参数，请通过 param 传递
 * @param {boolean} local  可选 | 本地文件路径时为 true
 * @param {object}  param  可选 | 查询参数
 */
$UD.openUrl(url, local, param)

/**
 * 以弹窗形式打开本地 HTML 文件
 * 在 HTML 内部调用 window.close() 可关闭弹窗
 * @param {string} url    必传 | 本地 HTML 路径（不含查询参数，请用 param 传递）
 * @param {number} width  可选 | 默认 200
 * @param {number} height 可选 | 默认 200
 * @param {number} x      可选 | 窗口 x 坐标；不传则居中
 * @param {number} y      可选 | 窗口 y 坐标；不传则居中
 * @param {object} param  可选 | 传递给 HTML 文件的参数
 */
$UD.openView(url, width, height, x, y, param)

/**
 * 打开文件选择对话框
 * @param {string} filter  可选 | 如 'image(*.jpg *.png *.gif)' 或 'file(*.txt *.json)'
 * 结果通过 onSelectdialog 返回
 */
$UD.selectFileDialog(filter)

/**
 * 打开文件夹选择对话框
 * 结果通过 onSelectdialog 返回
 */
$UD.selectFolderDialog()
```

---

## Utils API

`Utils` 是从 `index.js` 导出的单例对象。

```js
/**
 * 获取插件根目录路径（以 *.ulanziPlugin 结尾的文件夹）
 * 兼容 Windows 和 macOS
 * @returns {string}
 */
Utils.getPluginPath()

/**
 * 获取当前操作系统类型
 * @returns {'windows' | 'mac'}
 */
Utils.getSystemType()

/**
 * 将语言代码规范化为支持的区域字符串
 * 例：'zh-CN' → 'zh_CN'，'en-US' → 'en'
 * @param {string} ln
 * @returns {string}
 */
Utils.adaptLanguage(ln)

/**
 * 安全解析 JSON 字符串；解析失败时返回 false
 * @param {string} jsonString
 * @returns {object|false}
 */
Utils.parseJson(jsonString)

/**
 * 对函数进行防抖处理
 * @param {function} fn
 * @param {number}   wait  延迟时间，单位 ms（默认：150）
 * @returns {function}
 */
Utils.debounce(fn, wait)

/**
 * 通过点分隔的键路径获取嵌套属性值
 * 支持数组下标表示法：'list[0].name'
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
