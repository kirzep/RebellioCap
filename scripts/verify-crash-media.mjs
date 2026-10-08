import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createReadStream, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createInterface } from 'node:readline';
import { DecodedCoverage } from './crash-media-coverage.mjs';

async function hash(path) {
  const digest = createHash('sha256');
  for await (const chunk of createReadStream(path)) digest.update(chunk);
  return digest.digest('hex');
}
function execute(executable, args, timeout, directory, name) {
  const child = spawnSync(executable, args, { windowsHide: true, encoding: 'utf8', timeout, maxBuffer: 4 * 1024 * 1024 });
  writeFileSync(join(directory, name + '.stdout.txt'), child.stdout ?? '');
  writeFileSync(join(directory, name + '.stderr.txt'), child.stderr ?? '');
  if (child.error || child.status !== 0 || child.stderr?.trim()) {
    throw Error(`${name} failed: ${child.error ?? child.stderr ?? child.status}`);
  }
  return child.stdout;
}

/** Decode the retained bytes without rounding timestamps to a nominal CFR.
 * Null-output default encoder time bases otherwise create false duplicate-DTS
 * errors from valid fine-grained input timestamps. Error stderr is mandatory.
 */
async function measureCoverage(engine, run, stream, directory) {
  const coverage = new DecodedCoverage(stream, run.config);
  const child = spawn(join(dirname(engine), 'ffprobe.exe'), ['-v', 'error', '-select_streams', String(stream.index),
    '-show_frames', '-show_entries', 'frame=best_effort_timestamp_time,duration_time,nb_samples', '-of', 'compact=p=0:nk=0', run.partial.path],
    {windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']});
  let stderr = '', failure;
  const timer = setTimeout(() => { failure = Error('Frame coverage probe timed out'); child.kill(); }, Math.max(60000, Math.ceil(run.recordingSeconds * 1000)));
  child.stderr.on('data', data => {
    stderr += data;
    if (stderr.length > 65536) { stderr = stderr.slice(0, 65536); failure = Error('Frame probe stderr limit exceeded'); child.kill(); }
  });
  const lines = createInterface({input: child.stdout});
  lines.on('line', line => {
    if (failure || !line.trim() || line.startsWith('side_data_type=')) return;
    try {
      const fields = Object.fromEntries(line.split('|').map(field => { const at = field.indexOf('='); return [field.slice(0, at), field.slice(at + 1)]; }));
      coverage.push({time: Number(fields.best_effort_timestamp_time),
        duration: fields.duration_time === undefined ? undefined : Number(fields.duration_time),
        samples: fields.nb_samples === undefined ? undefined : Number(fields.nb_samples)});
    } catch (error) { failure = error; child.kill(); }
  });
  try {
    const status = await new Promise((resolve, reject) => { child.once('error', reject); child.once('close', resolve); });
    writeFileSync(join(directory, 'coverage-stream-' + stream.index + '.stderr.txt'), stderr);
    if (failure || status !== 0 || stderr.trim()) throw failure ?? Error('Frame probe failed: ' + stderr);
    writeFileSync(join(directory, 'coverage-stream-' + stream.index + '.json'), JSON.stringify(coverage.stats(), null, 2));
    const stats = coverage.verify(run.recordingSeconds);
    return stats;
  } finally { clearTimeout(timer); lines.close(); }
}

export async function verifyCrashMedia(engine, run, directory) {
  mkdirSync(directory, { recursive: true });
  const partial = run.partial.path;
  if (await hash(partial) !== run.partial.sha256) throw Error('Crash partial changed before verification');
  const probe = execute(join(dirname(engine), 'ffprobe.exe'), ['-v', 'error', '-show_streams', '-show_format', '-of', 'json', partial],
    30000, directory, 'probe');
  run.probe = JSON.parse(probe);
  const config = run.config;
  const expectedAudio = Number(config.systemAudioEnabled) + Number(config.microphoneEnabled) + Number(config.systemAudioEnabled && config.microphoneEnabled);
  if (run.probe.streams.length !== 1 + expectedAudio || run.probe.streams.filter(stream => stream.codec_name === 'h264').length !== 1 ||
      run.probe.streams.filter(stream => stream.codec_name === 'aac').length !== expectedAudio) throw Error('Unexpected stream layout');
  if (!Number.isFinite(run.recordingSeconds) || run.recordingSeconds < 12) throw Error('Missing wall-time recording evidence');
  run.decodedStreams = [];
  for (const stream of run.probe.streams) {
    const progress = execute(join(dirname(engine), 'ffmpeg.exe'), ['-v', 'error', '-xerror', '-i', partial,
      '-map', '0:' + stream.index, '-fps_mode:v', 'passthrough', '-enc_time_base:v', 'demux', '-enc_time_base:a', 'demux',
      '-progress', 'pipe:1', '-nostats', '-f', 'null', '-'],
      Math.max(60000, Math.ceil(run.recordingSeconds * 1000)), directory, 'decode-stream-' + stream.index);
    const decodedSeconds = Math.max(0, ...[...progress.matchAll(/^out_time_us=(\d+)$/gm)].map(match => Number(match[1]) / 1000000));
    if (!progress.includes('progress=end') || decodedSeconds < run.recordingSeconds - 5) throw Error('Incomplete decoded stream ' + stream.index);
    const coverage = await measureCoverage(engine, run, stream, directory);
    run.decodedStreams.push({ index: stream.index, codec: stream.codec_name, decodedSeconds, coverage });
  }
  run.decodedSeconds = Math.min(...run.decodedStreams.map(stream => stream.decodedSeconds));
  if (await hash(partial) !== run.partial.sha256) throw Error('Verification changed the source partial');
}

// Recheck existing recording evidence in a fresh directory; never overwrite
// the original run or silently record a replacement after a failed probe.
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const path = process.env.REBELLIOCAP_CRASH_VERIFY_RESULT;
  if (!path) throw Error('REBELLIOCAP_CRASH_VERIFY_RESULT must identify an existing crash result.json');
  const previous = JSON.parse(readFileSync(path, 'utf8'));
  const evidence = mkdtempSync(join(tmpdir(), 'rebcap-crash-verification-'));
  const result = { ...previous, passed: false, verificationEvidence: evidence, originalEvidence: resolve(path) };
  delete result.failure;
  try {
    if (!previous.runs.length || await hash(previous.engine) !== previous.engineSha256) throw Error('Missing recording runs or changed engine');
    const expectedContainers = previous.containers ?? ['mp4', 'mkv'];
    if (previous.runs.length !== expectedContainers.length || expectedContainers.some(container => previous.runs.filter(run => run.container === container).length !== 1)) {
      throw Error('Recording evidence is missing a planned container');
    }
    for (const run of result.runs) await verifyCrashMedia(previous.engine, run, join(evidence, run.container));
    if (await hash(previous.engine) !== previous.engineSha256) throw Error('Engine changed during verification');
    result.passed = true;
  } catch (error) { result.failure = String(error); process.exitCode = 1; }
  writeFileSync(join(evidence, 'result.json'), JSON.stringify(result, null, 2));
  console.log(JSON.stringify({ passed: result.passed, evidence, runs: result.runs.map(run => ({container: run.container, decodedStreams: run.decodedStreams})), failure: result.failure }));
}
