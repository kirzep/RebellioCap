// Actual combined recording soak; caller supplies a previously validated host config.
import {spawn, execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {appendFileSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, statSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {createInterface} from 'node:readline';
import {setTimeout as sleep} from 'node:timers/promises';

const engine = resolve(process.env.REBELLIOCAP_SOAK_ENGINE ?? 'build/windows-hardware-release/RebellioCap.Engine.exe');
const sourceConfig = process.env.REBELLIOCAP_SOAK_CONFIG;
if (!sourceConfig) throw Error('REBELLIOCAP_SOAK_CONFIG must identify a validated native host configuration');
const seconds = Number(process.env.REBELLIOCAP_SOAK_SECONDS ?? 1860);
if (!Number.isInteger(seconds) || seconds < 12 || seconds > 28800) throw Error('Soak duration must be 12..28800 seconds');
const evidence = mkdtempSync(join(tmpdir(), 'rebcap-engine-soak-'));
const output = join(evidence, 'clips'); mkdirSync(output);
const config = {...JSON.parse(readFileSync(sourceConfig, 'utf8')), outputDirectory:output, continuousRecordingEnabled:false,
  saveReplayHotkey:'Ctrl+Alt+F11',toggleRecordingHotkey:'Ctrl+Alt+F12'};
const configPath = join(evidence, 'engine.json'); writeFileSync(configPath, JSON.stringify(config));
const result = {passed:false,evidence,engine,engineSha256:createHash('sha256').update(readFileSync(engine)).digest('hex'),seconds,config,samples:[],clips:[],errors:[]};
writeFileSync(join(evidence, 'running.json'),JSON.stringify(result,null,2));
console.log(JSON.stringify({evidence,seconds,engineSha256:result.engineSha256}));
const child = spawn(engine,['session-json','--config',configPath],{windowsHide:true,stdio:['pipe','pipe','pipe']});
let latest, fatal, sequence=0, readyResolve;
const pending = new Map();
async function deadline(promise, milliseconds, label) {
  let timer;
  try { return await Promise.race([promise,new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error(label)),milliseconds);})]); }
  finally { clearTimeout(timer); }
}
const ready = new Promise(resolve => {readyResolve=resolve;});
const exited = new Promise(resolve => child.once('exit', (code,signal) => resolve({code,signal})));
child.stderr.on('data',data => appendFileSync(join(evidence,'stderr.log'),data));
child.on('error',error => {fatal=error;readyResolve();});
createInterface({input:child.stdout}).on('line',line => {
  appendFileSync(join(evidence,'events.jsonl'),line+'\n');
  try {
    const event=JSON.parse(line); if(event.snapshot)latest=event.snapshot;
    if(event.type==='ready')readyResolve();
    if(event.type==='fatal_error'){fatal=Error(JSON.stringify(event.error));result.errors.push(event);readyResolve();}
    const wait=pending.get(event.requestId);
    if(wait && (event.error || event.type===wait.type)){pending.delete(event.requestId);event.error?wait.reject(Error(JSON.stringify(event.error))):wait.resolve(event);}
  } catch(error){fatal=error;result.errors.push(String(error));}
});
async function command(command,type='command_result') {
  if(fatal)throw fatal;
  const requestId='soak-'+(++sequence);
  const response=new Promise((resolve,reject)=>pending.set(requestId,{resolve,reject,type}));
  child.stdin.write(JSON.stringify({protocolVersion:1,requestId,type:'command',command})+'\n');
  try{return await deadline(response,30000,'Command timeout: '+command);}
  finally{pending.delete(requestId);}
}
function sample(phase, elapsedSeconds) {
  const processData=JSON.parse(execFileSync('powershell.exe',['-NoProfile','-NonInteractive','-Command',`$p=Get-Process -Id ${child.pid};[ordered]@{privateBytes=$p.PrivateMemorySize64;workingSetBytes=$p.WorkingSet64;cpuMs=$p.TotalProcessorTime.TotalMilliseconds}|ConvertTo-Json -Compress`],{windowsHide:true,encoding:'utf8'}));
  result.samples.push({utc:new Date().toISOString(),phase,elapsedSeconds,...processData,snapshot:latest});
  writeFileSync(join(evidence,'samples.json'),JSON.stringify(result.samples,null,2));
  if(fatal)throw fatal;
  const metrics=latest?.metrics;
  if(metrics && (metrics.pipelineErrors || metrics.continuousFailures || metrics.failedSaves))throw Error('Engine reported failed recording/save');
}
try {
  await deadline(ready,30000,'Ready timeout');if(fatal)throw fatal;
  await command('stop_replay');sample('idle-before',0);await sleep(seconds<60?1000:30000);sample('idle-before',seconds<60?1:30);
  await command('start_replay');await command('toggle_recording');
  if(!latest?.continuousRecordingActive)throw Error('Continuous recording did not start');
  const started=Date.now();let saved=false;
  while((Date.now()-started)/1000<seconds){
    const elapsed=(Date.now()-started)/1000;
    sample('recording',elapsed);
    if(!saved && elapsed>=Math.min(30,seconds/3)){await command('save_replay','clip_saved');saved=true;}
    await sleep(Math.min(seconds<60?2000:30000,Math.max(1,seconds*1000-(Date.now()-started))));
  }
  await command('save_replay','clip_saved');
  await command('toggle_recording');await command('stop_replay');sample('idle-after',0);await sleep(seconds<60?1000:30000);sample('idle-after',seconds<60?1:30);
  await command('stop');const exit=await deadline(exited,10000,'Stop exit timeout');
  if(exit.code!==0)throw Error('Engine exit '+JSON.stringify(exit));
  result.finalSnapshot=latest;
  const directories=[output,...readdirSync(output,{withFileTypes:true}).filter(entry=>entry.isDirectory()).map(entry=>join(output,entry.name))];
  result.clips=directories.flatMap(directory=>readdirSync(directory).filter(name=>/\.(mp4|mkv)$/i.test(name)).map(name=>({path:join(directory,name),bytes:statSync(join(directory,name)).size})));
  if(result.clips.length<3 || !latest?.metrics.videoPackets || !latest?.metrics.audioPackets)throw Error('Missing combined media/save evidence');
  result.passed=true;
} catch(error){result.failure=String(error);process.exitCode=1;}
finally {
  if(child.exitCode===null){try{await command('stop');}catch{}try{await deadline(exited,10000,'Cleanup timeout');}catch{}if(child.exitCode===null)child.kill();}
  result.finishedUtc=new Date().toISOString();writeFileSync(join(evidence,'result.json'),JSON.stringify(result,null,2));
  console.log(JSON.stringify({passed:result.passed,evidence,failure:result.failure}));
}
