import {expect,test} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
if(process.env.PLAYWRIGHT_CHANNEL)test.use({channel:process.env.PLAYWRIGHT_CHANNEL});
test('edits a real clip, saves a project and prepares Discord export',async({page})=>{
 const errors:string[]=[];page.on('pageerror',e=>errors.push(e.message));await page.setViewportSize({width:1120,height:780});
 const media=resolve('e2e/fixtures/editor-sample.mp4');
 await page.route('**/qa-video.mp4',route=>route.fulfill({contentType:'video/mp4',body:readFileSync(media)}));
 const asset={id:'v',name:'Момент.mp4',path:'C:/fixture.mp4',kind:'video',width:640,height:360,duration:6,audio:[1],url:'/qa-video.mp4'};
 await page.route('**/editor-qa',route=>route.fulfill({contentType:'text/html',body:`<!doctype html><html lang="ru"><head><meta charset="utf-8"></head><body><div id="root"></div><script type="module">import RefreshRuntime from '/@react-refresh';RefreshRuntime.injectIntoGlobalHook(window);window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;window.__vite_plugin_react_preamble_installed__=true;</script><script type="module">
 import React from '/node_modules/.vite/deps/react.js';import ReactDOM from '/node_modules/.vite/deps/react-dom_client.js';import {ClipEditor} from '/src/editor/ClipEditor.tsx';import '/src/styles/global.css';
 window.__TAURI_INTERNALS__={metadata:{currentWindow:{label:'main'}},convertFileSrc:path=>path,invoke:async(cmd,args)=>{if(cmd==='plugin:window|inner_size')return {width:1120,height:780};if(cmd==='plugin:window|outer_position')return {x:120,y:80};if(cmd==='plugin:window|is_maximized')return false;if(cmd==='plugin:window|set_size'){window.restoredSize=args.value.toJSON();return null;}if(cmd==='plugin:window|set_position'){window.restoredPosition=args.value;return null;}if(cmd==='plugin:window|is_fullscreen')return !!window.fullscreen;if(cmd==='plugin:window|set_fullscreen'){window.fullscreen=args.value;return null;}if(cmd==='plugin:window|minimize'){window.minimized=true;return null;}if(cmd==='editor_import')return window.nextAsset?? ${JSON.stringify(asset)};if(cmd==='editor_save'){window.saved=args;return 'C:/Момент.rebcap';}if(cmd==='editor_export'){window.exported=args;return true;}if(cmd==='editor_status')return {state:'done',bytes:1000000,path:'C:/render.mp4'};return null;}};
 const editorRoot=ReactDOM.createRoot(document.getElementById('root'));editorRoot.render(React.createElement(ClipEditor,{clipName:'Момент.mp4',onClose:()=>{window.closedEditor=true;editorRoot.unmount();}}));
 </script></body></html>`}));
 await page.goto('/editor-qa');await expect(page.locator('.ed-item')).toHaveCount(2);await expect.poll(()=>page.locator('video').evaluate((v:HTMLVideoElement)=>v.readyState)).toBeGreaterThanOrEqual(2);
 // Play remains available when viewport size and composition aspect change.
 for(const [width,height] of [[800,540],[1120,600],[1366,768],[800,400],[560,540]]){
  await page.setViewportSize({width,height});const play=page.getByRole('button',{name:'Воспроизвести',exact:true});await expect(play).toBeInViewport();
  const box=await play.boundingBox();expect(box!.width).toBeGreaterThanOrEqual(32);expect(box!.y+box!.height).toBeLessThanOrEqual(height);
  expect(await play.evaluate(el=>{const r=el.getBoundingClientRect();return el.contains(document.elementFromPoint(r.x+r.width/2,r.y+r.height/2));})).toBe(true);
 }
 await page.setViewportSize({width:1120,height:780});
 for(const selector of ['.ed-handle','.ed-level']){
  const bounds=await page.locator(selector).evaluateAll(elements=>elements.map(element=>{const box=element.getBoundingClientRect();return {width:box.width,height:box.height};}));
  expect(bounds.every(box=>box.width>=24&&box.height>=24)).toBe(true);
 }
 await page.emulateMedia({forcedColors:'active'});
 expect(await page.evaluate(()=>matchMedia('(forced-colors: active)').matches)).toBe(true);
 const contrastHandle=page.locator('.ed-item.ed-video').getByRole('button',{name:'Подрезать конец'});
 await contrastHandle.focus();
 expect(await contrastHandle.evaluate(element=>parseFloat(getComputedStyle(element).outlineWidth))).toBeGreaterThanOrEqual(2);
 expect(await page.locator('.ed-item-name').first().evaluate(element=>getComputedStyle(element).opacity)).toBe('1');
 expect(await page.getByRole('button',{name:'Экспорт',exact:true}).evaluate(element=>{const reference=document.createElement('span');reference.style.color='ButtonText';document.body.append(reference);const expected=getComputedStyle(reference).color;reference.remove();return getComputedStyle(element).color===expected;})).toBe(true);
 await page.screenshot({path:test.info().outputPath('editor-forced-colors.png'),animations:'disabled'});
 await page.emulateMedia({forcedColors:'none'});
 // Short clips must retain both a move zone and a reachable level target.
 const shortClip=page.locator('.ed-item.ed-video');
 await shortClip.click({position:{x:90,y:50}});
 await page.getByLabel('Длина, с',{exact:true}).fill('1');
 await expect.poll(()=>shortClip.evaluate(el=>parseFloat((el as HTMLElement).style.width))).toBe(30);
 expect(await shortClip.evaluate(element=>{const r=element.getBoundingClientRect();return document.elementFromPoint(r.x+r.width/2,r.bottom-10)===element;})).toBe(true);
 const shortLevel=shortClip.getByRole('slider');
 expect(await shortLevel.evaluate(element=>{const r=element.getBoundingClientRect();return element.contains(document.elementFromPoint(r.x+r.width/2,r.y+r.height/2));})).toBe(true);
 const shortBox=await shortClip.boundingBox();
 await page.mouse.move(shortBox!.x+shortBox!.width/2,shortBox!.y+50);await page.mouse.down();await page.mouse.move(shortBox!.x+shortBox!.width/2+60,shortBox!.y+50,{steps:5});await page.mouse.up();
 await expect.poll(()=>shortClip.evaluate(el=>parseFloat((el as HTMLElement).style.left))).toBe(60);
 await expect.poll(()=>shortClip.evaluate(el=>parseFloat((el as HTMLElement).style.width))).toBe(30);
 await page.getByRole('button',{name:'Отменить',exact:true}).click();
 const shortLevelBox=await shortLevel.boundingBox(),shortLevelClip=await shortClip.boundingBox();
 await page.mouse.move(shortLevelBox!.x+shortLevelBox!.width/2,shortLevelBox!.y+shortLevelBox!.height/2);await page.mouse.down();await page.mouse.move(shortLevelBox!.x+shortLevelBox!.width/2,shortLevelClip!.y+35,{steps:5});await page.mouse.up();
 await expect.poll(async()=>Number(await shortLevel.getAttribute('aria-valuenow'))).toBeLessThan(100);
 await page.getByRole('button',{name:'Отменить',exact:true}).click();
 await expect(shortLevel).toHaveAttribute('aria-valuenow','100');
 const shortRight=await shortClip.getByRole('button',{name:'Подрезать конец'}).boundingBox();
 await page.mouse.move(shortRight!.x+shortRight!.width/2,shortRight!.y+45);await page.mouse.down();await page.mouse.move(shortRight!.x+shortRight!.width/2-5,shortRight!.y+45,{steps:5});await page.mouse.up();
 await expect.poll(()=>shortClip.evaluate(el=>parseFloat((el as HTMLElement).style.width))).toBeLessThan(30);
 await page.getByRole('button',{name:'Отменить',exact:true}).click();
 await page.getByRole('button',{name:'Отменить',exact:true}).click();
 await expect.poll(()=>shortClip.evaluate(el=>parseFloat((el as HTMLElement).style.width))).toBe(180);
 const keyboardClip=page.locator('.ed-item.ed-video');const keyboardWidth=await keyboardClip.evaluate(el=>parseFloat((el as HTMLElement).style.width));
 await keyboardClip.getByRole('button',{name:'Подрезать конец'}).focus();await page.keyboard.press('ArrowLeft');
 await expect.poll(()=>keyboardClip.evaluate(el=>parseFloat((el as HTMLElement).style.width))).toBeCloseTo(keyboardWidth-.5);
 await page.getByRole('button',{name:'Отменить',exact:true}).click();await expect.poll(()=>keyboardClip.evaluate(el=>parseFloat((el as HTMLElement).style.width))).toBeCloseTo(keyboardWidth);
 await expect(page.getByRole('button',{name:'Назад',exact:true})).toBeVisible();await expect(page.locator('.ed-dot')).toHaveCount(0);
 await page.getByRole('button',{name:'Свернуть окно'}).click();await expect.poll(()=>page.evaluate(()=>(window as any).minimized)).toBe(true);await page.getByRole('button',{name:'Полноэкранный режим',exact:true}).click();await expect.poll(()=>page.evaluate(()=>(window as any).fullscreen)).toBe(true);await page.getByRole('button',{name:'Выйти из полноэкранного режима',exact:true}).click();await expect.poll(()=>page.evaluate(()=>(window as any).fullscreen)).toBe(false);expect(await page.evaluate(()=>(window as any).restoredSize.Physical)).toEqual({width:1120,height:780});
 expect(await page.locator('.ed').evaluate(el=>getComputedStyle(el).userSelect)).toBe('none');expect(await page.getByLabel('Название проекта').evaluate(el=>getComputedStyle(el).userSelect)).toBe('text');
 expect(await page.locator('.ed').evaluate(el=>{const event=new MouseEvent('contextmenu',{bubbles:true,cancelable:true});el.dispatchEvent(event);return event.defaultPrevented;})).toBe(true);
 const ruler=await page.locator('.ed-ruler').boundingBox();await page.mouse.move(ruler!.x+10,ruler!.y+15);await page.mouse.down();await page.mouse.move(ruler!.x+75,ruler!.y+15,{steps:5});await page.mouse.up();await expect(page.getByLabel('Позиция воспроизведения')).toHaveValue('2.5');await page.getByLabel('Позиция воспроизведения').fill('0');
 const target=page.getByLabel('Переместить видео');const position=await target.boundingBox();await page.mouse.move(position!.x+position!.width/2,position!.y+position!.height/2);await page.mouse.down();await page.mouse.move(position!.x+position!.width/2+20,position!.y+position!.height/2+10,{steps:5});await page.mouse.up();expect(await page.locator('video').evaluate(v=>parseFloat((v as HTMLElement).style.left))).toBeGreaterThan(0);expect(await page.locator('.ed-screen').evaluate(el=>{const rect=el.getBoundingClientRect();return getComputedStyle(el).overflow==='hidden'&&!el.contains(document.elementFromPoint(rect.right+5,rect.y+rect.height/2));})).toBe(true);await page.getByRole('button',{name:'Отменить',exact:true}).click();
 const opacity=page.getByRole('slider',{name:'Прозрачность Момент.mp4',exact:true});
 const videoItem=page.locator('.ed-item.ed-video').first();const audioItem=page.locator('.ed-item.ed-audio').first();
 async function dragLevel(control:typeof opacity,item:typeof videoItem,fraction:number){const handle=await control.boundingBox(),clip=await item.boundingBox();await page.mouse.move(handle!.x+handle!.width/2,handle!.y+handle!.height/2);await page.mouse.down();await page.mouse.move(handle!.x+handle!.width/2,clip!.y+13+(1-fraction)*(clip!.height-26),{steps:5});await page.mouse.up();}
 await dragLevel(opacity,videoItem,.3);await expect(opacity).toHaveAttribute('aria-valuenow',/^(2[8-9]|3[0-2])$/);
 await expect.poll(()=>page.locator('video').evaluate(v=>Number((v as HTMLElement).style.opacity))).toBeLessThan(.4);
 await page.getByRole('button',{name:'Отменить',exact:true}).click();await expect(opacity).toHaveAttribute('aria-valuenow','100');
 await page.getByRole('button',{name:'Повторить',exact:true}).click();await expect(opacity).toHaveAttribute('aria-valuenow',/^(2[8-9]|3[0-2])$/);
 const volume=page.getByRole('slider',{name:'Громкость Момент.mp4',exact:true});await dragLevel(volume,audioItem,.4);await expect(volume).toHaveAttribute('aria-valuenow',/^(3[8-9]|4[0-2])$/);
 await expect.poll(()=>page.locator('audio').evaluate((a:HTMLAudioElement)=>a.volume)).toBeLessThan(.5);
 await volume.focus();await page.keyboard.press('End');await expect(volume).toHaveAttribute('aria-valuenow','100');await page.keyboard.press('Home');await expect(volume).toHaveAttribute('aria-valuenow','0');await page.getByRole('button',{name:'Отменить',exact:true}).click();await page.getByRole('button',{name:'Отменить',exact:true}).click();await expect(volume).toHaveAttribute('aria-valuenow',/^(3[8-9]|4[0-2])$/);
 await page.locator('.ed-item.ed-audio').click({button:'right',position:{x:25,y:55}});await expect(page.getByRole('menuitem',{name:'Отключить звук',exact:true})).toBeVisible();await expect(page.getByRole('menuitem',{name:'Вписать в кадр',exact:true})).toHaveCount(0);await page.getByRole('menuitem',{name:'Отключить звук',exact:true}).click();await expect(volume).toHaveAttribute('aria-valuenow','0');await page.getByRole('button',{name:'Отменить',exact:true}).click();
 await page.locator('.ed-item.ed-video').click({button:'right',position:{x:25,y:55}});await expect(page.getByRole('menuitem',{name:'Вписать в кадр',exact:true})).toBeVisible();await expect(page.getByRole('menuitem',{name:'Громкость 100%',exact:true})).toHaveCount(0);await page.getByRole('menuitem',{name:'Дублировать',exact:true}).click();await expect(page.locator('.ed-item')).toHaveCount(4);await page.getByRole('button',{name:'Отменить',exact:true}).click();await expect(page.locator('.ed-item')).toHaveCount(2);
 await page.screenshot({path:test.info().outputPath('editor-levels.png'),animations:'disabled'});
 // Vertical gestures modify only their own level; both clips keep their timing.
 expect(await videoItem.evaluate(el=>(el as HTMLElement).style.left)).toBe('0px');expect(await audioItem.evaluate(el=>(el as HTMLElement).style.width)).toBe('180px');
 await page.getByRole('button',{name:'Поднять дорожку 1'}).click();await expect(page.locator('.ed-track').first().locator('.ed-video')).toHaveCount(1);await page.getByRole('button',{name:'Отменить',exact:true}).click();
 await page.locator('.ed-item.ed-video').click();await page.getByLabel('Позиция воспроизведения').fill('2');await page.getByRole('button',{name:'Разрезать',exact:true}).click();await expect(page.locator('.ed-item')).toHaveCount(4);
 const rightClip=page.locator('.ed-item.ed-video').nth(1);const rb=await rightClip.boundingBox();await page.mouse.move(rb!.x+25,rb!.y+55);await page.mouse.down();await page.mouse.move(rb!.x+29,rb!.y+55,{steps:3});await page.mouse.up();expect(await rightClip.evaluate(el=>(el as HTMLElement).style.left)).toBe('60px');
 await page.keyboard.down('Control');await page.mouse.move(rb!.x+25,rb!.y+55);await page.mouse.down();await page.mouse.move(rb!.x+29,rb!.y+55,{steps:3});await page.mouse.up();await page.keyboard.up('Control');expect(await rightClip.evaluate(el=>parseFloat((el as HTMLElement).style.left))).toBeGreaterThan(60);await page.getByRole('button',{name:'Отменить',exact:true}).click();
 await page.getByLabel('Позиция воспроизведения').fill('4');await page.locator('.ed-item.ed-video').nth(1).click();await page.getByRole('button',{name:'Разрезать',exact:true}).click();await expect(page.locator('.ed-item')).toHaveCount(6);
 await page.getByRole('button',{name:'Удалить',exact:true}).click();await expect(page.locator('.ed-item')).toHaveCount(4);
 await page.getByRole('button',{name:'Добавить текст',exact:true}).click();await page.getByLabel('Текст',{exact:true}).fill('Мой лучший момент');await expect(page.locator('.ed-text').first()).toContainText('Мой лучший момент');
 await page.getByRole('combobox',{name:'Формат кадра'}).click();await page.getByRole('option',{name:'9:16 · TikTok / Shorts',exact:true}).click();await expect.poll(async()=>{const preview=await page.locator('.ed-screen').boundingBox();return preview!.height/preview!.width;}).toBeCloseTo(1920/1080,1);
 const textTarget=page.getByLabel('Переместить текст');const textBox=await textTarget.boundingBox();await page.mouse.move(textBox!.x+textBox!.width/2,textBox!.y+textBox!.height/2);await page.mouse.down();await page.mouse.move(textBox!.x+textBox!.width/2+15,textBox!.y+textBox!.height/2+10,{steps:4});await page.mouse.up();const textHandle=await textTarget.getByRole('button',{name:'Масштабировать: правый нижний угол'}).boundingBox();await page.mouse.move(textHandle!.x+12,textHandle!.y+12);await page.mouse.down();await page.mouse.move(textHandle!.x+22,textHandle!.y+22,{steps:4});await page.mouse.up();
 await page.evaluate(()=>{(window as any).nextAsset={id:'img',name:'image.png',path:'C:/image.png',kind:'image',width:80,height:40,duration:5,audio:[],url:'data:image/svg+xml,'+encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" width="80" height="40"><rect width="80" height="40" fill="red"/></svg>')};});await page.getByRole('button',{name:'Добавить медиа',exact:true}).last().click();await expect(page.getByLabel('Переместить изображение')).toBeVisible();const imageTarget=page.getByLabel('Переместить изображение');const imageBox=await imageTarget.boundingBox();await page.mouse.move(imageBox!.x+imageBox!.width/2,imageBox!.y+imageBox!.height/2);await page.mouse.down();await page.mouse.move(imageBox!.x+imageBox!.width/2+10,imageBox!.y+imageBox!.height/2+10,{steps:4});await page.mouse.up();
 await page.getByRole('button',{name:'Сохранить',exact:true}).click();await expect.poll(()=>page.evaluate(()=>(window as any).saved?.project.items.length)).toBe(6);expect(await page.evaluate(()=>(window as any).saved.project.items.find((i:any)=>i.kind==='text').fontSize)).toBeGreaterThan(72);expect(await page.evaluate(()=>(window as any).saved.project.assets[0].url)).toBeUndefined();expect(await page.evaluate(()=>(window as any).saved.project.items.find((i:any)=>i.kind==='video').opacity)).toBeLessThan(.4);expect(await page.evaluate(()=>(window as any).saved.project.items.find((i:any)=>i.kind==='audio').gain)).toBeLessThan(.5);
 await page.screenshot({path:test.info().outputPath('editor.png'),animations:'disabled'});
 await page.getByRole('button',{name:'Экспорт',exact:true}).click();const heights=await page.getByRole('dialog',{name:'Экспорт видео'}).locator('.ed-pair input:visible,.ed-pair [role=combobox]:visible').evaluateAll(els=>els.map(el=>el.getBoundingClientRect().height));expect(new Set(heights).size).toBe(1);await page.getByLabel('Подготовить для отправки в Discord').check();await page.getByRole('combobox',{name:'Лимит загрузки Discord',exact:true}).click();await page.getByRole('option',{name:'Nitro Basic · 50 МБ',exact:true}).click();await page.locator('.ed-discord').screenshot({path:test.info().outputPath('discord-hero.png')});expect(await page.evaluate(()=>JSON.parse(localStorage.getItem('rebcap.editor.discord')!).limit)).toBe(50);await page.getByRole('button',{name:'Рендерить и сохранить',exact:true}).click();await expect.poll(()=>page.evaluate(()=>(window as any).exported?.rasters.length)).toBe(1);await expect(page.getByText('Сохранено · 1.00 МБ')).toBeVisible();expect(await page.evaluate(()=>(window as any).exported.plan.discordLimitMb)).toBe(50);await page.getByRole('button',{name:'Закрыть экспорт'}).click();await page.getByRole('button',{name:'Полноэкранный режим',exact:true}).click();await page.evaluate(()=>{(window as any).restoredSize=null;});await page.getByRole('button',{name:'Назад',exact:true}).click();await expect(page.getByRole('dialog',{name:'Редактор клипов',exact:true})).toHaveCount(0);await expect.poll(()=>page.evaluate(()=>(window as any).restoredSize?.Physical?.width)).toBe(1120);expect(await page.evaluate(()=>(window as any).fullscreen)).toBe(false);expect(errors).toEqual([]);
});
