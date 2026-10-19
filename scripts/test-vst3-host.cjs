'use strict';
const {spawn}=require('node:child_process'),path=require('node:path'),assert=require('node:assert/strict');
const host=spawn(path.resolve(process.argv[2]),['--json-lines'],{stdio:['pipe','pipe','inherit']});
const pending=new Map();let seq=0,buffer='';
function failAll(error){for(const p of pending.values()){clearTimeout(p.timer);p.reject(error);}pending.clear();}
host.on('error',failAll);host.on('exit',code=>failAll(new Error('Host exited: '+code)));
host.stdout.on('data',chunk=>{
  buffer+=chunk.toString();let nl;
  while((nl=buffer.indexOf('\n'))>=0){const line=buffer.slice(0,nl);buffer=buffer.slice(nl+1);let reply;try{reply=JSON.parse(line);}catch{continue;}
    const p=pending.get(reply.replyTo);if(p){clearTimeout(p.timer);pending.delete(reply.replyTo);reply.ok?p.resolve(reply):p.reject(new Error(reply.message));}
  }
});
function request(command,payload={}){return new Promise((resolve,reject)=>{const id='test-'+(++seq);const timer=setTimeout(()=>{pending.delete(id);reject(new Error(command+' timeout'));},20000);pending.set(id,{resolve,reject,timer});host.stdin.write(JSON.stringify({id,command,payload})+'\n');});}
(async()=>{
  const status=await request('status');assert.equal(status.vst3,true);
  const plugin=path.resolve(process.argv[3]);
  for(const [target,id] of [['pad','A_1'],['track','0'],['master','0']]){
    const route={target,id,slot:1,path:plugin,format:'VST3',enabled:true};
    await request('setMixerInsertVST',route);
    const samples=Buffer.alloc(256*2*4);for(let n=0;n<512;n++)samples.writeFloatLE(0.4,n*4);
    const packet={target,id,frames:256,sampleRate:48000,pcm:samples.toString('base64')};
    const wet=await request('processPCM',packet);assert.equal(wet.processedPlugins,1);
    const output=Buffer.from(wet.pcm,'base64');for(let n=0;n<512;n++)assert(Math.abs(output.readFloatLE(n*4)-0.2)<1e-5,'VST3 gain must process actual PCM');
    await request('openEditor',route);await request('closeEditor',route);
    await request('setMixerInsertVST',{...route,enabled:false});
    const dry=await request('processPCM',packet);assert.equal(dry.processedPlugins,0);assert.equal(dry.pcm,packet.pcm);
    await request('setMixerInsertVST',{...route,path:''});
  }
  console.log('PASS native VST3: host startup, pad/track-0/master DSP, editor open/close, bypass and unload');
})().catch(err=>{console.error(err);process.exitCode=1;}).finally(()=>{host.stdin.end();setTimeout(()=>host.kill(),2000).unref();});
