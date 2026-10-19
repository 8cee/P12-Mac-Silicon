"use strict";
const {workerData,parentPort}=require('node:worker_threads');
const fs=require('node:fs'),path=require('node:path');
const {target,payload}=workerData;
try{
  fs.mkdirSync(path.dirname(target),{recursive:true});
  const text=JSON.stringify(payload),tmp=target+'.tmp-'+process.pid;
  // Windows FlushFileBuffers needs write access to the file handle.
  fs.writeFileSync(tmp,text);const fd=fs.openSync(tmp,'r+');try{fs.fsyncSync(fd);}finally{fs.closeSync(fd);}
  for(let i=3;i>=1;i--){const src=i===1?target:target.replace(/\.p12$/,'.'+(i-1)+'.p12'),dst=target.replace(/\.p12$/,'.'+i+'.p12');if(fs.existsSync(src)){fs.rmSync(dst,{force:true});fs.renameSync(src,dst);}}
  fs.renameSync(tmp,target);parentPort.postMessage({ok:true,path:target,bytes:Buffer.byteLength(text)});
}catch(err){parentPort.postMessage({ok:false,message:err.message});}
