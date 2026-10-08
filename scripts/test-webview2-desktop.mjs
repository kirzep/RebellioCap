// Real Tauri/WebView2 smoke. Requires the debug desktop binary, staged engine,
// installed WebView2 and the Vite server at 127.0.0.1:1420. No bridge fixtures.
import { chromium, expect } from '@playwright/test';
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync, unlinkSync, rmdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createServer } from 'node:net';

const workspace = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const binary = resolve(process.env.REBELLIOCAP_WEBVIEW_BINARY ?? join(workspace, 'apps/desktop/src-tauri/target/debug/RebellioCap.exe'));
const engine = resolve(process.env.REBELLIOCAP_WEBVIEW_ENGINE ?? join(workspace, 'build/windows-hardware-release/RebellioCap.Engine.exe'));
for (const file of [binary, engine]) if (!existsSync(file)) throw new Error(`Missing native test prerequisite: ${file}`);
const server = createServer();
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const port = server.address().port;
await new Promise(resolve => server.close(resolve));
const evidence = mkdtempSync(join(tmpdir(), 'rebcap-webview2-smoke-'));
const clips = join(evidence, 'clips'); mkdirSync(clips);
const hash = file => createHash('sha256').update(readFileSync(file)).digest('hex');
const result = { passed: false, evidence, binary, binarySha256: hash(binary), engine, engineSha256: hash(engine), checks: [], errors: [] };
const child = spawn(binary, [], { windowsHide: true, cwd: workspace, env: {
  ...process.env, REBELLIOCAP_TEST_CONFIG_ROOT: join(evidence, 'config'),
  REBELLIOCAP_ENGINE_PATH: '\\\\?\\' + engine,
  WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}`,
  WEBVIEW2_USER_DATA_FOLDER: join(evidence, 'webview'),
} });
let output = '', stderr = '', browser, page;
child.stdout.on('data', data => { output += data; });
child.stderr.on('data', data => { stderr += data; });
const invoke = (command, args) => page.evaluate(({ command, args }) => window.__TAURI_INTERNALS__.invoke(command, args), { command, args });
const screenshot = name => page.screenshot({ path: join(evidence, name + '.png'), animations: 'disabled' });
const check = name => { result.checks.push(name); console.log(`PASS ${name}`); };
async function closeEditor() {
  await page.getByRole('button', { name: 'Назад', exact: true }).click();
  if (await page.getByRole('button', { name: 'Закрыть без сохранения', exact: true }).count()) {
    await page.getByRole('button', { name: 'Закрыть без сохранения', exact: true }).click();
  }
  await expect(page.getByRole('dialog', { name: 'Редактор клипов', exact: true })).toHaveCount(0);
}
async function openEditor() {
  await page.getByRole('button', { name: 'Действия с native-qa.mp4', exact: true }).click();
  await page.getByRole('menuitem', { name: 'Редактировать клип', exact: true }).click();
  await expect(page.locator('.ed-item.ed-video')).toHaveCount(1, { timeout: 40000 });
  await expect.poll(() => page.locator('.ed-screen video').evaluate(video => video.readyState)).toBeGreaterThanOrEqual(2);
}
try {
  const deadline = Date.now() + 30000;
  while (!browser && Date.now() < deadline) {
    if (child.exitCode !== null) throw new Error(`Desktop exited before CDP: ${child.exitCode}, ${stderr}`);
    try { browser = await chromium.connectOverCDP(`http://127.0.0.1:${port}`, { timeout: 1000 }); }
    catch { await new Promise(resolve => setTimeout(resolve, 100)); }
  }
  if (!browser) throw new Error('WebView2 CDP startup timeout');
  const context = browser.contexts()[0];
  await expect.poll(() => context.pages().length).toBeGreaterThanOrEqual(2);
  await expect.poll(() => context.pages().some(page => page.url().includes('overlay'))).toBe(true);
  await expect.poll(() => context.pages().some(page => page.url() === 'http://127.0.0.1:1420/')).toBe(true);
  page = context.pages().find(page => !page.url().includes('overlay'));
  const overlay = context.pages().find(page => page.url().includes('overlay'));
  overlay.on('pageerror', error => result.errors.push(`overlay: ${error.message}`));
  overlay.on('console', message => { if (message.type() === 'error') result.errors.push(`overlay: ${message.text()}`); });
  let notificationDrains = 0;
  overlay.on('request', request => {
    if (request.url() === 'http://ipc.localhost/poll_notifications') notificationDrains++;
  });
  await overlay.reload();
  await expect.poll(() => notificationDrains).toBeGreaterThan(0);
  // A drain from the document being reloaded can overlap the new initial
  // drain. Let reload startup settle before measuring event-free idle.
  await new Promise(resolve => setTimeout(resolve, 300));
  const idleDrains = notificationDrains;
  await new Promise(resolve => setTimeout(resolve, 1500));
  expect(notificationDrains).toBe(idleDrains);
  page.on('pageerror', error => result.errors.push(error.message));
  page.on('console', message => { if (message.type() === 'error') result.errors.push(message.text()); });
  await page.reload();
  expect(await page.title()).toBe('RebellioCap');
  expect(page.url()).toMatch(/^http:\/\/127\.0\.0\.1:1420\//);
  await expect(page.getByTestId('start-screen')).toBeVisible();
  check('real host bootstrap and nonblank WebView2');
  await page.getByTestId('start-button').click();
  await expect(page.getByTestId('check-continue-button')).toBeEnabled({ timeout: 40000 });
  await page.getByTestId('check-continue-button').click();
  await expect(page.getByTestId('video-monitor-select')).toBeEnabled();
  await expect(page.getByText(/Запись сохраняется в SDR/)).toBeVisible();
  await screenshot('native-video-policy');
  await page.getByTestId('onboarding-next-button').click();
  await expect(page.getByTestId('audio-step')).toBeVisible();
  if (!process.env.REBELLIOCAP_NATIVE_FOLDER_DIALOG) {
    // Automated mode seeds only the output path through real IPC. Manual mode
    // waits for an operator to complete the actual Windows folder picker.
    const saved = await invoke('get_onboarding_state');
    await invoke('save_onboarding_draft', { draft: { ...saved.draft, output_directory: clips }, lastCompletedStep: saved.onboarding.last_completed_step });
    await page.reload();
    await expect(page.getByTestId('audio-step')).toBeVisible();
  }
  for (const step of ['replay-step', 'hotkeys-step', 'preferences-step', 'test-step']) {
    await page.getByTestId('onboarding-next-button').click();
    await expect(page.getByTestId(step)).toBeVisible();
    if (step === 'replay-step' && process.env.REBELLIOCAP_NATIVE_FOLDER_DIALOG) {
      await page.getByRole('button', { name: 'Выбрать папку', exact: true }).click();
      console.log(JSON.stringify({ manualFolderPicker: true, clips, port, evidence }));
      await expect(page.locator('#output-directory')).toHaveValue(clips, { timeout: 300000 });
    }
  }
  await page.getByTestId('run-test-button').click();
  await expect(page.getByTestId('test-success-summary')).toBeVisible({ timeout: 40000 });
  const recorded = await invoke('get_onboarding_state');
  expect(recorded.last_successful_test.succeeded).toBe(true);
  expect(recorded.last_successful_test.video_packets).toBeGreaterThan(0);
  expect(recorded.last_successful_test.audio_packets).toBeGreaterThan(0);
  result.recording = recorded.last_successful_test;
  copyFileSync(recorded.last_successful_test.clip_path, join(clips, 'native-qa.mp4'));
  const originalHash = hash(join(clips, 'native-qa.mp4'));
  await screenshot('native-recording-test');
  await page.getByTestId('complete-onboarding-button').click();
  await expect(page.getByTestId('home-panel')).toBeVisible({ timeout: 30000 });
  check('native system check, six onboarding steps, hardware test and explicit completion');
  await expect(overlay.getByText('Мгновенный повтор включён', { exact: true })).toBeVisible({ timeout: 2500 });
  const preferences = await invoke('get_notification_settings');
  await invoke('set_notification_settings', { settings: { ...preferences, overlayEnabled: false, soundEnabled: false } });
  await expect(overlay.getByRole('status')).toHaveCount(0);
  await invoke('set_notification_settings', { settings: { ...preferences, overlayEnabled: true, soundEnabled: false } });
  await invoke('stop_replay');
  await expect(overlay.getByText('Мгновенный повтор выключен', { exact: true })).toBeVisible({ timeout: 2500 });
  await invoke('start_replay');
  await expect(overlay.getByText('Мгновенный повтор включён', { exact: true })).toBeVisible({ timeout: 6000 });
  const afterEvents = notificationDrains;
  await new Promise(resolve => setTimeout(resolve, 1500));
  expect(notificationDrains).toBe(afterEvents);
  result.notificationDrains = notificationDrains;
  check('native overlay is idle without events and receives queued transitions/settings through targeted readiness');
  await screenshot('native-home');
  const catalogFixture = join(clips, 'CatalogQA');
  mkdirSync(catalogFixture);
  const catalogNames = Array.from({length:10000}, (_, n) => join(catalogFixture, `catalog-${n}.mp4`));
  const catalogOwner = 'native-catalog-' + Date.now();
  try {
    for (const filename of catalogNames) writeFileSync(filename, 'catalog-only fixture');
    await invoke('begin_clip_catalog', {owner:catalogOwner});
    const request = {owner:catalogOwner,query:'',folder:null,folders:false,page:0,limit:60,refresh:true};
    const indexed = await invoke('list_clip_page', {request});
    expect(indexed.totalClips).toBeGreaterThanOrEqual(10001); expect(indexed.items).toHaveLength(60);
    const found = await invoke('list_clip_page', {request:{...request,refresh:false,query:'catalog-9999.mp4'}});
    expect(found.items).toHaveLength(1); expect(found.items[0].relativePath).toBe('CatalogQA/catalog-9999.mp4');
    const grouped = await invoke('list_clip_page', {request:{...request,refresh:false,folders:true}});
    expect(grouped.folders.find(folder => folder.name === 'CatalogQA').count).toBe(10000);
    expect(await overlay.evaluate(async owner => {
      try { await window.__TAURI_INTERNALS__.invoke('begin_clip_catalog', {owner}); return 'unexpected success'; }
      catch (error) { return String(error); }
    }, catalogOwner)).toBe('clips.main_window_required');
    const cancelled = await page.evaluate(async owner => {
      const invoke = window.__TAURI_INTERNALS__.invoke;
      await invoke('begin_clip_catalog', {owner});
      const pending = invoke('list_clip_page', {request:{owner,query:'',folder:null,folders:false,page:0,limit:60,refresh:true}}).then(() => 'unexpected success', error => String(error));
      await new Promise(resolve => setTimeout(resolve, 10));
      await invoke('release_clip_catalog', {owner});
      return pending;
    }, catalogOwner + '-cancel');
    expect(cancelled).toBe('clips.cancelled');
    await page.getByRole('button', { name: 'Клипы', exact: true }).click();
    await expect(page.getByRole('heading', {name:`Файлы · ${indexed.totalClips}`})).toBeVisible({timeout:40000});
    await expect(page.locator('button[aria-label^="Открыть catalog-"]')).toHaveCount(60);
    await page.getByRole('button', {name:'Следующая страница',exact:true}).click();
    await expect(page.getByRole('status').filter({hasText:`Страница 2 из ${indexed.pageCount}`})).toBeVisible();
    await page.getByLabel('Поиск клипов').fill('catalog-9999.mp4');
    await expect(page.locator('button[aria-label^="Открыть catalog-"]')).toHaveCount(1);
    await page.getByRole('button', {name:'По папкам',exact:true}).click();
    await expect(page.getByRole('button', {name:'Открыть папку CatalogQA',exact:true})).toBeVisible();
    await expect(page.getByText(/10000 клипов/)).toBeVisible();
    result.nativeCatalog = {totalClips:indexed.totalClips,pageCount:indexed.pageCount,fixtureFiles:10000,cancellation:cancelled};
    await screenshot('native-catalog-10000');
  } finally {
    await invoke('release_clip_catalog', {owner:catalogOwner});
    for (const filename of catalogNames) if (existsSync(filename)) unlinkSync(filename);
    rmdirSync(catalogFixture);
  }
  await page.getByLabel('Поиск клипов').fill('');
  await page.getByRole('button', {name:'Все клипы',exact:true}).click();
  await page.getByRole('button', {name:'Обновить',exact:true}).click();
  await expect(page.getByText('native-qa.mp4', {exact:true})).toBeVisible({timeout:40000});
  check('native 10000-file catalog uses bounded pages, whole-index search/folders and cancellable owner-scoped scans');
  await page.getByRole('button', { name: 'Клипы', exact: true }).click();
  await page.evaluate(async moduleUrl => {
    const { getCurrentWebviewWindow } = await import(moduleUrl);
    window.__qaImportProgress = [];
    window.__qaReleaseProgress = await getCurrentWebviewWindow().listen('editor-import-progress', event => {
      window.__qaImportProgress.push(event.payload);
    });
  }, '/@fs/' + join(workspace, 'node_modules/@tauri-apps/api/webviewWindow.js').replaceAll('\\', '/'));
  await openEditor();
  const progress = await page.evaluate(() => window.__qaImportProgress);
  expect(progress.map(event => event.phase)).toEqual(expect.arrayContaining(['analyzing', 'checking_cache', 'preparing_preview', 'publishing']));
  expect(new Set(progress.map(event => event.owner)).size).toBe(1);
  expect(new Set(progress.map(event => event.operationId)).size).toBe(1);
  expect(progress.find(event => event.phase === 'analyzing')).toMatchObject({ completed: 0, total: 1, filename: 'native-qa.mp4' });
  result.importProgress = progress;
  await page.evaluate(() => window.__qaReleaseProgress());
  check('native import publishes operation-scoped stages and real file counts');
  expect(await overlay.evaluate(async () => {
    try { await window.__TAURI_INTERNALS__.invoke('editor_begin_session', { owner: 'overlay-probe' }); return 'unexpected success'; }
    catch (error) { return String(error); }
  })).toBe('editor.main_window_required');
  const src = await page.locator('.ed-screen video').evaluate(video => video.currentSrc);
  expect(src).toMatch(/^http:\/\/asset\.localhost\//);
  const temporaryPath = decodeURIComponent(src.split('asset.localhost/')[1]);
  await page.getByRole('button', { name: 'Воспроизвести', exact: true }).click();
  await expect.poll(() => page.locator('.ed-screen video').evaluate(video => video.currentTime)).toBeGreaterThan(.2);
  await expect.poll(() => page.locator('.ed-screen audio').evaluateAll(audio => audio.every(track => track.readyState >= 2 && !track.error))).toBe(true);
  await page.getByRole('button', { name: 'Пауза', exact: true }).click();
  await screenshot('native-editor');
  await page.getByRole('button', { name: 'Полноэкранный режим', exact: true }).click();
  await expect.poll(() => invoke('plugin:window|is_fullscreen', { label: 'main' })).toBe(true);
  await closeEditor();
  await expect.poll(() => existsSync(temporaryPath)).toBe(false);
  expect(await invoke('plugin:window|is_fullscreen', { label: 'main' })).toBe(false);
  await openEditor();
  expect(await page.locator('.ed-screen video').evaluate(video => video.currentSrc)).not.toBe(src);
  check('native editor asset/audio playback, overlay ACL, fullscreen restore and preview retirement/reopen');
  // Observe real IPC network requests; Tauri's invoke property is immutable.
  let snapshotPolls = 0;
  page.on('request', request => {
    if (request.url() === 'http://ipc.localhost/get_engine_snapshot') snapshotPolls++;
  });
  await expect.poll(() => snapshotPolls).toBeGreaterThan(0);
  await page.getByRole('button', { name: 'Воспроизвести', exact: true }).click();
  await expect.poll(() => page.locator('.ed-screen video').evaluate(video => video.currentTime)).toBeGreaterThan(.2);
  await page.getByRole('button', { name: 'Свернуть окно', exact: true }).click();
  await expect.poll(() => invoke('plugin:window|is_minimized', { label: 'main' })).toBe(true);
  await expect.poll(() => page.locator('.ed-screen video').evaluate(video => video.paused), { timeout: 1500 }).toBe(true);
  await expect.poll(() => page.locator('.ed-screen audio').evaluateAll(audio => audio.every(track => track.paused))).toBe(true);
  const polls = snapshotPolls;
  await new Promise(resolve => setTimeout(resolve, 2300));
  expect(snapshotPolls).toBe(polls);
  // A second instance exercises the actual single-instance/tray show path.
  const restore = spawn(binary, [], { windowsHide: true, env: {
    ...process.env, REBELLIOCAP_TEST_CONFIG_ROOT: join(evidence, 'config'),
  } });
  await new Promise((resolve, reject) => { restore.once('error', reject); restore.once('exit', resolve); });
  await expect.poll(() => invoke('plugin:window|is_minimized', { label: 'main' })).toBe(false);
  await expect.poll(() => snapshotPolls, { timeout: 5000 }).toBeGreaterThan(polls);
  check('native minimize pauses video/audio and stops Home polling; single-instance restore resumes polling');
  await closeEditor();
  await page.getByRole('button', { name: 'Открыть native-qa.mp4', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Закрыть просмотр', exact: true })).toBeVisible();
  await expect.poll(() => page.locator('video').evaluate(video => video.readyState)).toBeGreaterThanOrEqual(2);
  await expect.poll(() => page.locator('audio').evaluateAll(audio => audio.every(track => track.readyState >= 2 && !track.error))).toBe(true);
  await expect.poll(() => page.locator('video').evaluate(video => video.currentTime)).toBeGreaterThan(.2);
  await screenshot('native-player');
  await page.getByRole('button', { name: 'Закрыть просмотр', exact: true }).click();
  expect(hash(join(clips, 'native-qa.mp4'))).toBe(originalHash);
  check('native gallery player video/audio and unchanged original');
  expect(result.errors).toEqual([]);
  check('console/page error health');
  if (process.env.REBELLIOCAP_NATIVE_FOLDER_DIALOG) {
    result.nativeFolderPicker = true;
    check('native first run includes Windows folder picker in the same onboarding pass');
  }
  if (process.env.REBELLIOCAP_WEBVIEW_MANUAL_CHECKPOINT) {
    const finished = join(evidence, 'manual-finished.signal');
    console.log(JSON.stringify({ manualCheckpoint: true, finished, port, evidence }));
    await expect.poll(() => existsSync(finished), { timeout: 300000 }).toBe(true);
  }
  result.passed = true;
} catch (error) {
  result.failure = error.stack ?? String(error);
  if (page && !page.isClosed()) await screenshot('failure').catch(() => {});
  process.exitCode = 1;
} finally {
  if (page && !page.isClosed()) await invoke('quit_application').catch(() => {});
  if (browser) await browser.close().catch(() => {});
  if (child.exitCode === null) {
    await Promise.race([new Promise(resolve => child.once('exit', resolve)), new Promise(resolve => setTimeout(resolve, 5000))]);
    if (child.exitCode === null) child.kill();
  }
  writeFileSync(join(evidence, 'stdout.log'), output);
  writeFileSync(join(evidence, 'stderr.log'), stderr);
  writeFileSync(join(evidence, 'result.json'), JSON.stringify(result, null, 2));
  console.log(JSON.stringify({ passed: result.passed, evidence, failure: result.failure }, null, 2));
}
