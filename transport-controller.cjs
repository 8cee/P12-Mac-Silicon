"use strict";
// Sole Electron transport owner. Requests are ordered; concurrent duplicates
// share a promise and acknowledged duplicates never cross the host pipe.
class TransportController {
  constructor(send,beforeStart=async()=>{}){this.send=send;this.beforeStart=beforeStart;this.tail=Promise.resolve();this.pending=new Map();this.state={playing:false,bpm:96};this.known=false;this.starting=false;}
  invalidate(){this.known=false;this.state={playing:false,bpm:96};}
  request(payload={}){
    const key=JSON.stringify(payload);
    if(this.pending.has(key))return this.pending.get(key);
    const task=this.tail.then(async()=>{
      const next={bpm:Number(payload.bpm)||this.state.bpm,playing:payload.playing==null?this.state.playing:!!payload.playing};
      const explicitRestart=next.playing&&payload.reset&&payload.restartToken&&payload.restartToken!==this.state.restartToken;
      if(this.known&&next.bpm===this.state.bpm&&next.playing===this.state.playing&&!explicitRestart)return {ok:true,redundant:true,playing:next.playing};
      this.starting=next.playing&&!this.state.playing;
      try{
        if(this.starting)await this.beforeStart();
        const result=await this.send({...next,reset:!!explicitRestart,restartToken:payload.restartToken,startEpochMs:payload.startEpochMs,pause:!!payload.pause,resume:!!payload.resume});
        if(result?.ok){this.state={...next,restartToken:explicitRestart?payload.restartToken:this.state.restartToken};this.known=true;}
        return result;
      }finally{this.starting=false;}
    });
    this.pending.set(key,task);this.tail=task.catch(()=>{});
    task.finally(()=>{if(this.pending.get(key)===task)this.pending.delete(key);}).catch(()=>{});
    return task;
  }
}
module.exports={TransportController};
