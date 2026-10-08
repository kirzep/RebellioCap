// Wall-time continuous recording acceptance. Only the child launched here is
// killed; outputs and evidence live in a fresh temporary directory.
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { appendFileSync, createReadStream, mkdirSync, mkdtempSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { setTimeout as sleep } from 'node:timers/promises';
import { verifyCrashMedia } from './verify-crash-media.mjs';

const engine = resolve(process.env.REBELLIOCAP_CRASH_ENGINE ?? 'build/windows-hardware-release/RebellioCap.Engine.exe');
const sourceConfig = process.env.REBELLIOCAP_CRASH_CONFIG;
if (!sourceConfig) throw Error('REBELLIOCAP_CRASH_CONFIG must identify a validated engine configuration');
const seconds = Number(process.env.REBELLIOCAP_CRASH_SECONDS ?? 180);
if (!Number.isInteger(seconds) || seconds < 12 || seconds > 28800) throw Error('Crash duration must be 12..28800 seconds');
const containers = (process.env.REBELLIOCAP_CRASH_CONTAINERS ?? 'mp4,mkv').split(',');
if (!containers.length || new Set(containers).size !== containers.length || containers.some(value => !['mp4', 'mkv'].includes(value))) throw Error('Containers must be unique mp4 and/or mkv');
async function hash(path) {
  const digest = createHash('sha256');
  for await (const chunk of createReadStream(path)) digest.update(chunk);
  return digest.digest('hex');
}
const evidence = mkdtempSync(join(tmpdir(), 'rebcap-continuous-crash-'));
const result = { passed: false, evidence, engine, engineSha256: await hash(engine), seconds, containers, runs: [], limitations: [
  'Records current desktop and enabled audio endpoints; content is uncontrolled.',
  'Decode and timestamps do not prove physical lip-sync, VLC playback or disk-full recovery.',
] };
console.log(JSON.stringify({ evidence, seconds, containers, engineSha256: result.engineSha256 }));

async function deadline(promise, milliseconds, label) {
  let timer;
  try { return await Promise.race([promise, new Promise((_, reject) => { timer = setTimeout(() => reject(Error(label)), milliseconds); })]); }
  finally { clearTimeout(timer); }
}

function files(directory) {
  return readdirSync(directory, { withFileTypes: true }).flatMap(entry => {
    const path = join(directory, entry.name);
    return entry.isDirectory() ? files(path) : [path];
  });
}

async function record(container) {
  const directory = join(evidence, container);
  const output = join(directory, 'clips');
  mkdirSync(output, { recursive: true });
  const config = { ...JSON.parse(readFileSync(sourceConfig, 'utf8')), container, outputDirectory: output,
    continuousRecordingEnabled: false, saveReplayHotkey: 'Ctrl+Alt+F11', toggleRecordingHotkey: 'Ctrl+Alt+F12' };
  const configPath = join(directory, 'engine.json');
  writeFileSync(configPath, JSON.stringify(config, null, 2));
  const run = { container, config, samples: [] };
  result.runs.push(run);
  const child = spawn(engine, ['session-json', '--config', configPath], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
  let latest, fatal, sequence = 0, readyResolve;
  const pending = new Map();
  const ready = new Promise(resolve => { readyResolve = resolve; });
  const exited = new Promise(resolve => child.once('exit', (code, signal) => resolve({ code, signal })));
  child.on('error', error => { fatal = error; readyResolve(); });
  child.stderr.on('data', data => appendFileSync(join(directory, 'stderr.log'), data));
  createInterface({ input: child.stdout }).on('line', line => {
    appendFileSync(join(directory, 'events.jsonl'), line + '\n');
    try {
      const event = JSON.parse(line);
      if (event.snapshot) latest = event.snapshot;
      if (event.type === 'ready') readyResolve();
      if (event.type === 'fatal_error') { fatal = Error(JSON.stringify(event.error)); readyResolve(); }
      const wait = pending.get(event.requestId);
      if (wait && (event.error || event.type === 'command_result')) {
        pending.delete(event.requestId);
        event.error ? wait.reject(Error(JSON.stringify(event.error))) : wait.resolve(event);
      }
    } catch (error) { fatal = error; }
  });
  async function command(command) {
    if (fatal) throw fatal;
    const requestId = 'crash-' + (++sequence);
    const response = new Promise((resolve, reject) => pending.set(requestId, { resolve, reject }));
    child.stdin.write(JSON.stringify({ protocolVersion: 1, requestId, type: 'command', command }) + '\n');
    try { return await deadline(response, 30000, 'Command timeout: ' + command); }
    finally { pending.delete(requestId); }
  }
  try {
    await deadline(ready, 30000, 'Ready timeout');
    if (fatal) throw fatal;
    await command('stop_replay');
    await command('toggle_recording');
    if (!latest?.continuousRecordingActive) throw Error('Continuous recording did not start');
    const started = performance.now();
    while ((performance.now() - started) / 1000 < seconds) {
      await sleep(Math.min(10000, Math.max(1, seconds * 1000 - (performance.now() - started))));
      await command('get_snapshot');
      if (!latest?.continuousRecordingActive || latest.metrics.pipelineErrors || latest.metrics.continuousFailures) {
        throw Error('Recording failed before intentional termination');
      }
      run.samples.push({ elapsedSeconds: (performance.now() - started) / 1000, snapshot: latest });
      writeFileSync(join(directory, 'samples.json'), JSON.stringify(run.samples, null, 2));
    }
    run.recordingSeconds = (performance.now() - started) / 1000;
    run.finalSnapshot = latest;
    if (!child.kill()) throw Error('Could not terminate owned engine process');
    run.exit = await deadline(exited, 10000, 'Termination timeout');
    const partials = files(output).filter(path => path.endsWith('.' + container + '.partial'));
    if (partials.length !== 1 || statSync(partials[0]).size === 0) throw Error('Expected one nonempty crash partial');
    const partial = partials[0];
    run.partial = { path: partial, bytes: statSync(partial).size, sha256: await hash(partial) };
    await verifyCrashMedia(engine, run, directory);
    console.log(JSON.stringify({ container, recordingSeconds: run.recordingSeconds, decodedSeconds: run.decodedSeconds, bytes: run.partial.bytes }));
  } finally {
    if (child.exitCode === null && child.signalCode === null) { child.kill(); await deadline(exited, 10000, 'Cleanup timeout'); }
  }
}

try {
  for (const container of containers) await record(container);
  if (await hash(engine) !== result.engineSha256) throw Error('Engine changed during acceptance');
  result.passed = true;
} catch (error) { result.failure = String(error); process.exitCode = 1; }
finally {
  result.finishedUtc = new Date().toISOString();
  writeFileSync(join(evidence, 'result.json'), JSON.stringify(result, null, 2));
  console.log(JSON.stringify({ passed: result.passed, evidence, failure: result.failure }));
}
