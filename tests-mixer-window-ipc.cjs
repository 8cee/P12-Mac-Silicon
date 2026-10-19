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
 const event={sender:mainWindow.webContents};assert.equal((await handlers['p12:mixer-open']({sender:{}})).ok,false);
 assert.equal((await handlers['p12:mixer-open'](event)).ok,true);
 assert.equal(options.resizable,true);assert.equal(options.webPreferences.contextIsolation,true);assert.equal(options.webPreferences.nodeIntegration,false);assert(loads[0].endsWith('mixer-detached.html'));
 await handlers['p12:mixer-open'](event);assert.equal(loads.length,1);
 listeners['p12:mixer-action']({sender:{}},{type:'play'});assert.equal(sent.length,0);
 const el={dataset:{},style:{},addEventListener(){}};let value;
 vm.runInContext(html.slice(html.indexOf('  function wireMiniKnob('),html.indexOf('  let mixerWinSelectedFid')),context);
 context.wireMiniKnob(el,-8,7,0,v=>value=v);el.p12SetValue(4);assert.equal(value,4);assert.equal(el.dataset.p12KnobValue,'4');el.p12SetValue(99);assert.equal(value,7);
 const child=fs.readFileSync('mixer-detached.html','utf8');for(const m of child.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g))new vm.Script(m[1]);
 console.log('PASS Mixer Electron IPC: resizable secure child, single window, sender isolation, original knob callbacks and bounds, detached script syntax');
})().catch(e=>{console.error(e);process.exit(1);});
