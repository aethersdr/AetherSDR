

import { RandomPort, UlanzideckApi, Utils } from 'ulanzideck-api';


const generatePort = new RandomPort(); 
//生成随机接口
const port = generatePort.getPort(); 
//获取根目录文件路径
const _pluginPath = Utils.getPluginPath()

console.log('Random port: ', port)
console.log('Plugin path: ', _pluginPath)
console.log('UlanzideckApi loaded');




const $UD = new UlanzideckApi();
//socket 连接
$UD.connect('com.ulanzi.ulanzideck.analogclock.clock')

$UD.onConnected(conn => {

  console.log('=onConnected=')
})

$UD.onAdd(message => {

  console.log('onAdd', message)

})

$UD.onClear(message => {

  console.log('onClear', message)
})
$UD.onClose(message => {

  console.log('=onClose=', message)
})
