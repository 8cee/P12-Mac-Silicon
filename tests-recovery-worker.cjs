'use strict';
const {Worker}=require('node:worker_threads'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{const dir=fs.mkdtempSync(path.join(os.tmpdir(),'p12-recovery-')),target=path.join(dir,'P12_RECOVERY.p12');try{
 // Simulate Windows rejecting fsync on a read-only handle, even on Linux CI.
 const workerCode=`const fs=require('node:fs'),handles=new Map(),open=fs.openSync,flush=fs.fsyncSync,close=fs.closeSync;
 fs.openSync=(name,flags,...rest)=>{const fd=open(name,flags,...rest);handles.set(fd,flags);return fd;};
 fs.fsyncSync=fd=>{if(handles.get(fd)==='r'){const e=new Error('EPERM: operation not permitted, fsync');e.code='EPERM';throw e;}return flush(fd);};
 fs.closeSync=fd=>{handles.delete(fd);return close(fd);};
 require(require('node:worker_threads').workerData.workerPath);`;
 for(let i=0;i<5;i++){const result=await new Promise((resolve,reject)=>{const w=new Worker(workerCode,{eval:true,workerData:{workerPath:path.resolve('recovery-worker.cjs'),target,payload:{patterns:{1:{bpm:96}},serial:i}}});w.once('message',resolve);w.once('error',reject);});assert(result.ok,result.message);}
 assert.equal(JSON.parse(fs.readFileSync(target)).serial,4);for(let i=1;i<=3;i++)assert.equal(JSON.parse(fs.readFileSync(path.join(dir,'P12_RECOVERY.'+i+'.p12'))).serial,4-i);
 console.log('PASS recovery worker: Windows writable fsync, atomic completed snapshot and three chronological recovery versions');
 }finally{fs.rmSync(dir,{recursive:true,force:true});}})().catch(e=>{console.error(e);process.exit(1);});
