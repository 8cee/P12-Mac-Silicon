'use strict';
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=fs.readFileSync('P-12.html','utf8');
const a=source.indexOf('  function sp1200BitReverse8('),b=source.indexOf('  function planSP1200Vintage4Pages(',a);
const constants={SP12_NAME_OFFSET:0x9114,SP12_AUDIO_OFFSET:0x94e2,SP12_PAGE_SAMPLES:65536,SP12_DIR_OFFSET:0x9260,SP12_DIR_RECORD_BYTES:10,SP12_PAGE_USED_OFFSETS:[0x9258,0x925a,0x9254,0x9256,0x9110,0x9112,0x925c,0x925e],SP1200_HFE_SECTOR_BYTES:3584};
const context=vm.createContext({...constants,Uint8Array,DataView,SP1200_HFE_BASE_B64:source.match(/const SP1200_HFE_BASE_B64\s*=\s*'([^']+)'/)[1],SP1200_HFE_RAW_BASE_B64:source.match(/const SP1200_HFE_RAW_BASE_B64\s*=\s*'([^']+)'/)[1],base64ToArrayBuffer:s=>Uint8Array.from(Buffer.from(s,'base64')).buffer});
vm.runInContext(source.slice(a,b),context);
const reverse=v=>{let r=0;for(let i=0;i<8;i++){r=(r<<1)|(v&1);v>>>=1;}return r;};
function decode(hfe){
 const lut=hfe.readUInt16LE(18)*512,result=[];
 for(let sector=0;sector<160;sector++){
  const track=Math.floor(sector/2),side=sector%2,base=hfe.readUInt16LE(lut+track*4)*512;
  const byte=pos=>hfe[base+Math.floor(pos/256)*512+side*256+pos%256];
  const read=n=>{let value=0;for(let k=0;k<4;k++){const z=reverse(byte(176+n*4+k));value=(value<<2)|((z&16)?2:0)|((z&1)?1:0);}return value;};
  const data=Buffer.alloc(3584);let crc=0;
  for(let i=0;i<data.length;i++){
   const v=read(i);data[i]=reverse(v);crc^=v<<8;
   for(let k=0;k<8;k++) crc=(crc&0x8000)?((crc<<1)^0x8005)&65535:(crc<<1)&65535;
  }
  assert.equal(crc,(read(3584)<<8)|read(3585),'FM CRC sector '+sector);
  result.push(data);
 }
 return result;
}
const audioPage=(sectors,page)=>Buffer.concat(sectors.slice(24+page*28,24+(page+1)*28).map(x=>x.subarray(0,3552))).subarray(0,98304);
const word=(audio,n)=>{const off=Math.floor(n/2)*3;return (audio[off+n%2]<<4)|((audio[off+2]>>(n%2*4))&15);};
const image=new Uint8Array(824546),view=new DataView(image.buffer),hardwarePages=[0,1,6,7];
for(let n=0;n<32;n++){
 const off=constants.SP12_DIR_OFFSET+n*10,page=n%4;
 view.setUint16(off+2,((128+page)<<8)|n,true);view.setUint16(off+4,n*100,true);view.setUint16(off+6,(9-500)&65535,true);
 image.set(Buffer.from(('DCBA'[Math.floor(n/8)]+(n%8+1)+'PAD').padEnd(6,' ')),constants.SP12_NAME_OFFSET+n*6);
}
for(let page=0;page<4;page++){
 view.setUint16(constants.SP12_PAGE_USED_OFFSETS[page],4000+page,true);
 for(let i=0;i<98304;i++) image[constants.SP12_AUDIO_OFFSET+page*98304+i]=(i*37+Math.floor(i/251)*19+page*53)&255;
}
const before=Buffer.from(image),hfe=Buffer.from(context.sp1200BuildHfeFromSP12(image)),sectors=decode(hfe),original=decode(Buffer.from(context.SP1200_HFE_BASE_B64,'base64'));
assert.deepEqual(Buffer.from(image),before,'SP12 input stays unchanged');
for(let i=0;i<160;i++) if(i<12||i>=136) assert.deepEqual(sectors[i],original[i],'OS/unused sector preserved '+i);
const sequence=Buffer.concat(sectors.slice(12,23));
assert.deepEqual(sequence.subarray(0,0x9114-0x10),before.subarray(0x10,0x9114),'sequence RAM mapped from offset 0x10');
assert.deepEqual(sequence.subarray(0x9114-0x10),Buffer.concat(original.slice(12,23)).subarray(0x9114-0x10),'sequence transfer padding preserved');
assert.deepEqual(sectors[23].subarray(974),original[23].subarray(974),'sound-header trailing bytes preserved');
for(let n=0;n<32;n++){
 const off=constants.SP12_DIR_OFFSET-constants.SP12_NAME_OFFSET+n*10;
 assert.equal(sectors[23].readUInt16LE(off+2),((128+hardwarePages[n%4])<<8)|n,'pad/page '+n);
 assert.deepEqual(sectors[23].subarray(n*6,n*6+6),before.subarray(constants.SP12_NAME_OFFSET+n*6,constants.SP12_NAME_OFFSET+n*6+6),'pad name '+n);
}
for(let page=0;page<4;page++){
 assert.deepEqual(audioPage(sectors,page),before.subarray(constants.SP12_AUDIO_OFFSET+page*98304,constants.SP12_AUDIO_OFFSET+(page+1)*98304),'complete audio page '+page);
 assert.equal(sectors[23].readUInt16LE(constants.SP12_PAGE_USED_OFFSETS[hardwarePages[page]]-constants.SP12_NAME_OFFSET),4000+page,'page-used mapping');
 for(let part=0;part<27;part++) assert.deepEqual(sectors[24+page*28+part].subarray(3552),sectors[25+page*28+part].subarray(0,32),'sector overlap');
}
if(process.argv.length===4){
 const ref=decode(fs.readFileSync(process.argv[2])),saved=fs.readFileSync(process.argv[3]);
 assert.deepEqual(ref[23].subarray(0,974),saved.subarray(0x9114,0x94e2),'original HFE / Rossum header');
 let sounds=0,samples=0;
 for(let n=0;n<32;n++){
  const off=0x9260+n*10,counter=saved.readUInt16LE(off+6);if(counter===65533)continue;
  const page=(saved.readUInt16LE(off+2)>>8)-128,start=saved.readUInt16LE(off+4),length=(9-counter)&65535,disk=hardwarePages.indexOf(page);
  assert(disk>=0,'reference page');const diskAudio=audioPage(ref,disk),spAudio=saved.subarray(0x94e2+page*98304,0x94e2+(page+1)*98304);
  for(let i=0;i<length;i++)assert.equal(word(diskAudio,start+i),word(spAudio,start+i),'reference pad '+n+' sample '+i);
  sounds++;samples+=length;
 }
 console.log('PASS physical reference: '+sounds+' sounds / '+samples+' samples match exactly; all 160 FM CRCs valid');
}
console.log('PASS HFE layout: all 32 pad identities, four full audio pages, overlaps, CRCs, OS preservation; SP12 input unchanged');
