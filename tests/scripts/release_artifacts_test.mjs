import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, copyFileSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createReleaseArtifacts, validateReleaseVersion, verifyReleaseArtifacts } from '../../scripts/release-artifacts.mjs';

test('CLI uses the default bundle directory when --directory is absent', () => {
  const root = mkdtempSync(join(tmpdir(), 'release-cli-test-'));
  try {
    mkdirSync(join(root, 'scripts'));
    const bundles = join(root, 'apps/desktop/src-tauri/target/release/bundle/nsis');
    mkdirSync(bundles, { recursive: true });
    const script = join(root, 'scripts/release-artifacts.mjs');
    copyFileSync(fileURLToPath(new URL('../../scripts/release-artifacts.mjs', import.meta.url)), script);
    writeFileSync(join(root, 'apps/desktop/package.json'), '{"version":"0.1.8"}');
    writeFileSync(join(root, 'apps/desktop/src-tauri/tauri.conf.json'), '{"version":"0.1.8"}');
    writeFileSync(join(root, 'apps/desktop/src-tauri/Cargo.toml'), '[package]\nversion = "0.1.8"\n');
    writeFileSync(join(bundles, 'RebellioCap_0.1.8_x64-setup.exe'), 'MZfixture');
    writeFileSync(join(bundles, 'RebellioCap_0.1.8_x64-setup.exe.sig'), 'signed-update');
    const result = spawnSync(process.execPath, [script, '--tag', 'v0.1.8'], { encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
    assert.equal(JSON.parse(readFileSync(join(bundles, 'latest.json'))).version, '0.1.8');
  } finally { rmSync(root, { recursive: true, force: true }); }
});

test('stable release tag must match every desktop version', () => {
  assert.equal(validateReleaseVersion('v0.1.8', ['0.1.8', '0.1.8', '0.1.8']), '0.1.8');
  assert.throws(() => validateReleaseVersion('v0.1.8', ['0.1.7']), /version/i);
  for (const tag of ['v0.1.8-beta.1', 'v01.1.8', '0.1.8', 'v0.1.8/evil']) {
    assert.throws(() => validateReleaseVersion(tag, ['0.1.8']), /tag/i);
  }
});

test('manifest points to the signed public installer and includes exact bytes in checksums', () => {
  const root = mkdtempSync(join(tmpdir(), 'release-test-'));
  try {
    writeFileSync(join(root, 'RebellioCap_0.1.8_x64-setup.exe'), Buffer.from('MZfixture'));
    writeFileSync(join(root, 'RebellioCap_0.1.8_x64-setup.exe.sig'), 'signed-update\n');
    const manifest = createReleaseArtifacts({ directory: root, tag: 'v0.1.8', versions: ['0.1.8'], notes: 'Исправления', now: new Date('2026-10-08T12:00:00Z') });
    assert.equal(manifest.version, '0.1.8');
    assert.equal(manifest.platforms['windows-x86_64'].signature, 'signed-update');
    assert.equal(manifest.platforms['windows-x86_64'].url, 'https://github.com/kirzep/RebellioCap/releases/download/v0.1.8/RebellioCap_0.1.8_x64-setup.exe');
    assert.equal(JSON.parse(readFileSync(join(root, 'latest.json'))).notes, 'Исправления');
    assert.match(readFileSync(join(root, 'SHA256SUMS'), 'utf8'), /^[a-f0-9]{64}  RebellioCap_0.1.8_x64-setup.exe$/m);
    assert.equal(verifyReleaseArtifacts({ directory: root, tag: 'v0.1.8', versions: ['0.1.8'] }).version, '0.1.8');
    rmSync(join(root, 'RebellioCap_0.1.8_x64-setup.exe.sig'));
    assert.throws(() => createReleaseArtifacts({ directory: root, tag: 'v0.1.8', versions: ['0.1.8'] }), /signature/i);
    writeFileSync(join(root, 'RebellioCap_0.1.8_x64-setup.exe.sig'), '');
    assert.throws(() => createReleaseArtifacts({ directory: root, tag: 'v0.1.8', versions: ['0.1.8'] }), /signature/i);
  } finally { rmSync(root, { recursive: true, force: true }); }
});

test('release verification rejects changed bytes, mismatched metadata and unsafe checksum entries', () => {
  const root = mkdtempSync(join(tmpdir(), 'release-verify-'));
  const options = { directory: root, tag: 'v0.1.8', versions: ['0.1.8'] };
  const reset = () => {
    writeFileSync(join(root, 'RebellioCap_0.1.8_x64-setup.exe'), 'MZfixture');
    writeFileSync(join(root, 'RebellioCap_0.1.8_x64-setup.exe.sig'), 'signed-update');
    createReleaseArtifacts(options);
  };
  try {
    reset();
    writeFileSync(join(root, 'RebellioCap_0.1.8_x64-setup.exe'), 'MZchanged');
    assert.throws(() => verifyReleaseArtifacts(options), /checksum mismatch/i);
    reset();
    writeFileSync(join(root, 'SHA256SUMS'), `${'0'.repeat(64)}  ../outside.exe\n`);
    assert.throws(() => verifyReleaseArtifacts(options), /checksum entry/i);
    reset();
    const manifest = JSON.parse(readFileSync(join(root, 'latest.json')));
    manifest.version = '0.1.9';
    writeFileSync(join(root, 'latest.json'), JSON.stringify(manifest));
    const sums = ['RebellioCap_0.1.8_x64-setup.exe', 'RebellioCap_0.1.8_x64-setup.exe.sig', 'latest.json']
      .map(file => `${createHash('sha256').update(readFileSync(join(root, file))).digest('hex')}  ${file}`);
    writeFileSync(join(root, 'SHA256SUMS'), sums.join('\n'));
    assert.throws(() => verifyReleaseArtifacts(options), /manifest/i);
  } finally { rmSync(root, { recursive: true, force: true }); }
});
