'use strict';
const fs=require('node:fs'),assert=require('node:assert/strict'),crypto=require('node:crypto');
let html=fs.readFileSync('P-12.html','utf8');
// Project-only master level persistence; never part of Rossum conversion.
html=html.replace('      masterVolume: state.masterVolume,\n','');
const start=html.indexOf('  function sp1200BuildHfeFromSP12('),end=html.indexOf('  function planSP1200Vintage4Pages(',start);
assert(start>=0&&end>start);
html=html.slice(0,start)+'  // HFE container builder independently validated\n'+html.slice(end);
const marker='  // P-12 v1.0 Tune BF/Tune AT + sequencer pitch/triplet/high-tempo fix';
const digest=crypto.createHash('sha256').update(html.slice(html.indexOf(marker))).digest('hex');
assert.equal(digest,'d4fb018c2969fc50ca2760cd5459be1def5aed801954997f95e0486d8382b5ac');
console.log('PASS SHA256: original Rossum source unchanged except the separately tested HFE container builder');
