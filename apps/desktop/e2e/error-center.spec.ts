import { expect, test } from '@playwright/test';

async function openFixture(page: import('@playwright/test').Page) {
 await page.route('**/error-center-qa',route=>route.fulfill({contentType:'text/html',body:`<!doctype html><html><body><div id="root"></div><script type="module">import RefreshRuntime from '/@react-refresh';RefreshRuntime.injectIntoGlobalHook(window);window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;window.__vite_plugin_react_preamble_installed__=true;</script><script type="module" src="/e2e/fixtures/error-center.tsx"></script></body></html>`}));
 await page.goto('/error-center-qa');
}

test('one upper-right card groups errors, keeps focus and fits expanded long details',async({page})=>{
 await openFixture(page);
 const field=page.getByRole('textbox',{name:'Рабочее поле'});await field.focus();
 // Trigger without moving focus, as a native asynchronous failure would.
 await page.getByRole('button',{name:'Показать ошибку'}).evaluate((button:HTMLButtonElement)=>button.click());
 const center=page.getByRole('region',{name:'Ошибки приложения'});
 await expect(center).toBeVisible();await expect(field).toBeFocused();
 await expect(page.getByRole('alert')).toHaveCount(1);await expect(center.getByText('Ошибки: 2')).toBeVisible();
 const box=await center.boundingBox();expect(box!.y).toBeLessThan(80);expect(box!.x+box!.width).toBeGreaterThan(1200);
 await center.getByRole('button',{name:'Предыдущая ошибка'}).click();
 await center.getByText('Технические подробности').click();
 await center.getByRole('button',{name:'Повторить запись'}).click();await expect(page.getByLabel('Повторы',{exact:true})).toHaveText('1');
 await page.setViewportSize({width:800,height:540});
 const narrow=await center.boundingBox();expect(narrow!.x).toBeGreaterThanOrEqual(0);expect(narrow!.x+narrow!.width).toBeLessThanOrEqual(800);expect(narrow!.y+narrow!.height).toBeLessThanOrEqual(540);
 expect(await center.evaluate(node=>node.scrollWidth<=node.clientWidth)).toBe(true);
 await center.getByRole('button',{name:'Закрыть ошибку'}).click();await expect(center.getByText('Не удалось сохранить архив.')).toBeVisible();
 await center.getByRole('button',{name:'Закрыть ошибку'}).click();await expect(center).toHaveCount(0);
});

test('card recovery and dismissal work inside an open modal and after the modal closes',async({page})=>{
 await openFixture(page);await page.getByRole('button',{name:'Открыть диалог'}).click();
 const dialog=page.getByRole('dialog',{name:'Диалог проверки'});await dialog.getByRole('button',{name:'Вызвать ошибку'}).click();
 const center=page.getByRole('region',{name:'Ошибки приложения'});await expect(center).toBeVisible();
 await expect(dialog.locator('[data-error-center]')).toHaveCount(1);
 await center.getByRole('button',{name:'Предыдущая ошибка'}).click();
 await center.getByRole('button',{name:'Повторить запись'}).focus();await page.keyboard.press('Enter');
 await expect(page.getByLabel('Повторы',{exact:true})).toHaveText('1');
 await dialog.getByRole('button',{name:'Отмена',exact:true}).click();
 await expect(center).toBeVisible();await expect(page.getByRole('dialog')).toHaveCount(0);
 await center.getByRole('button',{name:'Закрыть ошибку'}).click();await expect(center.getByText('Не удалось сохранить архив.')).toBeVisible();
});
