import { createHash } from 'node:crypto';
import { readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

export function validateReleaseVersion(tag, versions) {
  if (!/^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(tag ?? '')) {
    throw new Error('Release tag must be a stable vMAJOR.MINOR.PATCH version.');
  }
  const version = tag.slice(1);
  if (!versions.length || versions.some(value => value !== version)) {
    throw new Error(`Desktop version must match release tag ${tag}.`);
  }
  return version;
}

export function extractReleaseNotes(tag, markdown) {
  const version = validateReleaseVersion(tag, [tag?.slice(1)]);
  const sections = markdown.replace(/\r\n/g, '\n').split(/^## /m).slice(1);
  const matching = sections.filter(section => section.split('\n', 1)[0].trim() === version);
  if (matching.length !== 1) throw new Error(`Expected one release notes section for ${tag}.`);
  const notes = matching[0].slice(matching[0].indexOf('\n') + 1).trim();
  if (!/^[-*] \S/m.test(notes)) throw new Error(`Release notes for ${tag} must describe changes in a bullet list.`);
  return notes;
}

export function createReleaseArtifacts({ directory, tag, versions, notes = '', now = new Date() }) {
  const version = validateReleaseVersion(tag, versions);
  const installers = readdirSync(directory).filter(name => name.endsWith('_x64-setup.exe'));
  if (installers.length !== 1 || installers[0] !== `RebellioCap_${version}_x64-setup.exe`) {
    throw new Error('Expected exactly one RebellioCap installer for this version and architecture.');
  }
  const name = installers[0];
  const bytes = readFileSync(join(directory, name));
  if (bytes[0] !== 0x4d || bytes[1] !== 0x5a) throw new Error('Installer is not a Windows executable.');
  let signature;
  try { signature = readFileSync(join(directory, `${name}.sig`), 'utf8').trim(); }
  catch { throw new Error('Updater signature is missing.'); }
  if (!signature) throw new Error('Updater signature is empty.');
  const manifest = {
    version, notes, pub_date: now.toISOString(),
    platforms: {
      'windows-x86_64': {
        url: `https://github.com/kirzep/RebellioCap/releases/download/${tag}/${encodeURIComponent(name)}`,
        signature,
      },
    },
  };
  writeFileSync(join(directory, 'latest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
  const sums = [name, `${name}.sig`, 'latest.json'].map(file =>
    `${createHash('sha256').update(readFileSync(join(directory, file))).digest('hex')}  ${file}`);
  writeFileSync(join(directory, 'SHA256SUMS'), `${sums.join('\n')}\n`);
  return manifest;
}

export function verifyReleaseArtifacts({ directory, tag, versions }) {
  const version = validateReleaseVersion(tag, versions);
  const name = `RebellioCap_${version}_x64-setup.exe`;
  const expected = [name, `${name}.sig`, 'latest.json'];
  const files = readdirSync(directory).sort();
  if (JSON.stringify(files) !== JSON.stringify([...expected, 'SHA256SUMS'].sort())) {
    throw new Error('Release must contain exactly the installer, signature, manifest and checksums.');
  }
  const lines = readFileSync(join(directory, 'SHA256SUMS'), 'utf8').trim().split(/\r?\n/);
  const entries = new Map();
  for (const line of lines) {
    const match = /^([a-f0-9]{64})  (.+)$/.exec(line);
    if (!match || !expected.includes(match[2]) || entries.has(match[2])) {
      throw new Error('Invalid release checksum entry.');
    }
    entries.set(match[2], match[1]);
  }
  for (const file of expected) {
    const hash = createHash('sha256').update(readFileSync(join(directory, file))).digest('hex');
    if (entries.get(file) !== hash) throw new Error(`Release checksum mismatch: ${file}.`);
  }
  const bytes = readFileSync(join(directory, name));
  if (bytes[0] !== 0x4d || bytes[1] !== 0x5a) throw new Error('Installer is not a Windows executable.');
  const signature = readFileSync(join(directory, `${name}.sig`), 'utf8').trim();
  const manifest = JSON.parse(readFileSync(join(directory, 'latest.json'), 'utf8'));
  const platform = manifest.platforms?.['windows-x86_64'];
  if (!signature || manifest.version !== version || platform?.signature !== signature ||
      platform?.url !== `https://github.com/kirzep/RebellioCap/releases/download/${tag}/${name}`) {
    throw new Error('Release manifest does not match the signed public installer.');
  }
  return manifest;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const { values: options } = parseArgs({ options: {
    tag: { type: 'string' }, directory: { type: 'string' },
    'notes-file': { type: 'string' }, 'validate-only': { type: 'boolean', default: false },
    'write-notes': { type: 'string' },
    'verify-only': { type: 'boolean', default: false },
  } });
  const root = fileURLToPath(new URL('../', import.meta.url));
  const tauri = JSON.parse(readFileSync(join(root, 'apps/desktop/src-tauri/tauri.conf.json')));
  const desktop = JSON.parse(readFileSync(join(root, 'apps/desktop/package.json')));
  const cargo = readFileSync(join(root, 'apps/desktop/src-tauri/Cargo.toml'), 'utf8');
  const cargoVersion = cargo.match(/^version\s*=\s*"([^"]+)"/m)?.[1];
  if (!options.tag) throw new Error('--tag is required.');
  const versions = [tauri.version, desktop.version, cargoVersion];
  validateReleaseVersion(options.tag, versions);
  if (options['write-notes']) {
    writeFileSync(options['write-notes'], `${extractReleaseNotes(options.tag, readFileSync(join(root, 'docs/release-notes.md'), 'utf8'))}\n`);
  } else if (options['verify-only']) {
    verifyReleaseArtifacts({ directory: options.directory ?? join(root, 'apps/desktop/src-tauri/target/release/bundle/nsis'), tag: options.tag, versions });
  } else if (!options['validate-only']) {
    const notes = options['notes-file'] ? readFileSync(options['notes-file'], 'utf8') : '';
    createReleaseArtifacts({ directory: options.directory ?? join(root, 'apps/desktop/src-tauri/target/release/bundle/nsis'), tag: options.tag, versions, notes });
  }
  console.log(`Validated release ${options.tag}.`);
}
