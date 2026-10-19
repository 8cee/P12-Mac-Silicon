'use strict';
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path'),assert=require('node:assert/strict');
const source=fs.readFileSync('main.js','utf8');
const native='/Applications/P-12.app/Contents/Resources/native/P-12 VST Host.app/Contents/MacOS/P-12 VST Host';
let launched;
const child={killed:false,stdout:{on(){}},stderr:{on(){}},on(){}};
const ctx=vm.createContext({process:{platform:'darwin',env:{},resourcesPath:'/Applications/P-12.app/Contents/Resources'},__dirname:'/Applications/P-12.app/Contents/Resources/app.asar',path,
  fs:{existsSync:p=>p===native,copyFileSync:()=>{throw new Error('Must not copy a signed Mac host');}},
  nativeHost:null,nativeHostPath:null,nativeHostFailed:null,nativeHostStdoutBuffer:'',
  p12Transport:{invalidate(){}},p12Log(){},p12Diag(){},handleNativeStdout(){},failAllNativePending(){},
  spawn:(file,args,options)=>{launched={file,args,options};return child;}
});
vm.runInContext(source.slice(source.indexOf('function findNativeHost()'),source.indexOf('function sendNativeFast(')),ctx);
assert.equal(ctx.ensureNativeHost().ok,true);assert.equal(launched.file,native);
assert.equal(launched.options.cwd,path.dirname(native));assert.deepEqual(Array.from(launched.options.stdio),['pipe','pipe','pipe']);
assert.equal(ctx.ensureNativeHost().ok,true);
console.log('PASS packaged Mac VST host resolves inside Resources and launches in place without copying its signed bundle');
