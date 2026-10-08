import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DecodedCoverage } from '../../scripts/crash-media-coverage.mjs';

test('rejects two frames with a long gap despite a matching final timestamp', () => {
  const coverage = new DecodedCoverage({codec_type: 'video'}, {fps: 60});
  coverage.push({time: 0, duration: 1 / 60});
  coverage.push({time: 178, duration: 1 / 60});
  assert.throws(() => coverage.verify(180), /coverage|gap/);
});

test('does not substitute a long packet duration for missing video frames', () => {
  const coverage = new DecodedCoverage({codec_type: 'video'}, {fps: 60});
  coverage.push({time: 0, duration: 178});
  coverage.push({time: 178, duration: 1 / 60});
  assert.throws(() => coverage.verify(180), /coverage|gap/);
});

test('rejects repeated frames whose durations would double-count coverage', () => {
  const coverage = new DecodedCoverage({codec_type: 'video'}, {fps: 60});
  for (let n = 0; n < 1200; n++) coverage.push({time: 0, duration: 1 / 60});
  assert.throws(() => coverage.verify(12), /coverage/);
  assert.ok(coverage.coveredSeconds < .02);
});

test('accepts contiguous frame coverage and reports counts and gaps', () => {
  const coverage = new DecodedCoverage({codec_type: 'video'}, {fps: 60});
  for (let n = 0; n < 600; n++) coverage.push({time: n / 60, duration: 1 / 60});
  const stats = coverage.verify(12);
  assert.equal(stats.frameCount, 600);
  assert.ok(Math.abs(stats.coveredSeconds - 10) < 1e-8);
  assert.ok(stats.maxGapSeconds < 1e-8);
});

test('counts actual AAC samples and rejects timestamps without samples', () => {
  const coverage = new DecodedCoverage({codec_type: 'audio', sample_rate: '48000'}, {fps: 60});
  for (let n = 0; n < 480; n++) coverage.push({time: n * 1024 / 48000, samples: 1024});
  assert.equal(coverage.verify(12).sampleCount, 480 * 1024);
  assert.throws(() => coverage.push({time: 11}), /samples/);
});

test('rejects substantial gaps even when most of the run is covered', () => {
  const coverage = new DecodedCoverage({codec_type: 'video'}, {fps: 60});
  for (let n = 0; n < 600; n++) coverage.push({time: n / 60 + (n >= 300 ? .5 : 0), duration: 1 / 60});
  assert.throws(() => coverage.verify(12), /gap/);
});

test('rejects unknown and backward timestamps', () => {
  const coverage = new DecodedCoverage({codec_type: 'video'}, {fps: 60});
  assert.throws(() => coverage.push({time: NaN}), /timestamp/);
  coverage.push({time: 1});
  assert.throws(() => coverage.push({time: 0}), /timestamp/);
});

test('accepts 60 FPS frames with millisecond-quantized Matroska durations', () => {
  const coverage = new DecodedCoverage({codec_type: 'video'}, {fps: 60});
  for (let n = 0; n < 10800; n++) coverage.push({time: Math.round(n * 1000 / 60) / 1000, duration: .016});
  assert.ok(coverage.verify(180).coveredSeconds >= 175);
});
