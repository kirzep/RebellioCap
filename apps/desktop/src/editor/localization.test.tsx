import {act,cleanup,fireEvent,render,screen} from '@testing-library/react';
import {afterEach,beforeEach,expect,it,vi} from 'vitest';
import {applyLanguageSettings,t} from '../i18n';
import {ClipEditor} from './ClipEditor';
import {formatImportProgress} from './importProgress';
import catalog from '../i18n/editor.en.json';

vi.mock('@tauri-apps/api/core',()=>({invoke:vi.fn(async()=>null),convertFileSrc:(path:string)=>path}));
beforeEach(()=>{
 localStorage.clear();
 vi.stubGlobal('ResizeObserver',class {observe(){} disconnect(){}});
 applyLanguageSettings({preference:'en',language:'en'});
});
afterEach(()=>{
 cleanup();
 applyLanguageSettings({preference:'ru',language:'ru'});
 vi.unstubAllGlobals();
});

it('updates editor labels live while preserving project names and written text',()=>{
 render(<ClipEditor onClose={vi.fn()}/>);
 expect(screen.getByRole('dialog',{name:'Clip editor'})).toBeInTheDocument();
 expect(screen.getByLabelText('Project name')).toHaveValue('New edit');
 fireEvent.change(screen.getByLabelText('Project name'),{target:{value:'Мой проект'}});
 fireEvent.click(screen.getByRole('button',{name:'Add text'}));
 expect(screen.getByLabelText('Text')).toHaveValue('Your text');
 fireEvent.change(screen.getByLabelText('Text'),{target:{value:'Мой титр'}});
 expect(screen.getByRole('group',{name:'Move text'})).toBeInTheDocument();
 act(()=>applyLanguageSettings({preference:'ru',language:'ru'}));
 expect(screen.getByRole('dialog',{name:'Редактор клипов'})).toBeInTheDocument();
 expect(screen.getByLabelText('Название проекта')).toHaveValue('Мой проект');
 expect(screen.getByLabelText('Текст')).toHaveValue('Мой титр');
 expect(screen.getByRole('group',{name:'Переместить текст'})).toBeInTheDocument();
});

it('localizes export, timeline menus and restore-safe canonical status messages',()=>{
 render(<ClipEditor onClose={vi.fn()}/>);
 fireEvent.click(screen.getByRole('button',{name:'Add text'}));
 fireEvent.contextMenu(document.querySelector('.ed-item')!,{clientX:20,clientY:20});
 expect(screen.getByRole('menu',{name:'Clip actions'})).toBeInTheDocument();
 expect(screen.getByRole('menuitem',{name:'Split at playhead'})).toBeInTheDocument();
 fireEvent.keyDown(window,{key:'Escape'});
 fireEvent.click(screen.getByRole('button',{name:'Export'}));
 fireEvent.click(screen.getByRole('checkbox',{name:'Prepare for sharing on Discord'}));
 const dialog=screen.getByRole('dialog',{name:'Export video'});
 expect(dialog).toHaveTextContent('Bitrate, Mbps');
 expect(dialog).toHaveTextContent('Free · 20 MB');
 expect(dialog).toHaveTextContent('Render and save');
 expect(dialog.textContent).not.toMatch(/[А-Яа-яЁё]/);
 expect(t('Автокопия недоступна: storage failure. Сохраните проект в .rebcap.')).toBe('Autosave unavailable: storage failure. Save your project as .rebcap.');
});

it('translates dynamic import stages and preserves the source filename',()=>{
 expect(formatImportProgress({owner:'owner',operationId:'operation',phase:'preparing_preview',completed:1,total:3,filename:'Мой клип.mp4'})).toBe('Preparing preview · source 2 of 3 · Мой клип.mp4');
 for(const [source,translation] of Object.entries(catalog)){
  expect(translation).not.toMatch(/[А-Яа-яЁё]/);
  expect(translation.match(/\{\d+\}/g)?.sort()??[]).toEqual(source.match(/\{\d+\}/g)?.sort()??[]);
 }
});
