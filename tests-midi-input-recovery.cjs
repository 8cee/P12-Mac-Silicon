'use strict';
// Exercises the actual production MIDI release function in a deterministic VM.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('P-12.html','utf8');
const start=html.indexOf('  function pianoReleaseIncomingNotes(');
const end=html.indexOf('  function pianoMidiMessage(',start);
assert(start>0&&end>start);
const fn=html.slice(start,end);
const events=[];
const context=vm.createContext({console,clearTimeout:()=>{},window:{electronAPI:{writeDiagnostic:e=>events.push(e)}}});
vm.runInContext(`
const pianoIncomingHeld=new Set(['1:60','1:64','2:67']);
const pianoSustainChannels=new Set();
const pianoSustainedKeys=new Set();
const pianoLiveVoices=new Map([['1:60',{note:60}],['1:64',{note:64}],['2:67',{note:67}]]);
const pianoHeld=new Map([['1:60',{recorded:{note:60}}],['2:67',{recorded:{note:67}}]]);
const pianoChordGesture=new Map([['1:60',{}],['2:67',{}]]);
let pianoChordSettleTimer=null,pianoPendingChord={id:1},pianoDetachedActive=true;
let displays=0,snapshots=0;
const sent=[],finished=[];
function pianoMidiMessage(m){sent.push(m);}
function pianoFinishSpecificHeld(k){finished.push(k);pianoHeld.delete(k);}
function updatePianoLiveChord(){displays++;}
function publishDetached(){snapshots++;}
${fn}
globalThis.check={release:pianoReleaseIncomingNotes,pianoIncomingHeld,pianoLiveVoices,
 pianoHeld,pianoChordGesture,sent,finished,getPending:()=>pianoPendingChord,
 getDisplays:()=>displays,getSnapshots:()=>snapshots};
`,context,{timeout:5000});
const t=context.check;
t.release('midi-all-notes-off',1);
assert.deepEqual(Array.from(t.pianoIncomingHeld),['2:67']);
assert.deepEqual(Array.from(t.pianoHeld.keys()),['2:67']);
assert.deepEqual(Array.from(t.pianoLiveVoices.keys()),['2:67']);
assert.deepEqual(Array.from(t.pianoChordGesture.keys()),['2:67']);
assert.deepEqual(Array.from(t.finished),['1:60']);
assert.deepEqual(Array.from(t.sent.map(e=>e.note).sort()),[60,64]);
assert.equal(events.length,1);assert.equal(events[0].details.count,2);
console.log('PASS MIDI CC 123 / 120 release only selected channel');
t.release('midi-disconnect');
assert.equal(t.pianoIncomingHeld.size,0);
assert.equal(t.pianoLiveVoices.size,0);
assert.equal(t.pianoHeld.size,0);
assert.equal(t.pianoChordGesture.size,0);
assert.equal(t.getPending(),null);
assert.equal(events.length,2);assert.equal(events[1].details.count,1);
console.log('PASS MIDI input disconnect releases remaining held notes and chord display');
t.release('midi-disconnect');
assert.equal(events.length,2,'No redundant diagnostic after empty release');
assert.equal(t.sent.length,3,'No duplicate Note Off on repeated disconnect');
console.log('PASS repeated MIDI release is idempotent and diagnostics coalesced');
assert(html.includes("midiAccess.onstatechange = event=>"));
assert(html.includes("pianoReleaseIncomingNotes('midi-all-notes-off',midiInChannel+1)"));
console.log('PASS production MIDI state-change and CC handlers connected');
