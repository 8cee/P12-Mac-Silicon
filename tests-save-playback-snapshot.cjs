'use strict';
// Exercise production saveProject through its real async VST state-capture gap.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('P-12.html','utf8');
const a=html.indexOf('  async function saveProject(){');
const b=html.indexOf('  // P-12 v1.0 Tune BF',a);
assert(a>=0 && b>a);
const functionCode=html.slice(a,b);
let captured=null,resolveVst;
const savePromise=new Promise(resolve=>{resolveVst=resolve;});
const diagnostics=[];
const fakeState={currentPattern:1,currentBank:'A',playing:true,samples:{},patterns:{1:{pianoRollNotes:[{note:60,tick:6,length:2}]}},pianoRollNotes:[{note:60,tick:6,length:2}],chain:[]};
const context=vm.createContext({
 console,performance:{now:()=>1000},state:fakeState,normalizeLoadedPadOccupancy(){},
 document:{getElementById:()=>({textContent:''}),createElement:()=>({click(){},remove(){}}),body:{appendChild(){}}},
 Blob:class {constructor(parts){this.parts=parts;}},
 URL:{createObjectURL(blob){captured=JSON.parse(blob.parts[0]);return 'blob:project'},revokeObjectURL(){}},
 setTimeout(){},p12DiagSend(...args){diagnostics.push(args)},
 p12ProjectFxSnapshot:()=>({bitDepth:12,drive:20}),
 captureVstPluginStates:()=>savePromise,
 serializePatternsForProject:()=>JSON.parse(JSON.stringify(fakeState.patterns)),
 PROJECT_FORMAT_VERSION:5,NUM_PATTERNS:100,
});
vm.runInContext(functionCode+'\nglobalThis.saveProjectUnderTest=saveProject;',context);
(async()=>{
 const pending=context.saveProjectUnderTest();
 // JS playback/MIDI events are free to change the live pattern while plugins
 // perform asynchronous serialization; save must preserve the earlier image.
 fakeState.patterns[1].pianoRollNotes.push({note:62,tick:12,length:1});
 fakeState.pianoRollNotes.push({note:62,tick:12,length:1});
 fakeState.currentPattern=2;fakeState.currentBank='B';
 resolveVst({plugin:'state'});
 await pending;
 assert(captured);
 assert.equal(captured.currentPattern,1);assert.equal(captured.currentBank,'A');
 assert.equal(captured.patterns[1].pianoRollNotes.length,1);
 assert.equal(captured.pianoRollNotes.length,1);
 assert.equal(captured.pianoRollNotes[0].note,60);
 assert.equal(captured.vstPluginStates.plugin,'state');
 assert.equal(fakeState.playing,true,'Saving does not stop transport');
 assert.deepEqual(diagnostics.map(e=>e[1]),['save-start','save-ok']);
 console.log('PASS project save remains snapshot-consistent during asynchronous VST capture and live MIDI changes');
})().catch(e=>{console.error(e);process.exitCode=1;});
