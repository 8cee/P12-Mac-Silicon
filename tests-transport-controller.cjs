'use strict';
const assert=require('node:assert/strict');const {TransportController}=require('./transport-controller.cjs');
(async()=>{
 const calls=[];let saves=0;const controller=new TransportController(async p=>{calls.push(p);await new Promise(r=>setTimeout(r,1));return {ok:true};},async()=>{saves++;});
 await Promise.all(Array.from({length:100},()=>controller.request({bpm:96,playing:true,reset:true,restartToken:'play-1'})));
 assert.equal(calls.length,1);assert.equal(saves,1);
 await controller.request({bpm:96,playing:true});await controller.request({bpm:96,playing:true,reset:true,restartToken:'play-1'});
 assert.equal(calls.length,1);
 await controller.request({bpm:110});assert.equal(calls.at(-1).playing,true);assert.equal(calls.at(-1).reset,false);
 await Promise.all([controller.request({playing:false,bpm:110}),controller.request({playing:true,bpm:110,restartToken:'play-2',reset:true})]);
 assert.deepEqual(calls.slice(-2).map(x=>x.playing),[false,true]);
 controller.invalidate();await controller.request({playing:false});assert.equal(calls.at(-1).playing,false);
 let release;const blocked=new TransportController(async p=>{assert.equal(done,true);return {ok:true};},()=>new Promise(r=>{release=r;}));let done=false;
 const starting=blocked.request({playing:true});await new Promise(r=>setImmediate(r));done=true;release();await starting;
 console.log('PASS transport: 100 duplicate STARTs coalesce, redundant SYNC ignored, BPM preserves playing, ordered STOP/START, host restart, recovery barrier');
})().catch(e=>{console.error(e);process.exit(1);});
