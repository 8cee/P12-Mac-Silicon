'use strict';
const { app, BrowserWindow, dialog, ipcMain, screen, Menu } = require('electron');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { Worker } = require('node:worker_threads');
const { TransportController } = require('./transport-controller.cjs');
let p12RecoveryWritePromise=null;
const p12Transport=new TransportController(payload=>requestNative('nativeTransport',payload),async()=>{if(p12RecoveryWritePromise)await p12RecoveryWritePromise;});
const P12_SMOKE_TEST = process.argv.includes('--p12-smoke-test');
if(P12_SMOKE_TEST)setTimeout(()=>{console.error('SMOKE TEST TIMEOUT');app.exit(1);},30000).unref();

// Packaged Electron resources live inside a read-only app.asar. Write diagnostics
// to the writable Windows userData folder so startup failures can be reported.
let electronLogPath;
try {
  electronLogPath = path.join(app.getPath('userData'), 'P12_ELECTRON.log');
  fs.mkdirSync(path.dirname(electronLogPath), { recursive: true });
} catch (_) { electronLogPath = path.join(__dirname, 'P12_ELECTRON.log'); }
function p12Log(...parts) {
  const line = `[${new Date().toISOString()}] ${parts.map(v => {
    if (v instanceof Error) return v.stack || v.message || String(v);
    if (typeof v === 'string') return v;
    try { return JSON.stringify(v); } catch { return String(v); }
  }).join(' ')}\n`;
  try { fs.appendFileSync(electronLogPath, line, 'utf8'); } catch {}
  try { console.log(line.trimEnd()); } catch {}
}
try { fs.writeFileSync(electronLogPath, `P-12 Electron startup ${new Date().toISOString()}\n`, 'utf8'); } catch {}
process.on('uncaughtException', err => p12Log('UNCAUGHT EXCEPTION', err));
process.on('unhandledRejection', err => p12Log('UNHANDLED REJECTION', err));

// One central, append-only diagnostic log for the whole P-12 application.
// IMPORTANT: no synchronous disk writes from the native audio callback. Renderer
// and native-host events arrive here on non-realtime threads and are bounded.
let p12DiagnosticStream = null;
let p12DiagnosticSession = `${Date.now()}-${process.pid}`;
let p12DiagnosticLastStatus = null;
let p12DiagnosticLastStatusAt = 0;
const p12PluginFailures=new Map(), p12PluginQuarantine=new Set();
function p12PluginKey(payload){return String(payload&&payload.path||'').toLowerCase();}
function p12RecordPluginFailure(payload,reason){const k=p12PluginKey(payload);if(!k)return;const n=(p12PluginFailures.get(k)||0)+1;p12PluginFailures.set(k,n);if(n>=3){p12PluginQuarantine.add(k);p12Diag('VST','quarantine',{path:payload.path,failures:n,reason},'ERROR');}}

const P12_DIAGNOSTIC_MAX_BYTES = 20 * 1024 * 1024;
function p12DiagnosticPath() { return path.join(app.getPath('userData'), 'P12_DIAGNOSTICS.log'); }
function p12SafeDiag(value, depth = 0) {
  if (depth > 4) return '[depth-limit]';
  if (value == null || typeof value === 'number' || typeof value === 'boolean') return value;
  if (typeof value === 'string') {
    if (value.length > 1600) return value.slice(0,1600) + `…[${value.length}]`;
    if (/^(?:data:|[A-Za-z0-9+/]{4096,}={0,2}$)/.test(value)) return `[large-data:${value.length}]`;
    return value;
  }
  if (ArrayBuffer.isView(value)) return `[typed-array:${value.byteLength}]`;
  if (Array.isArray(value)) return value.slice(0,64).map(v => p12SafeDiag(v, depth + 1));
  if (typeof value === 'object') {
    const out = {};
    for (const [k,v] of Object.entries(value).slice(0,96)) {
      if (/pcm|audioData|bytes|base64|buffer|sampleData|raw/i.test(k) &&
          (typeof v === 'string' || Array.isArray(v) || ArrayBuffer.isView(v))) {
        out[k] = `[omitted:${typeof v === 'string' ? v.length : (v && v.length) || (v && v.byteLength) || 0}]`;
      } else out[k] = p12SafeDiag(v, depth + 1);
    }
    return out;
  }
  return String(value).slice(0,1600);
}
function p12OpenDiagnosticStream() {
  if (p12DiagnosticStream) return p12DiagnosticStream;
  const target = p12DiagnosticPath();
  try {
    fs.mkdirSync(path.dirname(target), {recursive:true});
    try {
      const st=fs.statSync(target);
      if(st.size > P12_DIAGNOSTIC_MAX_BYTES){
        const old=target+'.1'; try{fs.rmSync(old,{force:true});}catch{}
        try{fs.renameSync(target,old);}catch{}
      }
    } catch {}
    p12DiagnosticStream = fs.createWriteStream(target, {flags:'a', encoding:'utf8'});
    p12DiagnosticStream.on('error', err => p12Log('DIAGNOSTIC STREAM ERROR', err));
    p12DiagnosticStream.write(`\n=== P-12 diagnostic session ${p12DiagnosticSession} ${new Date().toISOString()} ===\n`);
  } catch(e) { p12Log('DIAGNOSTIC OPEN ERROR', e); }
  return p12DiagnosticStream;
}
function p12Diag(category, event, details = {}, level = 'INFO') {
  try {
    const stream=p12OpenDiagnosticStream(); if(!stream)return;
    const rec={ts:new Date().toISOString(),session:p12DiagnosticSession,level:String(level).slice(0,12),category:String(category).slice(0,48),event:String(event).slice(0,96),details:p12SafeDiag(details)};
    let line=JSON.stringify(rec);
    if(line.length>12000){ rec.details={truncated:true,originalLength:line.length}; line=JSON.stringify(rec); }
    stream.write(line+'\n');
  } catch(e) { p12Log('DIAGNOSTIC WRITE ERROR', e); }
}
function p12DiagNativeStatus(s) {
  if(!s || s.ok===false)return;
  const now=Date.now();
  const counters=['slowCallbacks','nearDeadlineCallbacks','lateCallbackStarts','audioLockMisses','dryFallbackBlocks','vstCostlyBlocks','oversizePluginCallbacks','lateNativeEvents','lateNativeStarts','lateNativeStops','rejectedVoices'];
  const peaks=['worstCallbackMs','worstOverrunMs','callbackGapPeakMs','mixerPeakMs','masterVstPeakMs','trackVstPeakMs','padVstPeakMs','otherVstPeakMs'];
  let changed=!p12DiagnosticLastStatus;
  if(p12DiagnosticLastStatus) for(const k of counters) if(Number(s[k]||0)!==Number(p12DiagnosticLastStatus[k]||0)){changed=true;break;}
  const spike=peaks.some(k=>Number(s[k]||0)>=8.0) || Number(s.worstOverrunMs||0)>0;
  // Peaks are lifetime maxima: one overrun must not cause 20-30 logs/sec forever.
  const newSpike=spike && (!p12DiagnosticLastStatus || peaks.some(k=>Number(s[k]||0)>Number(p12DiagnosticLastStatus[k]||0)));
  if(((changed || newSpike) && now-p12DiagnosticLastStatusAt>=1000) || now-p12DiagnosticLastStatusAt>=30000){
    const snap={sampleRate:s.sampleRate,blockSize:s.blockSize,activeVoices:s.activeVoices,inputPeak:s.inputPeak,outputPeak:s.outputPeak};
    for(const k of counters.concat(peaks,['liveNativeEvents','liveNativeWorstQueueMs']))snap[k]=s[k];
    p12Diag('AUDIO','native-status',snap,spike?'WARN':'INFO');
    p12DiagnosticLastStatus={...s};p12DiagnosticLastStatusAt=now;
  }
}


// ---------- P-12 safety/recovery file services ----------
function p12AtomicReplace(target, data) {
  const dir=path.dirname(target); fs.mkdirSync(dir,{recursive:true});
  const tmp=target+'.tmp-'+process.pid+'-'+Date.now();
  fs.writeFileSync(tmp,data);
  try { const fh=fs.openSync(tmp,'r+'); try{fs.fsyncSync(fh);}finally{fs.closeSync(fh);} } catch {}
  try { fs.renameSync(tmp,target); }
  catch(err){ try{fs.rmSync(target,{force:true});fs.renameSync(tmp,target);}catch(e){try{fs.rmSync(tmp,{force:true});}catch{};throw e;} }
  return target;
}
function p12RecoveryPath(index=0){return path.join(app.getPath('userData'),index?'P12_RECOVERY.'+index+'.p12':'P12_RECOVERY.p12');}
function p12RotateRecovery(){
  for(let i=3;i>=1;i--){const src=p12RecoveryPath(i-1),dst=p12RecoveryPath(i);try{if(fs.existsSync(src)){fs.rmSync(dst,{force:true});fs.renameSync(src,dst);}}catch{}}
}
ipcMain.handle('p12:safe-save-text',async(_e,payload={})=>{
  const text=String(payload.text??''); const defaultName=String(payload.defaultName||'Project.p12').replace(/[\/:*?"<>|]/g,'_');
  const pick=await dialog.showSaveDialog(mainWindow,{defaultPath:defaultName,filters:Array.isArray(payload.filters)?payload.filters:undefined});
  if(pick.canceled||!pick.filePath)return {ok:false,canceled:true};
  try{p12AtomicReplace(pick.filePath,Buffer.from(text,'utf8'));p12Diag('FILE','atomic-save',{path:pick.filePath,bytes:Buffer.byteLength(text)});return {ok:true,path:pick.filePath};}
  catch(err){p12Diag('FILE','atomic-save-error',{path:pick.filePath,message:err.message},'ERROR');return {ok:false,message:err.message};}
});
ipcMain.handle('p12:safe-save-bytes',async(_e,payload={})=>{
  const bytes=Buffer.from(payload.bytes||[]); const defaultName=String(payload.defaultName||'P12-export.bin').replace(/[\/:*?"<>|]/g,'_');
  const pick=await dialog.showSaveDialog(mainWindow,{defaultPath:defaultName,filters:Array.isArray(payload.filters)?payload.filters:undefined});
  if(pick.canceled||!pick.filePath)return {ok:false,canceled:true};
  try{p12AtomicReplace(pick.filePath,bytes);p12Diag('FILE','atomic-export',{path:pick.filePath,bytes:bytes.length});return {ok:true,path:pick.filePath};}
  catch(err){p12Diag('FILE','atomic-export-error',{path:pick.filePath,message:err.message},'ERROR');return {ok:false,message:err.message};}
});
ipcMain.handle('p12:recovery-write',async(event,payload={})=>{
  if(event.sender!==mainWindow?.webContents||p12Transport.state.playing||p12Transport.starting||p12RecoveryWritePromise)return {ok:false,deferred:true};
  const target=p12RecoveryPath();
  p12RecoveryWritePromise=new Promise(resolve=>{
    const worker=new Worker(path.join(__dirname,'recovery-worker.cjs'),{workerData:{target,payload}});
    worker.once('message',resolve);worker.once('error',err=>resolve({ok:false,message:err.message}));
    worker.once('exit',code=>{if(code!==0)resolve({ok:false,message:'Recovery worker exited '+code});});
  });
  try{const r=await p12RecoveryWritePromise;p12Diag('RECOVERY','autosave',r);return r;}finally{p12RecoveryWritePromise=null;}
});
ipcMain.handle('p12:recovery-read',async()=>{try{const p=p12RecoveryPath(0);if(!fs.existsSync(p))return {ok:true,exists:false};return {ok:true,exists:true,path:p,text:fs.readFileSync(p,'utf8'),mtimeMs:fs.statSync(p).mtimeMs};}catch(err){return {ok:false,message:err.message};}});
ipcMain.handle('p12:recovery-clear',async()=>{for(let i=0;i<=3;i++)try{fs.rmSync(p12RecoveryPath(i),{force:true});}catch{};p12Diag('RECOVERY','cleared');return {ok:true};});
ipcMain.handle('p12:window-state',async(_e,payload={})=>{
  if(!mainWindow)return {ok:false};
  try{
    if(payload&&payload.set){
      const b=payload.set||{};
      let width=Math.max(900,Number(b.width)||1600),height=Math.max(650,Number(b.height)||980);
      let x=Number.isFinite(Number(b.x))?Number(b.x):mainWindow.getBounds().x;
      let y=Number.isFinite(Number(b.y))?Number(b.y):mainWindow.getBounds().y;
      const display=screen.getDisplayMatching({x,y,width,height})||screen.getPrimaryDisplay();
      const wa=display.workArea;
      width=Math.min(width,wa.width);height=Math.min(height,wa.height);
      x=Math.max(wa.x,Math.min(x,wa.x+wa.width-width));
      y=Math.max(wa.y,Math.min(y,wa.y+wa.height-height));
      mainWindow.setBounds({x:Math.round(x),y:Math.round(y),width:Math.round(width),height:Math.round(height)});
    }
    return {ok:true,bounds:mainWindow.getBounds()};
  }catch(err){return {ok:false,message:err.message};}
});
ipcMain.handle('p12:write-performance-snapshot',async(_e,payload={})=>{p12Diag('PERF','snapshot',payload,'INFO');return {ok:true,path:p12DiagnosticPath()};});

// Async diagnostic stream: never block the real-time host or log per audio buffer.
let p12AudioDebugStream = null;
let p12AudioDebugLast = 0;
function p12AudioDebugPath() { return path.join(app.getPath('userData'), 'P12_AUDIO_DEBUG.log'); }
function p12WriteAudioDebug(packet) {
  try {
    if (!p12AudioDebugStream) {
      const target = p12AudioDebugPath();
      fs.mkdirSync(path.dirname(target), {recursive:true});
      p12AudioDebugStream = fs.createWriteStream(target, {flags:'a', encoding:'utf8'});
      p12AudioDebugStream.on('error', err=>p12Log('AUDIO DEBUG STREAM ERROR',err));
      p12AudioDebugStream.write(`\n=== P-12 session ${new Date().toISOString()} ===\n`);
    }
    // Limit IPC injection size and rate; more than two events per 200ms is unnecessary.
    const now=Date.now(); if(now-p12AudioDebugLast<100)return;
    p12AudioDebugLast=now;
    const type=String(packet&&packet.type||'event').slice(0,64);
    const details=JSON.stringify(packet&&packet.details||{}).slice(0,6000);
    p12AudioDebugStream.write(`[${new Date().toISOString()}] ${type} ${details}\n`);
  }catch(e){p12Log('AUDIO DEBUG ERROR',e);}
}
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
if (process.platform === 'win32') app.setAppUserModelId('com.p12.sampler');

let mainWindow = null;
let nativeHost = null;
let p12ShuttingDown = false;
let nativeHostPath = null;
let nativeHostFailed = null;
let nativeHostStdoutBuffer = '';
let nativeRequestSeq = 0;
const nativePending = new Map();


function defaultVstFolders() {
  const out = [];
  if (process.platform === 'win32') {
    out.push('C:\\Program Files\\Common Files\\VST3');
    out.push('C:\\Program Files\\VstPlugins');
    out.push('C:\\Program Files\\Steinberg\\VstPlugins');
    out.push('C:\\Program Files\\Common Files\\VST2');
  } else if (process.platform === 'darwin') {
    out.push('/Library/Audio/Plug-Ins/VST3');
    if (process.env.HOME) out.push(path.join(process.env.HOME, 'Library', 'Audio', 'Plug-Ins', 'VST3'));
    out.push('/Library/Audio/Plug-Ins/VST');
    if (process.env.HOME) out.push(path.join(process.env.HOME, 'Library', 'Audio', 'Plug-Ins', 'VST'));
  } else {
    out.push('/usr/lib/vst3', '/usr/local/lib/vst3');
    if (process.env.HOME) out.push(path.join(process.env.HOME, '.vst3'));
    if (process.env.HOME) out.push(path.join(process.env.HOME, '.vst'));
  }
  return [...new Set(out)].slice(0, 4);
}

function cleanScanRoot(value) {
  let v = String(value || '').trim();
  if (v.length >= 2 && ((v.startsWith('"') && v.endsWith('"')) || (v.startsWith("'") && v.endsWith("'")))) v = v.slice(1, -1).trim();
  return v;
}

function scanErr(stats, target, err) {
  stats.errorCount++;
  if (stats.errors.length < 24) stats.errors.push({ path: String(target || ''), code: err && err.code || '', message: err && err.message || String(err || 'Unknown scan error') });
}

async function canonicalPath(target) {
  try { return await fsp.realpath(target); } catch { return path.resolve(target); }
}

function pluginFormatForPath(full, isDirectory = false) {
  const lower = String(full || '').toLowerCase();
  if (lower.endsWith('.vst3')) return { format: 'VST3', legacyCandidate: false };
  if (lower.endsWith('.vst')) return { format: 'VST2', legacyCandidate: false };
  if (!isDirectory && process.platform === 'win32' && lower.endsWith('.dll')) return { format: 'VST2', legacyCandidate: true };
  if (!isDirectory && process.platform !== 'win32' && process.platform !== 'darwin' && lower.endsWith('.so')) return { format: 'VST2', legacyCandidate: true };
  return null;
}

function stripPluginExtension(name) {
  return String(name || '').replace(/\.(?:vst3|vst|dll|so)$/i, '');
}

async function addVstResult(full, entLike, formatInfo, results, seenPlugins, stats) {
  const canonical = await canonicalPath(full);
  const key = (process.platform === 'win32' ? canonical.toLowerCase() : canonical) + '|' + formatInfo.format;
  if (seenPlugins.has(key)) return false;
  seenPlugins.add(key);
  const base = path.basename(full);
  results.push({
    name: stripPluginExtension(base),
    path: full,
    vendor: '',
    category: formatInfo.legacyCandidate ? 'VST2 / Legacy candidate' : formatInfo.format,
    format: formatInfo.format,
    legacyCandidate: !!formatInfo.legacyCandidate
  });
  stats.plugins++;
  stats.formats[formatInfo.format] = (stats.formats[formatInfo.format] || 0) + 1;
  if (formatInfo.legacyCandidate) stats.legacyCandidates++;
  if (entLike && entLike.isDirectory && entLike.isDirectory()) stats.bundleDirs++;
  else if (entLike && entLike.isFile && entLike.isFile()) stats.pluginFiles++;
  else stats.otherBundles++;
  return true;
}

async function scanVstRoot(root, results, seenPlugins, seenDirs, stats, depth = 0) {
  if (!root || depth > 32 || stats.entries >= 250000) return;

  let lst;
  try { lst = await fsp.lstat(root); }
  catch (err) { scanErr(stats, root, err); if (depth === 0 && err && err.code === 'ENOENT') stats.missing = true; return; }

  let st = lst;
  if (lst.isSymbolicLink()) {
    stats.linksFollowed++;
    try { st = await fsp.stat(root); }
    catch (err) { scanErr(stats, root, err); return; }
  }

  // A folder picker can select a plug-in bundle directly. Treat .vst3 and
  // macOS .vst bundles as atomic plug-ins; never recurse through their internals.
  const directFormat = pluginFormatForPath(root, st.isDirectory());
  if (directFormat) {
    await addVstResult(root, st, directFormat, results, seenPlugins, stats);
    return;
  }

  if (!st.isDirectory()) return;

  const realDir = await canonicalPath(root);
  const dirKey = process.platform === 'win32' ? realDir.toLowerCase() : realDir;
  if (seenDirs.has(dirKey)) return;
  seenDirs.add(dirKey);
  stats.directories++;

  let entries;
  try { entries = await fsp.readdir(root, { withFileTypes: true }); }
  catch (err) { scanErr(stats, root, err); return; }

  for (const ent of entries) {
    if (stats.entries >= 250000) { stats.truncated = true; break; }
    stats.entries++;
    const full = path.join(root, ent.name);
    const fmt = pluginFormatForPath(full, ent.isDirectory());

    if (fmt) {
      await addVstResult(full, ent, fmt, results, seenPlugins, stats);
      // VST3/.vst bundles are directories containing implementation files;
      // counting them as one plug-in avoids false DLLs from bundle internals.
      if (ent.isDirectory()) continue;
    }

    if (ent.isDirectory()) {
      await scanVstRoot(full, results, seenPlugins, seenDirs, stats, depth + 1);
    } else if (ent.isSymbolicLink()) {
      try {
        const targetStat = await fsp.stat(full);
        if (targetStat.isDirectory()) await scanVstRoot(full, results, seenPlugins, seenDirs, stats, depth + 1);
        else {
          const linkedFmt = pluginFormatForPath(full, false);
          if (linkedFmt) await addVstResult(full, targetStat, linkedFmt, results, seenPlugins, stats);
        }
      } catch (err) { scanErr(stats, full, err); }
    }
  }
}

async function scanVstFolders(folders) {
  p12Diag('VST','scan-start',{folders:Array.isArray(folders)?folders:[]});
  const supplied = (Array.isArray(folders) ? folders : []).map(cleanScanRoot).filter(Boolean);
  const useRoots = supplied.length ? supplied : defaultVstFolders();
  const roots = [...new Set(useRoots.map(cleanScanRoot).filter(Boolean))];
  const results = [], seenPlugins = new Set(), seenDirs = new Set();
  const started = Date.now();
  const rootReports = [];
  let totalDirs = 0, totalEntries = 0, totalErrors = 0, totalLinks = 0, totalLegacy = 0;
  const totalFormats = { VST3: 0, VST2: 0 };

  for (const root of roots) {
    const stats = { root, plugins: 0, formats: { VST3: 0, VST2: 0 }, legacyCandidates: 0, directories: 0, entries: 0, errorCount: 0, errors: [], bundleDirs: 0, pluginFiles: 0, otherBundles: 0, linksFollowed: 0, missing: false, truncated: false };
    await scanVstRoot(root, results, seenPlugins, seenDirs, stats, 0);
    totalDirs += stats.directories;
    totalEntries += stats.entries;
    totalErrors += stats.errorCount;
    totalLinks += stats.linksFollowed;
    totalLegacy += stats.legacyCandidates;
    totalFormats.VST3 += stats.formats.VST3 || 0;
    totalFormats.VST2 += stats.formats.VST2 || 0;
    rootReports.push(stats);
  }

  results.sort((a, b) => a.format.localeCompare(b.format) || a.name.localeCompare(b.name));
  const report = {
    plugins: results,
    folders: roots,
    diagnostics: {
      liveDiskScan: true,
      durationMs: Date.now() - started,
      directoriesVisited: totalDirs,
      entriesInspected: totalEntries,
      errors: totalErrors,
      linksFollowed: totalLinks,
      legacyCandidates: totalLegacy,
      formatCounts: totalFormats,
      roots: rootReports
    }
  };
  p12Diag('VST','scan-complete',{durationMs:report.diagnostics.durationMs,plugins:results.length,formatCounts:report.diagnostics.formatCounts,errors:report.diagnostics.errors,roots});
  return report;
}


function settleNativeReply(message) {
  if (!message || !message.replyTo) return false;
  const pending = nativePending.get(String(message.replyTo));
  if (!pending) return false;
  nativePending.delete(String(message.replyTo));
  clearTimeout(pending.timer);
  const durationMs = pending.started ? Date.now()-pending.started : null;
  if (pending.command === 'nativeStatus') p12DiagNativeStatus(message);
  else if (pending.command !== 'processPCM' && (pending.command !== 'nativeMeters' || message.ok===false)) p12Diag('NATIVE_HOST','response',{command:pending.command,durationMs,ok:message&&message.ok!==false,message:message&&message.message||''},message&&message.ok===false?'ERROR':'INFO');
  if(message&&message.ok===false && ['loadVST3','loadVST2','setMixerInsertVST'].includes(pending.command))p12RecordPluginFailure(pending.payload,message.message||'host rejected plug-in');
  pending.resolve(message);
  return true;
}

function failAllNativePending(message) {
  for (const [id, pending] of nativePending) {
    clearTimeout(pending.timer);
    pending.resolve({ ok: false, message });
  }
  nativePending.clear();
}

function handleNativeStdout(chunk) {
  nativeHostStdoutBuffer += String(chunk || '');
  const lines = nativeHostStdoutBuffer.split(/\r?\n/);
  nativeHostStdoutBuffer = lines.pop() || '';
  for (const raw of lines) {
    const line = raw.trim();
    if (!line) continue;
    try {
      const message = JSON.parse(line);
      if (!settleNativeReply(message)) { p12Diag('NATIVE_HOST','unsolicited-json',message); console.log('[P12 VST HOST]', line.slice(0, 4000)); }
    } catch (_err) {
      p12Diag('NATIVE_HOST','stdout-text',{line:line.slice(0,4000)},'WARN');
      console.log('[P12 VST HOST]', line.slice(0, 4000));
    }
  }
}

function findNativeHost() {
  const candidates = [];
  if (process.env.P12_VST_HOST) candidates.push(process.env.P12_VST_HOST);
  if (process.platform === 'win32') {
    candidates.push(path.join(process.resourcesPath || __dirname, 'native', 'p12-vst-host.exe'));
    candidates.push(path.join(__dirname, 'native', 'p12-vst-host.exe'));
  } else {
    candidates.push(path.join(process.resourcesPath || __dirname, 'native', 'P-12 VST Host.app', 'Contents', 'MacOS', 'P-12 VST Host'));
    candidates.push(path.join(__dirname, 'native', 'P-12 VST Host.app', 'Contents', 'MacOS', 'P-12 VST Host'));
    candidates.push(path.join(process.resourcesPath || __dirname, 'native', 'p12-vst-host'));
    candidates.push(path.join(__dirname, 'native', 'p12-vst-host'));
  }
  return candidates.find(p => p && fs.existsSync(p)) || null;
}

function ensureNativeHost() {
  if(p12ShuttingDown)return {ok:false,message:'P-12 is shutting down.'};
  if (nativeHost && !nativeHost.killed) return { ok: true, path: nativeHostPath };
  nativeHostPath = findNativeHost();
  if (!nativeHostPath) {
    p12Diag('NATIVE_HOST','binary-missing',{resourcesPath:process.resourcesPath,appPath:__dirname},'ERROR');
    return { ok: false, message: 'Native host is missing. Run BUILD_MAC.command to compile and package the Apple Silicon VST3 host.', requiresNativeHost: true };
  }
  try {
    p12Transport.invalidate();
    // Writable private host runtime: plugin-owned ICU may be placed adjacent
    // to the executable without modifying the installed application/vendor.
    if(process.platform==='win32'){
    const runtimeDir=path.join(app.getPath('userData'),'native-runtime');
    fs.mkdirSync(runtimeDir,{recursive:true});
    const runtimeHost=path.join(runtimeDir,path.basename(nativeHostPath));
    fs.copyFileSync(nativeHostPath,runtimeHost);
    for(const ent of fs.readdirSync(path.dirname(nativeHostPath),{withFileTypes:true})){
      if(ent.isFile()&&/\.dll$/i.test(ent.name))fs.copyFileSync(path.join(path.dirname(nativeHostPath),ent.name),path.join(runtimeDir,ent.name));
    }
    nativeHostPath=runtimeHost;
    }
    // Launch the signed Mac app executable in place; never copy or mutate its bundle.
    nativeHost = spawn(nativeHostPath, ['--json-lines'], { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true, cwd: path.dirname(nativeHostPath), env: { ...process.env, P12_VST_HOST: '1' } });
    p12Log('VST HOST START', { path: nativeHostPath, cwd: path.dirname(nativeHostPath) });
    p12Diag('NATIVE_HOST','start',{path:nativeHostPath,cwd:path.dirname(nativeHostPath)});
    nativeHostFailed = null;
    // stdout is the JSON-lines reply channel. stderr is reserved for bounded logs.
    nativeHostStdoutBuffer = '';
    nativeHost.stdout.on('data', handleNativeStdout);
    nativeHost.stderr.on('data', chunk => {
      const line = String(chunk || '').trim();
      if (line) { p12Log('VST HOST STDERR', line.slice(0, 4000)); p12Diag('NATIVE_HOST','stderr',{line:line.slice(0,4000)},/fail|error|crash|timeout/i.test(line)?'ERROR':'INFO'); }
    });
    nativeHost.on('exit', (code, signal) => {
      nativeHost = null;
      nativeHostFailed = `Native VST host exited (${code ?? signal ?? 'unknown'}).`;
      p12Log('VST HOST EXIT', { code, signal, message: nativeHostFailed });
      p12Diag('NATIVE_HOST','exit',{code,signal,message:nativeHostFailed},'ERROR');
      failAllNativePending(nativeHostFailed);
    });
    nativeHost.on('error', err => {
      nativeHost = null;
      nativeHostFailed = err.message;
      p12Log('VST HOST ERROR', err);
      p12Diag('NATIVE_HOST','process-error',{message:err.message,stack:err.stack},'ERROR');
      failAllNativePending(`Native VST host error: ${err.message}`);
    });
    return { ok: true, path: nativeHostPath };
  } catch (err) {
    nativeHost = null; nativeHostFailed = err.message;
    p12Log('VST HOST START FAILURE',{path:nativeHostPath,message:err.message,code:err.code});
    p12Diag('NATIVE_HOST','start-failed',{path:nativeHostPath,message:err.message,code:err.code},'ERROR');
    return { ok: false, message: `Native VST host failed to start: ${err.message}`, requiresNativeHost: true };
  }
}

function sendNativeFast(command, payload) {
  const status = ensureNativeHost();
  if (!status.ok) return status;
  try {
    nativeHost.stdin.write(JSON.stringify({ command, payload }) + '\n');
    return { ok: true, queued: true, host: status.path };
  } catch (err) {
    return { ok: false, message: `Native VST host IPC failed: ${err.message}` };
  }
}

function requestNative(command, payload, timeoutMs = 45000) {
  const status = ensureNativeHost();
  if (!status.ok) return Promise.resolve(status);
  const id = `p12-${process.pid}-${Date.now()}-${++nativeRequestSeq}`;
  return new Promise(resolve => {
    // Per-buffer console/disk logging starves the renderer under VST load.
    if (command !== 'processPCM' && command !== 'nativeStatus' && command !== 'nativeMeters') {
      const summary={id,command,path:payload&&payload.path||'',target:payload&&payload.target||'',slot:payload&&payload.slot||'',kind:payload&&payload.kind,index:payload&&payload.index};
      p12Log('VST HOST REQUEST', summary); p12Diag('NATIVE_HOST','request',summary);
    }
    const timer = setTimeout(() => {
      nativePending.delete(id);
      if(command!=='processPCM'&&command!=='nativeStatus'&&command!=='nativeMeters'){p12WriteAudioDebug({type:'host-timeout',details:{command,timeoutMs}});p12Diag('NATIVE_HOST','timeout',{command,timeoutMs},'ERROR');if(['loadVST3','loadVST2','setMixerInsertVST'].includes(command))p12RecordPluginFailure(payload,'timeout');}
      resolve({ ok: false, message: `Native VST host timed out while handling ${command}.` });
    }, timeoutMs);
    nativePending.set(id, { resolve, timer, command, started:Date.now(), payload });
    try {
      nativeHost.stdin.write(JSON.stringify({ id, command, payload }) + '\n');
    } catch (err) {
      clearTimeout(timer);
      nativePending.delete(id);
      resolve({ ok: false, message: `Native VST host IPC failed: ${err.message}` });
    }
  });
}

function createWindow() {
  p12Log('Creating BrowserWindow', { electron: process.versions.electron, node: process.versions.node, cwd: process.cwd(), dirname: __dirname });
  p12Diag('APP','create-main-window',{electron:process.versions.electron,node:process.versions.node,platform:process.platform,arch:process.arch});
  mainWindow = new BrowserWindow({
    width: 1600,
    height: 980,
    minWidth: 1100,
    minHeight: 720,
    backgroundColor: '#111315',
    icon: path.join(__dirname, 'build', 'icon.png'),
    show: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
      backgroundThrottling: false
    }
  });
  mainWindow.removeMenu();
  mainWindow.webContents.on('did-fail-load', (_event, code, description, url) => { p12Log('DID-FAIL-LOAD', { code, description, url }); p12Diag('RENDERER','did-fail-load',{code,description,url},'ERROR'); });
  mainWindow.webContents.on('render-process-gone', (_event, details) => { p12Log('RENDER-PROCESS-GONE', details); p12Diag('RENDERER','process-gone',details,'ERROR'); });
  mainWindow.webContents.on('unresponsive', () => { p12Log('RENDERER UNRESPONSIVE'); p12Diag('RENDERER','unresponsive',{},'ERROR'); });
  mainWindow.webContents.on('console-message', (_event, level, message, line, sourceId) => { p12Log('RENDERER CONSOLE', { level, message, line, sourceId }); if(level>=2)p12Diag('RENDERER','console',{level,message,line,sourceId},level>=3?'ERROR':'WARN'); });
  const htmlPath = path.join(__dirname, 'P-12.html');
  p12Log('Loading HTML', htmlPath);
  mainWindow.loadFile(htmlPath).then(() => {p12Log('HTML loaded');p12Diag('APP','html-loaded',{path:htmlPath});}).catch(err => {p12Log('LOADFILE ERROR', err);p12Diag('APP','html-load-error',{message:err.message,stack:err.stack},'ERROR');});
  mainWindow.once('ready-to-show', () => {
    p12Log('ready-to-show'); p12Diag('APP','ready-to-show'); mainWindow.show();
    if(P12_SMOKE_TEST){
      mainWindow.webContents.executeJavaScript(`(async()=>{
        const api=window.electronAPI;
        if(!api || api.nativeHosting!==true || !document.getElementById('saveProjectBtn'))throw new Error('Mac preload or application UI unavailable');
        for(const method of ['openMixerWindow','openPlaylistWindow','openPianoWindow']){
          const result=await api[method]();if(!result.ok)throw new Error(method+' failed');
        }
        const host=await api.getVSTHostStatus();
        if(!host.ok || !host.vst3)throw new Error('Native VST3 host unavailable');
        return true;
      })()`).then(()=>{p12Log('SMOKE TEST PASS: Mac preload, main UI and three detached windows');app.quit();})
      .catch(err=>{p12Log('SMOKE TEST FAIL',err);app.exit(1);});
    }
  });
  mainWindow.on('closed', () => { p12Log('BrowserWindow closed'); p12Diag('APP','main-window-closed'); mixerWindow?.close();pianoWindow?.close();playlistWindow?.close(); mainWindow = null; });
}

// Detachable Piano Roll: the main renderer remains the sole native audio
// engine and project owner. This child receives snapshots, not samples/DAW state.
let mixerWindow=null;
ipcMain.handle('p12:mixer-open',async event=>{
  if(event.sender!==mainWindow?.webContents)return {ok:false};
  if(mixerWindow&&!mixerWindow.isDestroyed()){mixerWindow.show();mixerWindow.focus();return {ok:true};}
  mixerWindow=new BrowserWindow({title:'P-12 — Mixer 32',width:1700,height:980,minWidth:700,minHeight:450,resizable:true,backgroundColor:'#17191b',
    webPreferences:{preload:path.join(__dirname,'preload.js'),contextIsolation:true,nodeIntegration:false,sandbox:false,backgroundThrottling:false}});
  mixerWindow.removeMenu();mixerWindow.webContents.setWindowOpenHandler(()=>({action:'deny'}));
  mixerWindow.on('closed',()=>{mixerWindow=null;mainWindow?.webContents.send('p12:mixer-action',{type:'closed'});});
  try{await mixerWindow.loadFile(path.join(__dirname,'mixer-detached.html'));return {ok:true};}catch(err){return {ok:false,message:err.message};}
});
ipcMain.handle('p12:mixer-close',event=>{if(event.sender===mainWindow?.webContents)mixerWindow?.close();return {ok:true};});
ipcMain.on('p12:mixer-meters',(event,packet)=>{if(event.sender===mainWindow?.webContents)mixerWindow?.webContents.send('p12:mixer-meters',packet);});
ipcMain.on('p12:mixer-state',(event,state)=>{if(event.sender===mainWindow?.webContents)mixerWindow?.webContents.send('p12:mixer-state',state);});
ipcMain.on('p12:mixer-action',(event,action)=>{if(event.sender===mixerWindow?.webContents){mainWindow?.webContents.send('p12:mixer-action',action);if(action?.focusMain){mainWindow?.show();mainWindow?.focus();}}});
let playlistWindow=null;
ipcMain.handle('p12:playlist-open',async event=>{
  if(event.sender!==mainWindow?.webContents)return {ok:false};
  if(playlistWindow&&!playlistWindow.isDestroyed()){playlistWindow.show();playlistWindow.focus();return {ok:true};}
  playlistWindow=new BrowserWindow({title:'P-12 — Playlist / Arrangement',width:1180,height:650,minWidth:620,minHeight:360,resizable:true,backgroundColor:'#17191b',
    webPreferences:{preload:path.join(__dirname,'preload.js'),contextIsolation:true,nodeIntegration:false,sandbox:false}});
  playlistWindow.removeMenu();playlistWindow.webContents.setWindowOpenHandler(()=>({action:'deny'}));
  playlistWindow.on('closed',()=>{playlistWindow=null;mainWindow?.webContents.send('p12:playlist-action',{type:'closed'});});
  try{await playlistWindow.loadFile(path.join(__dirname,'playlist-detached.html'));return {ok:true};}catch(err){playlistWindow?.close();return {ok:false,message:err.message};}
});
ipcMain.on('p12:playlist-state',(event,state)=>{if(event.sender===mainWindow?.webContents)playlistWindow?.webContents.send('p12:playlist-state',state);});
ipcMain.on('p12:playlist-action',(event,action)=>{if(event.sender===playlistWindow?.webContents&&action&&typeof action==='object')mainWindow?.webContents.send('p12:playlist-action',action);});
let pianoWindow=null;
ipcMain.handle('p12:piano-open',async()=>{
  if(!mainWindow||mainWindow.isDestroyed())return {ok:false,message:'Main application unavailable'};
  if(pianoWindow&&!pianoWindow.isDestroyed()) {pianoWindow.show();pianoWindow.focus();return {ok:true};}
  pianoWindow=new BrowserWindow({width:1180,height:790,minWidth:700,minHeight:410,
    title:'P-12 — VST Piano Roll',backgroundColor:'#101720',
    webPreferences:{preload:path.join(__dirname,'preload.js'),contextIsolation:true,
      nodeIntegration:false,sandbox:false,backgroundThrottling:false}});
  pianoWindow.removeMenu();
  pianoWindow.on('closed',()=>{
    pianoWindow=null;
    if(mainWindow&&!mainWindow.isDestroyed())mainWindow.webContents.send('p12:piano-action',{type:'close'});
  });
  await pianoWindow.loadFile(path.join(__dirname,'piano-detached.html'));
  return {ok:true};
});
// Detached Piano Roll editing may generate a large number of pointer events.
// Summarize edits off the audio thread; the main log stores no MIDI payloads.
const p12PianoActionCounts=new Map();
let p12PianoActionFlushTimer=null;
function p12FlushPianoActions(){
  p12PianoActionFlushTimer=null;
  if(!p12PianoActionCounts.size)return;
  const counts=Object.fromEntries(p12PianoActionCounts);
  p12PianoActionCounts.clear();
  p12Diag('PIANO','detached-edit-summary',{counts});
}
ipcMain.on('p12:piano-action',(event,action)=>{
  if(!pianoWindow||event.sender!==pianoWindow.webContents)return;
  if(!action||typeof action!=='object')return;
  const kind=String(action.type||'').slice(0,48);
  if(['play','stop','rec','recPlay','editGrid','tc'].includes(kind))
    p12Diag('PIANO','detached-control',{type:kind,grid:kind==='editGrid'?String(action.editGrid||''):undefined});
  else if(!['state','audition','auditionOff','close'].includes(kind)){
    p12PianoActionCounts.set(kind,1+(p12PianoActionCounts.get(kind)||0));
    if(!p12PianoActionFlushTimer)p12PianoActionFlushTimer=setTimeout(p12FlushPianoActions,3000);
  }
  if(mainWindow&&!mainWindow.isDestroyed())mainWindow.webContents.send('p12:piano-action',action);
});
ipcMain.on('p12:piano-state',(event,snapshot)=>{
  if(!mainWindow||event.sender!==mainWindow.webContents)return;
  if(pianoWindow&&!pianoWindow.isDestroyed())pianoWindow.webContents.send('p12:piano-state',snapshot);
});
ipcMain.handle('p12:pick-vst-folder', async () => {
  p12Diag('VST','folder-picker-open');
  const result = await dialog.showOpenDialog(mainWindow, { title: 'Choose VST2 / VST3 plug-in folder', properties: ['openDirectory'] });
  const chosen=result.canceled?null:result.filePaths[0];p12Diag('VST','folder-picker-result',{canceled:result.canceled,path:chosen||''});return chosen;
});
ipcMain.handle('p12:default-vst-folders', async () => defaultVstFolders());
ipcMain.handle('p12:scan-vst-folders', async (_event, payload) => scanVstFolders(payload && payload.folders));
ipcMain.handle('p12:vst-host-status', async () => {
  const p = findNativeHost();
  if (!p) return { ok: false, requiresNativeHost: true, message: nativeHostFailed || 'Native VST2/VST3 DSP host binary not installed.' };
  const result = await requestNative('ping', {});
  return { ...result, nativeHostPath: p };
});
ipcMain.handle('p12:vst-load', async (_event, route) => {
  const q=p12PluginKey(route);if(q&&p12PluginQuarantine.has(q))return {ok:false,quarantined:true,message:'Plug-in quarantined for this P-12 session after repeated host failures.'};
  const format = String(route && route.format || '').toUpperCase() || (String(route && route.path || '').toLowerCase().endsWith('.vst3') ? 'VST3' : 'VST2');
  const payload = { ...(route || {}), format };
  return requestNative(format === 'VST3' ? 'loadVST3' : 'loadVST2', payload, 180000);
});
ipcMain.handle('p12:vst-unload', async (_event, route) => requestNative('unloadVST', route || {}));
ipcMain.handle('p12:vst-insert', async (_event, assignment) => {const q=p12PluginKey(assignment);if(q&&p12PluginQuarantine.has(q))return {ok:false,quarantined:true,message:'Plug-in quarantined for this P-12 session after repeated host failures.'};return requestNative('setMixerInsertVST', assignment, 180000);});
ipcMain.handle('p12:vst-quarantine-status',async()=>({ok:true,paths:[...p12PluginQuarantine]}));
ipcMain.handle('p12:vst-quarantine-clear',async()=>{p12PluginFailures.clear();p12PluginQuarantine.clear();p12Diag('VST','quarantine-cleared');return {ok:true};});
// Real PCM exchange, not MIDI-only GUI hosting. Renderer batches 2048 frames.
ipcMain.handle('p12:vst-pcm', async (_event, packet) => requestNative('processPCM', packet || {}, 12000));
// Native engine phase 1: one-time sample staging and non-blocking event queue.
// PCM NEVER travels over Electron IPC during playback in this mode.
ipcMain.handle('p12:native-engine', async (_event, packet) => {
  if(_event.sender!==mainWindow?.webContents && !(packet?.command==='nativeStatus'&&_event.sender===mixerWindow?.webContents))return {ok:false,message:'The main project owns native transport'};
  if (!packet || !['nativeMode','nativeSampleLoad','nativeStop','nativeStatus','nativeMeters','nativeTransport','nativeSequence','nativeSampleClear','allNotesOff','nativeBufferSize','nativeBuiltinSync','nativePadControlsSync','renderVSTToWav','inspectInstrument'].includes(packet.command))
    return {ok:false,message:'Invalid native audio request'};
  if(packet.command==='nativeTransport')return p12Transport.request(packet.payload||{});
  if(packet.command==='nativeMode')p12Transport.invalidate();
  return requestNative(packet.command, packet.payload || {}, packet.command==='renderVSTToWav'?120000:(packet.command==='nativeSampleLoad'?60000:12000));
});
ipcMain.on('p12:audio-debug', (_event, packet) => p12WriteAudioDebug(packet));
ipcMain.handle('p12:audio-debug-path', async () => ({path:p12AudioDebugPath()}));
ipcMain.on('p12:diagnostic', (_event, packet) => { if(packet&&typeof packet==='object')p12Diag(packet.category||'RENDERER',packet.event||'event',packet.details||{},packet.level||'INFO'); });
ipcMain.handle('p12:diagnostic-path', async () => ({path:p12DiagnosticPath(),audioDebugPath:p12AudioDebugPath(),electronLogPath}));
ipcMain.on('p12:native-trigger', (_event, packet) => { sendNativeFast('nativeTrigger', packet || {}); });
ipcMain.on('p12:native-stop', (_event, packet) => { sendNativeFast('nativeStop', packet || {}); });

const p12EditorRequests=new Map();
ipcMain.handle('p12:vst-editor', async (_event, assignment) => {
  const key=JSON.stringify([assignment?.target,assignment?.id,assignment?.slot,assignment?.path,!!assignment?.generic]);
  if(p12EditorRequests.has(key))return p12EditorRequests.get(key);
  const pending=requestNative('openEditor',assignment,120000);
  p12EditorRequests.set(key,pending);
  try{return await pending;}finally{if(p12EditorRequests.get(key)===pending)p12EditorRequests.delete(key);}
});
ipcMain.handle('p12:vst-editor-close', async (_event, assignment) => requestNative('closeEditor', assignment));
ipcMain.handle('p12:vst-state-get', async (_event, assignment) => requestNative('getPluginState', assignment));
ipcMain.handle('p12:vst-state-set', async (_event, assignment) => requestNative('setPluginState', assignment));
ipcMain.handle('p12:vst-audio-list', async () => requestNative('listAudioOutputs', {}));
ipcMain.handle('p12:vst-audio-set', async (_event, payload) => requestNative('setAudioOutput', payload || {}));
ipcMain.handle('p12:vst-audio-test', async () => requestNative('testTone', {}));
ipcMain.on('p12:vst-midi', (_event, message) => { sendNativeFast('midi', message); }); // MIDI events are deliberately not logged per-note (realtime-safe).
ipcMain.on('p12:vst-all-notes-off', () => { p12Diag('MIDI','all-notes-off'); sendNativeFast('allNotesOff', {}); });

app.on('child-process-gone', (_event, details) => {p12Log('CHILD-PROCESS-GONE', details);p12Diag('APP','child-process-gone',details,'ERROR');});
app.whenReady().then(() => {
  if(process.platform==='darwin')Menu.setApplicationMenu(Menu.buildFromTemplate([{role:'appMenu'},{role:'editMenu'},{role:'viewMenu'},{role:'windowMenu'}]));
 p12Log('Electron app ready');p12Diag('APP','ready',{appVersion:app.getVersion(),electron:process.versions.electron,node:process.versions.node}); createWindow(); }).catch(err => {p12Log('APP READY ERROR', err);p12Diag('APP','ready-error',{message:err.message,stack:err.stack},'ERROR');});
app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
app.on('window-all-closed', () => { p12Log('window-all-closed');p12Diag('APP','window-all-closed'); if (process.platform !== 'darwin') app.quit(); });
app.on('before-quit', () => {
  p12ShuttingDown = true;
  p12Diag('APP','before-quit');
  if (p12AudioDebugStream) {p12AudioDebugStream.end();p12AudioDebugStream=null;}
  failAllNativePending('P-12 is shutting down.');
  try { if (nativeHost && !nativeHost.killed) nativeHost.kill(); } catch {}
  if (p12DiagnosticStream) {p12DiagnosticStream.end();p12DiagnosticStream=null;}
});
