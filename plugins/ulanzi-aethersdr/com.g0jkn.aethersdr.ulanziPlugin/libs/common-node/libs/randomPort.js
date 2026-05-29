import crypto from 'crypto';
import fs from 'fs';

export default class RandomPort {

    constructor(minPort = 49152, maxPort = 65535) {
        this.minPort = minPort;
        this.maxPort = maxPort;
    }

    getPort(){
        
        // 生成随机端口
        this.randomPort = this.generateRandomPort(this.minPort, this.maxPort);

        //将接口写入文件供前端调用
        this.writePort()
        
        return this.randomPort
    }
    
    generateRandomPort(minPort = 49152, maxPort = 65535) {
        const range = maxPort - minPort + 1;
        const randomValue = crypto.randomInt(range);
        return minPort + randomValue;
    }

    writePort() {

        // 文件路径
        const currentFilePath = process.argv[1];
        
        let split_tag = '/'
        if(currentFilePath.indexOf('\\') > -1){
          split_tag = '\\'
        }
        const pathArr = currentFilePath.split(split_tag);
        const idx = pathArr.findIndex(f => f.endsWith('ulanziPlugin'));
        const __folderpath = `${pathArr.slice(0, idx + 1).join("/")}/`;

        const filePath = __folderpath + 'ws-port.js'; // 文件路径
        const data = `window.__port = ${this.randomPort};`; // 要写入的数据


        try {
            fs.writeFileSync(filePath, data, 'utf8');
            console.log('文件已成功写入');
            // 返回随机端口
            return this.randomPort
        } catch (err) {
            
            console.error('写入文件时发生错误:', err);
            
            // 返回随机端口
            return ''
        }

     }
     

}
