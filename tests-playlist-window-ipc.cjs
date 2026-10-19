'use strict';
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('P-12.html','utf8'), main=fs.readFileSync('main.js','utf8');
// Exercise the actual Electron window handlers using a BrowserWindow double.
const handlers={},listeners={},sent=[];let options,loads=[];
class Window {constructor(o){options=o;this.webContents={send:(...x)=>sent.push(x),setWindowOpenHandler:()=>{}};}removeMenu(){}on(){}async loadFile(p){loads.push(p);}show(){}focus(){}isDestroyed(){return false;}close(){this.closed=true;}}
const mainWindow={webContents:{send:(...x)=>sent.push(x)}};
const context={mainWindow,BrowserWindow:Window,ipcMain:{handle:(k,f)=>handlers[k]=f,on:(k,f)=>listeners[k]=f},path:require('node:path'),__dirname:process.cwd()};
vm.createContext(context);
vm.runInContext(main.slice(main.indexOf('let mixerWindow=null;'),main.indexOf('let pianoWindow=null;')),context);
(async()=>{
 const event={sender:mainWindow.webContents};assert.equal((await handlers['p12:playlist-open']({sender:{}})).ok,false);
 assert.equal((await handlers['p12:playlist-open'](event)).ok,true);
 assert.equal(options.resizable,true);assert.equal(options.webPreferences.contextIsolation,true);assert.equal(options.webPreferences.nodeIntegration,false);assert(loads[0].endsWith('playlist-detached.html'));
 await handlers['p12:playlist-open'](event);assert.equal(loads.length,1);
 listeners['p12:playlist-action']({sender:{}},{type:'play'});assert.equal(sent.length,0);
 const sender=vm.runInContext('playlistWindow.webContents',context);listeners['p12:playlist-action']({sender},{type:'range',index:1,bars:1});assert.equal(sent[0][0],'p12:playlist-action');
 listeners['p12:playlist-state']({sender:mainWindow.webContents},{clips:[]});assert.equal(sent[1][0],'p12:playlist-state');
 assert(JSON.parse(fs.readFileSync('package.json','utf8')).build.files.includes('playlist-detached.html'));
 const child=fs.readFileSync('playlist-detached.html','utf8');for(const m of child.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g))new vm.Script(m[1]);
 console.log('PASS Playlist Electron IPC: resizable secure child, single window, sender isolation, state/action routing, detached script syntax');
})().catch(e=>{console.error(e);process.exit(1);});
