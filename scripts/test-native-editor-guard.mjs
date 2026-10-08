// Attach only to a runner's isolated real WebView2 checkpoint.
import {chromium, expect} from '@playwright/test';
import {writeFileSync} from 'node:fs';
import {join} from 'node:path';
const port=Number(process.env.REBELLIOCAP_GUARD_PORT);
const evidence=process.env.REBELLIOCAP_GUARD_EVIDENCE;
if (!Number.isInteger(port) || !evidence) throw Error('Checkpoint port/evidence required');
const browser=await chromium.connectOverCDP(`http://127.0.0.1:${port}`);
const page=browser.contexts()[0].pages().find(page=>page.url()==='http://127.0.0.1:1420/');
if (!page) throw Error('Native main page missing');
const result={passed:false,checks:[]};
try {
  await page.getByRole('button',{name:'Действия с native-qa.mp4',exact:true}).click();
  await page.getByRole('menuitem',{name:'Редактировать клип',exact:true}).click();
  await expect(page.locator('.ed-item.ed-video')).toHaveCount(1,{timeout:40000});
  await page.getByLabel('Название проекта').fill('Native dirty guard');
  const settings=()=>page.evaluate(()=>window.__TAURI_INTERNALS__.invoke('plugin:event|emit',{event:'tray-settings',payload:null}));
  await settings();
  await expect(page.getByRole('heading',{name:'Сохранить монтаж?',exact:true})).toBeVisible();
  await page.getByRole('button',{name:'Продолжить монтаж',exact:true}).click();
  await expect(page.getByLabel('Название проекта')).toHaveValue('Native dirty guard');
  await expect(page.getByRole('heading',{name:'Сохранить монтаж?',exact:true})).toHaveCount(0);
  result.checks.push('real native tray-settings event respects dirty editor and cancellation');
  await settings();
  await expect(page.getByRole('heading',{name:'Сохранить монтаж?',exact:true})).toBeVisible();
  await page.screenshot({path:join(evidence,'native-dirty-guard.png')});
  await page.getByRole('button',{name:'Закрыть без сохранения',exact:true}).click();
  await expect(page.getByRole('dialog',{name:'Редактор клипов',exact:true})).toHaveCount(0);
  await expect(page.getByTestId('runtime-settings')).toBeVisible();
  await expect(page.getByRole('heading',{name:'Приложение',exact:true})).toBeVisible();
  result.checks.push('explicit discard completes requested native settings navigation');
  result.passed=true;
} catch(error) {result.failure=error.stack??String(error);process.exitCode=1;}
finally {
  writeFileSync(join(evidence,'native-guard-result.json'),JSON.stringify(result,null,2));
  console.log(JSON.stringify(result));
  await browser.close();
}
