'use strict';
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const assert=require('node:assert/strict'),cp=require('node:child_process');
process.chdir(path.join(__dirname,'..'));
const pkg=JSON.parse(fs.readFileSync('package.json','utf8'));
for(const file of pkg.build.files){
  assert(fs.statSync(file).size>0,`Missing package resource: ${file}`);
  if(/\.(js|cjs)$/.test(file))new vm.Script(fs.readFileSync(file,'utf8'),{filename:file});
  if(file.endsWith('.html'))for(const m of fs.readFileSync(file,'utf8').matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/gi))new vm.Script(m[1],{filename:file});
}
assert.equal(pkg.build.appId,'com.p12.sampler.silicon');
assert(pkg.build.extraResources.some(r=>r.from==='native/P-12 VST Host.app'),'Package the native Mac host');
assert(pkg.build.files.includes(pkg.build.mac.icon));
const exposed={};
const electron={contextBridge:{exposeInMainWorld:(key,value)=>exposed[key]=value},ipcRenderer:{invoke:()=>Promise.resolve({ok:true}),on:()=>{},send:()=>{}}};
vm.runInNewContext(fs.readFileSync('preload.js','utf8'),{process:{platform:'darwin'},require:name=>{assert.equal(name,'electron');return electron;}});
assert.equal(exposed.electronAPI.nativeHosting,true);
for(const method of ['safeSaveText','safeSaveBytes','openMixerWindow','openPlaylistWindow','openPianoWindow'])assert.equal(typeof exposed.electronAPI[method],'function');
const html=fs.readFileSync('P-12.html','utf8');
const c=vm.createContext({window:{electronAPI:exposed.electronAPI}});
vm.runInContext(html.match(/  function nativeBridge\(\)\{[\s\S]*?\n  \}/)[0],c);
assert.equal(c.nativeBridge(),exposed.electronAPI,'Mac edition must enable the native audio bridge');
const baseline=JSON.parse(fs.readFileSync('SOURCE_BASELINE.json','utf8'));
const original=html.replace("return b&&b.nativeHosting!==false&&typeof b.nativeAudioCommand==='function'?b:null;","return b&&typeof b.nativeAudioCommand==='function'?b:null;");
assert.equal(require('node:crypto').createHash('sha256').update(original).digest('hex'),baseline.originalHtmlSha256,'App source must match uploaded baseline byte-for-byte');
console.log('PASS package resources, script syntax, preload, native host selection and uploaded source integrity');
for(const test of ['tests-mac-host-launch.cjs','tests-playlist-window-ipc.cjs','tests-mixer-window-ipc.cjs','tests-recovery-worker.cjs','tests-transport-controller.cjs','tests-save-playback-snapshot.cjs','tests-midi-input-recovery.cjs','tests-hfe-hardware-layout.cjs','tests-rossum-encoder-unchanged.cjs'])cp.execFileSync(process.execPath,[test],{stdio:'inherit',timeout:30000});
console.log('PASS Mac Silicon distribution preflight');
