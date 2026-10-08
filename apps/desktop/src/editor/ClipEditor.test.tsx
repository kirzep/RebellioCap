import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { StrictMode } from 'react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { invoke } from '@tauri-apps/api/core';
import { ClipEditor } from './ClipEditor';
import { requestNavigation } from './navigationGuard';
import { autosavePrefix, legacyAutosaveKey, writeAutosave } from './autosave';
import { emptyProject } from './model';
import { TimelineLevel } from './TimelineLevel';
import {subscribeImportProgress} from './importProgress';
import { newItem } from './model';

it('does not offer empty previous projects for recovery', () => {
 localStorage.setItem(autosavePrefix+'empty',JSON.stringify({version:2,id:'empty',savedAt:1,project:emptyProject()}));
 render(<ClipEditor onClose={vi.fn()}/>);
 expect(screen.queryByText('Продолжить предыдущий монтаж?')).not.toBeInTheDocument();
});
it('remembers declining recovery across editor openings', () => {
 writeAutosave(localStorage,{id:'declined',savedAt:1,project:{...emptyProject(),items:[newItem('text',0,0,5)]}});
 const view=render(<ClipEditor onClose={vi.fn()}/>);
 fireEvent.click(screen.getByRole('button',{name:'Новый проект'}));
 view.unmount();
 render(<ClipEditor onClose={vi.fn()}/>);
 expect(screen.queryByText('Продолжить предыдущий монтаж?')).not.toBeInTheDocument();
 expect(localStorage.getItem(autosavePrefix+'declined')).not.toBeNull();
});
it('keeps an embedded montage across navigation without a close prompt', async () => {
 const view=render(<ClipEditor {...{embedded:true,active:true}} onClose={vi.fn()}/>);
 fireEvent.click(screen.getByRole('button',{name:'Добавить текст'}));
 const destination=vi.fn();act(()=>requestNavigation(destination,true));
 expect(destination).toHaveBeenCalledOnce();
 expect(screen.queryByText('Сохранить монтаж?')).not.toBeInTheDocument();
 view.rerender(<ClipEditor {...{embedded:true,active:false}} onClose={vi.fn()}/>);
 view.rerender(<ClipEditor {...{embedded:true,active:true}} onClose={vi.fn()}/>);
 expect(screen.getByLabelText('Текст')).toHaveValue('Ваш текст');
 expect(screen.getByRole('region',{name:'Редактор клипов'})).toBeInTheDocument();
});
it('still protects a hidden embedded montage before application quit', () => {
 const attention=vi.fn(),quit=vi.fn();
 const view=render(<ClipEditor embedded onNeedsAttention={attention} onClose={vi.fn()}/>);
 fireEvent.click(screen.getByRole('button',{name:'Добавить текст'}));
 view.rerender(<ClipEditor embedded active={false} onNeedsAttention={attention} onClose={vi.fn()}/>);
 act(()=>requestNavigation(quit));
 expect(attention).toHaveBeenCalledOnce();expect(quit).not.toHaveBeenCalled();
 view.rerender(<ClipEditor embedded active onNeedsAttention={attention} onClose={vi.fn()}/>);
 fireEvent.click(screen.getByRole('button',{name:'Закрыть без сохранения'}));
 expect(quit).toHaveBeenCalledOnce();
});
vi.mock('./importProgress',async importOriginal=>({...await importOriginal<typeof import('./importProgress')>(),subscribeImportProgress:vi.fn(async()=>()=>{})}));

it('shows native import stages and ignores cancelled or unrelated progress',async()=>{
 let report!:(progress:any)=>void;let resolve!:(asset:unknown)=>void;
 const release=vi.fn();
 vi.mocked(subscribeImportProgress).mockImplementationOnce(async (_owner,_operation,callback)=>{report=callback;return release;});
 vi.mocked(invoke).mockImplementation(command=>command==='editor_import'?new Promise(done=>{resolve=done;}) as never:Promise.resolve(null) as never);
 render(<ClipEditor clipName="slow.mp4" onClose={vi.fn()}/>);
 await waitFor(()=>expect(typeof resolve).toBe('function'));
 const args=vi.mocked(invoke).mock.calls.find(([command])=>command==='editor_import')![1] as {owner:string;operationId:string};
 act(()=>report({owner:args.owner,operationId:args.operationId,phase:'preparing_preview',completed:1,total:3,filename:'clip.mp4'}));
 expect(screen.getByText('Готовим превью · исходник 2 из 3 · clip.mp4')).toBeInTheDocument();
 fireEvent.click(screen.getByRole('button',{name:'Отменить импорт'}));
 expect(release).toHaveBeenCalledOnce();
 act(()=>report({owner:args.owner,operationId:args.operationId,phase:'publishing',completed:3,total:3}));
 expect(screen.queryByText('Завершаем подготовку')).not.toBeInTheDocument();
 await act(async()=>resolve(null));expect(release).toHaveBeenCalledOnce();
});
it('continues importing if the optional progress subscription fails',async()=>{
 vi.mocked(subscribeImportProgress).mockRejectedValueOnce(new Error('listener unavailable'));
 await openEditor();
 expect(screen.getByRole('button',{name:'Воспроизвести'})).toBeEnabled();
 expect(screen.queryByText('Error: listener unavailable')).not.toBeInTheDocument();
});
it('releases progress after real unmount even while native import remains pending',async()=>{
 const release=vi.fn();let resolve!:(asset:unknown)=>void;
 vi.mocked(subscribeImportProgress).mockResolvedValueOnce(release);
 vi.mocked(invoke).mockImplementation(command=>command==='editor_import'?new Promise(done=>{resolve=done;}) as never:Promise.resolve(null) as never);
 const view=render(<ClipEditor clipName="slow.mp4" onClose={vi.fn()}/>);
 await waitFor(()=>expect(typeof resolve).toBe('function'));
 view.unmount();await waitFor(()=>expect(release).toHaveBeenCalledOnce());
 await act(async()=>resolve(null));expect(release).toHaveBeenCalledOnce();
});

vi.mock('./TimelineLevel', async importOriginal => {
  const actual = await importOriginal<typeof import('./TimelineLevel')>();
  return { TimelineLevel: vi.fn(actual.TimelineLevel) };
});

it('keeps preview ownership through Undo/Redo and releases it when switching projects', async () => {
  const original = vi.mocked(invoke).getMockImplementation()!;
  vi.mocked(invoke).mockImplementation((command,args) => command==='editor_import'
    ? original(command,args).then((asset:any)=>({...asset,previewDirectory:'C:/temp/owned-preview'})) as never
    : command==='editor_open' ? Promise.resolve({...emptyProject(),name:'Second'}) as never : original(command,args));
  
  await openEditor();
  fireEvent.click(screen.getByRole('button',{name:'Отменить'}));
  fireEvent.click(screen.getByRole('button',{name:'Повторить'}));
  expect(vi.mocked(invoke).mock.calls.filter(([command])=>command==='editor_release_previews')).toHaveLength(0);
  await openAnotherProject();
  await waitFor(()=>expect(invoke).toHaveBeenCalledWith('editor_release_previews',expect.objectContaining({directories:['C:/temp/owned-preview']})));
});

it('releases previews only after their last redo reference is discarded', async () => {
  const original = vi.mocked(invoke).getMockImplementation()!;
  vi.mocked(invoke).mockImplementation((command,args) => command==='editor_import'
    ? original(command,args).then((asset:any)=>({...asset,previewDirectory:'C:/temp/redo-preview'})) as never : original(command,args));
  await openEditor();
  fireEvent.click(screen.getByRole('button',{name:'Отменить'}));
  expect(vi.mocked(invoke).mock.calls.filter(([command])=>command==='editor_release_previews')).toHaveLength(0);
  fireEvent.click(screen.getByRole('button',{name:'Добавить текст'}));
  await waitFor(()=>expect(invoke).toHaveBeenCalledWith('editor_release_previews',expect.objectContaining({directories:['C:/temp/redo-preview']})));
});

it('does not persist temporary preview ownership in a saved project', async () => {
  const original = vi.mocked(invoke).getMockImplementation()!;
  vi.mocked(invoke).mockImplementation((command,args) => command==='editor_import'
    ? original(command,args).then((asset:any)=>({...asset,previewDirectory:'C:/temp/preview'})) as never : original(command,args));
  await openEditor();
  fireEvent.click(screen.getByRole('button',{name:'Сохранить'}));
  await waitFor(()=>expect(invoke).toHaveBeenCalledWith('editor_save',expect.anything()));
  const args=vi.mocked(invoke).mock.calls.find(([command])=>command==='editor_save')![1] as {project:{assets:Record<string,unknown>[]}};
  expect(args.project.assets[0]).not.toHaveProperty('previewDirectory');
});

it('uses separate native owners for a reopened editor and its delayed release', async () => {
  const first=render(<ClipEditor clipName="clip.mp4" onClose={vi.fn()}/>);
  await waitFor(()=>expect(screen.getByRole('button',{name:'Воспроизвести'})).toBeEnabled());
  const firstOwner=(vi.mocked(invoke).mock.calls.find(([command])=>command==='editor_import')![1] as {owner:string}).owner;
  expect(firstOwner).toEqual(expect.any(String));
  first.unmount();
  render(<ClipEditor clipName="clip.mp4" onClose={vi.fn()}/>);
  await waitFor(()=>expect(screen.getByRole('button',{name:'Воспроизвести'})).toBeEnabled());
  const imports=vi.mocked(invoke).mock.calls.filter(([command])=>command==='editor_import');
  const secondOwner=(imports[1][1] as {owner:string}).owner;
  expect(secondOwner).not.toBe(firstOwner);
  await waitFor(()=>expect(invoke).toHaveBeenCalledWith('editor_release',{owner:firstOwner}));
});

it('does not start an import cancelled while native session registration is pending', async () => {
  let registered!:(value:unknown)=>void;
  vi.mocked(invoke).mockImplementation(command=>command==='editor_begin_session'?new Promise(resolve=>{registered=resolve;}) as never:Promise.resolve(null) as never);
  render(<ClipEditor clipName="slow.mp4" onClose={vi.fn()}/>);
  await waitFor(()=>expect(typeof registered).toBe('function'));
  fireEvent.click(screen.getByRole('button',{name:'Отменить импорт'}));
  await act(async()=>registered(null));
  expect(vi.mocked(invoke).mock.calls.filter(([command])=>command==='editor_import')).toHaveLength(0);
  expect(screen.getByRole('button',{name:'Открыть'})).toBeEnabled();
});

it('retires preview ownership returned by an import after cancellation', async () => {
  let resolve!:(asset:unknown)=>void;
  vi.mocked(invoke).mockImplementation(command=>command==='editor_import'?new Promise(done=>{resolve=done;}) as never:Promise.resolve(null) as never);
  render(<ClipEditor clipName="slow.mp4" onClose={vi.fn()}/>);
  await waitFor(()=>expect(typeof resolve).toBe('function'));
  fireEvent.click(screen.getByRole('button',{name:'Отменить импорт'}));
  await act(async()=>resolve({id:'late',name:'late.mp4',path:'C:/late.mp4',kind:'video',duration:6,width:640,height:360,audio:[],previewDirectory:'C:/temp/late'}));
  expect(invoke).toHaveBeenCalledWith('editor_release_previews',{directories:['C:/temp/late'],owner:expect.any(String)});
  expect(screen.queryByText('late.mp4')).not.toBeInTheDocument();
});

it('trims linked clips by keyboard frames and undoes the whole adjustment', async () => {
  await openEditor();
  const widths=()=>[...document.querySelectorAll<HTMLElement>('.ed-item')].map(el=>parseFloat(el.style.width));
  const initial=widths();
  fireEvent.keyDown(screen.getAllByRole('button',{name:'Подрезать конец'})[0],{key:'ArrowLeft'});
  expect(widths()).toEqual(initial.map(width=>width-.5));
  fireEvent.click(screen.getByRole('button',{name:'Отменить'}));
  expect(widths()).toEqual(initial);
  fireEvent.keyDown(screen.getAllByRole('button',{name:'Подрезать начало'})[0],{key:'ArrowRight',shiftKey:true});
  expect(widths()).toEqual(initial.map(width=>width-5));
  for(const clip of document.querySelectorAll<HTMLElement>('.ed-item'))expect(parseFloat(clip.style.left)).toBe(5);
  fireEvent.click(screen.getByRole('button',{name:'Отменить'}));
  expect(widths()).toEqual(initial);
});

it('advances playback without rendering static timeline controls and splits at the live cursor', async () => {
  await openEditor();
  let tick!: FrameRequestCallback;
  vi.stubGlobal('requestAnimationFrame', vi.fn((callback: FrameRequestCallback) => { tick = callback; return 1; }));
  vi.stubGlobal('cancelAnimationFrame', vi.fn());
  fireEvent.click(screen.getByRole('button', { name: 'Воспроизвести' }));
  vi.mocked(TimelineLevel).mockClear();
  const now = performance.now();
  act(() => tick(now + 1000));
  act(() => tick(now + 2000));
  expect(Number((screen.getByLabelText('Позиция воспроизведения') as HTMLInputElement).value)).toBeGreaterThan(1.9);
  expect(document.querySelector('video')!.currentTime).toBeGreaterThan(1.9);
  expect(document.querySelector('audio')!.currentTime).toBeGreaterThan(1.9);
  expect(vi.mocked(TimelineLevel)).not.toHaveBeenCalled();
  // The editor root did not render on the tick, so this callback must read the
  // live clock instead of a render-time closure.
  fireEvent.click(screen.getByRole('button', { name: 'Разрезать' }));
  expect(document.querySelectorAll('.ed-item')).toHaveLength(4);
  const starts = [...document.querySelectorAll<HTMLElement>('.ed-item')].map(el => parseFloat(el.style.left));
  expect(starts.some(start => start > 57 && start < 63)).toBe(true);
});

it('reports quota failure and can retry autosave without losing the project', async()=>{
  await openEditor();
  const storage=vi.spyOn(Storage.prototype,'setItem').mockImplementation(()=>{throw new DOMException('full','QuotaExceededError');});
  vi.useFakeTimers();
  try {
    fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'Keep this edit'}});
    await act(async()=>vi.advanceTimersByTimeAsync(800));
    expect(screen.getByText(/Автокопия не сохранена/)).toBeInTheDocument();
    expect(screen.getByLabelText('Название проекта')).toHaveValue('Keep this edit');
    storage.mockRestore();
    fireEvent.click(screen.getByRole('button',{name:'Повторить автосохранение'}));
    expect(screen.getByText(/Автокопия сохранена/)).toBeInTheDocument();
  } finally { vi.useRealTimers(); }
});

it('reports unavailable storage without blocking editing',()=>{
  vi.spyOn(Storage.prototype,'getItem').mockImplementation(()=>{throw new DOMException('denied','SecurityError');});
  render(<ClipEditor onClose={vi.fn()}/>);
  expect(screen.getByText(/Автокопия недоступна/)).toBeInTheDocument();
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'Still editable'}});
  expect(screen.getByLabelText('Название проекта')).toHaveValue('Still editable');
});

it('flushes the latest edit when leaving before the autosave delay', async()=>{
  await openEditor();
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'Just edited'}});
  fireEvent.click(screen.getByRole('button',{name:'Назад'}));
  fireEvent.click(screen.getByRole('button',{name:'Закрыть без сохранения'}));
  const key=Object.keys(localStorage).find(key=>key.startsWith(autosavePrefix));
  expect(key).toBeDefined();
  expect(JSON.parse(localStorage.getItem(key!)!).project.name).toBe('Just edited');
});

it('keeps the previous project recovery when another project opens and is edited', async()=>{
  await openEditor();
  const original=vi.mocked(invoke).getMockImplementation()!;
  
  vi.mocked(invoke).mockImplementation((command,args)=>command==='editor_open'?Promise.resolve({...emptyProject(),name:'Second',items:[newItem('text',0,0,5)]}) as never:original(command,args));
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'First'}});
  await openAnotherProject();
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'Second edited'}});
  fireEvent.click(screen.getByRole('button',{name:'Назад'}));
  fireEvent.click(screen.getByRole('button',{name:'Закрыть без сохранения'}));
  const projects=Object.keys(localStorage).filter(key=>key.startsWith(autosavePrefix)).map(key=>JSON.parse(localStorage.getItem(key!)!).project.name);
  expect(projects.sort()).toEqual(['First','Second edited']);
});

it('warns about corrupt recovery without opening a broken restore dialog', async()=>{
  localStorage.setItem(legacyAutosaveKey,'{');
  render(<ClipEditor onClose={vi.fn()}/>);
  expect(screen.getByText(/Повреждённых автокопий: 1/)).toBeInTheDocument();
  expect(screen.queryByText('Продолжить предыдущий монтаж?')).not.toBeInTheDocument();
  expect(localStorage.getItem(legacyAutosaveKey)).toBe('{');
});

it('restores the selected project and retains the other recovery', async()=>{
  writeAutosave(localStorage,{id:'older',savedAt:1,project:{...emptyProject(),name:'Older',items:[newItem('text',0,0,5)]}});
  writeAutosave(localStorage,{id:'newer',savedAt:2,project:{...emptyProject(),name:'Newer',items:[newItem('text',0,0,5)]}});
  vi.mocked(invoke).mockImplementation(async(command,args)=>command==='editor_restore'?(args as {project:unknown}).project:null);
  render(<ClipEditor onClose={vi.fn()}/>);
  fireEvent.change(document.querySelector('select[aria-label="Автокопия для восстановления"]')!,{target:{value:'older'}});
  await act(async()=>fireEvent.click(screen.getByRole('button',{name:'Восстановить'})));
  expect(screen.getByLabelText('Название проекта')).toHaveValue('Older');
  expect(localStorage.getItem(autosavePrefix+'newer')).toContain('Newer');
  expect(vi.mocked(invoke)).toHaveBeenCalledWith('editor_restore',expect.objectContaining({project:expect.objectContaining({name:'Older',recoveryId:'older'})}));
});

it('defers external navigation until discard and forgets cancelled destinations', async () => {
  await openEditor();
  const settings=vi.fn(),quit=vi.fn();
  act(()=>requestNavigation(settings));
  expect(settings).not.toHaveBeenCalled();
  expect(screen.getByText('Сохранить монтаж?')).toBeInTheDocument();
  fireEvent.click(screen.getByRole('button',{name:'Продолжить монтаж'}));
  act(()=>requestNavigation(quit));
  fireEvent.click(screen.getByRole('button',{name:'Закрыть без сохранения'}));
  expect(quit).toHaveBeenCalledOnce();
  expect(settings).not.toHaveBeenCalled();
});

it('keeps the destination pending after a cancelled save and navigates after a successful save', async()=>{
  await openEditor();
  const destination=vi.fn();
  act(()=>requestNavigation(destination));
  await act(async()=>fireEvent.click(screen.getByRole('button',{name:'Сохранить .rebcap'})));
  expect(destination).not.toHaveBeenCalled();
  expect(screen.getByText('Сохранить монтаж?')).toBeInTheDocument();
  vi.mocked(invoke).mockResolvedValue('C:/saved.rebcap');
  await act(async()=>fireEvent.click(screen.getByRole('button',{name:'Сохранить .rebcap'})));
  expect(destination).toHaveBeenCalledOnce();
});

vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn(), convertFileSrc: (path: string) => path }));

it('keeps the current montage and history when opening another project exceeds its budget', async()=>{
  await openEditor();
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'Keep current montage'}});
  
  const original=vi.mocked(invoke).getMockImplementation()!;
  vi.mocked(invoke).mockImplementation((command,args)=>command==='editor_open'?Promise.reject('editor.operation_timeout'):original(command,args));
  await openAnotherProject();
  expect(screen.getByText(/Подготовка медиа превысила время ожидания/)).toBeInTheDocument();
  expect(screen.getByLabelText('Название проекта')).toHaveValue('Keep current montage');
  expect(screen.getByRole('button',{name:'Открыть'})).toBeEnabled();
  fireEvent.click(screen.getByRole('button',{name:'Отменить'}));
  expect(screen.getByLabelText('Название проекта')).toHaveValue('clip');
});

it('can cancel a pending import and keeps the montage editable', async()=>{
  let reject!:(error:unknown)=>void;
  vi.mocked(invoke).mockImplementation(command=>command==='editor_import'?new Promise((_resolve,failed)=>{reject=failed;}) as never:Promise.resolve(null) as never);
  render(<ClipEditor clipName="slow.mp4" onClose={vi.fn()}/>);
  await waitFor(()=>expect(typeof reject).toBe("function"));
  fireEvent.click(screen.getByRole('button',{name:'Отменить импорт'}));
  expect(invoke).toHaveBeenCalledWith('editor_cancel_import',{owner:expect.any(String)});
  await act(async()=>reject('editor.operation_cancelled'));
  expect(screen.getByText('Импорт отменён.')).toBeInTheDocument();
  expect(screen.getByRole('button',{name:'Открыть'})).toBeEnabled();
});

it('ignores a successful import response arriving after cancellation', async()=>{
  let resolve!:(asset:unknown)=>void;
  vi.mocked(invoke).mockImplementation(command=>command==='editor_import'?new Promise(done=>{resolve=done;}) as never:Promise.resolve(null) as never);
  render(<ClipEditor clipName="slow.mp4" onClose={vi.fn()}/>);
  await waitFor(()=>expect(typeof resolve).toBe("function"));
  fireEvent.click(screen.getByRole('button',{name:'Отменить импорт'}));
  await act(async()=>resolve({id:'late',name:'late.mp4',path:'C:/late.mp4',kind:'video',duration:6,width:640,height:360,audio:[]}));
  expect(screen.getByLabelText('Название проекта')).toHaveValue('Новый монтаж');
  expect(screen.queryByText('late.mp4')).not.toBeInTheDocument();
  expect(screen.getByText('Импорт отменён.')).toBeInTheDocument();
});

it('reports an unconfirmed cancellation instead of claiming success', async()=>{
  let resolve!:(asset:unknown)=>void;
  vi.mocked(invoke).mockImplementation(command=>command==='editor_import'?new Promise(done=>{resolve=done;}) as never:command==='editor_cancel_import'?Promise.reject('host unavailable'):Promise.resolve(null) as never);
  render(<ClipEditor clipName="slow.mp4" onClose={vi.fn()}/>);
  await waitFor(()=>expect(typeof resolve).toBe("function"));
  await act(async()=>fireEvent.click(screen.getByRole('button',{name:'Отменить импорт'})));
  await act(async()=>resolve(null));
  expect(screen.getByText(/Не удалось подтвердить отмену импорта/)).toBeInTheDocument();
  expect(screen.queryByText('Импорт отменён.')).not.toBeInTheDocument();
});

it('blocks external navigation while rendering and releases the guard on unmount', async()=>{
  await openEditor();
  const original=vi.mocked(invoke).getMockImplementation()!;
  vi.mocked(invoke).mockImplementation((command,args)=>command==='editor_export'?Promise.resolve(true) as never:original(command,args));
  fireEvent.click(screen.getByRole('button',{name:'Экспорт'}));
  await act(async()=>fireEvent.click(screen.getByRole('button',{name:'Рендерить и сохранить'})));
  const destination=vi.fn();
  act(()=>requestNavigation(destination));
  expect(destination).not.toHaveBeenCalled();
  expect(screen.getByText('Дождитесь завершения текущей операции или отмените рендер.')).toBeInTheDocument();
  expect(screen.queryByText('Сохранить монтаж?')).not.toBeInTheDocument();
  cleanup();
  requestNavigation(destination);
  expect(destination).toHaveBeenCalledOnce();
});

it('does not leave when content changes during the close prompt save', async()=>{
  await openEditor();
  let resolve!:(path:string)=>void;
  vi.mocked(invoke).mockImplementation(()=>new Promise<string>(done=>{resolve=done;}) as never);
  const destination=vi.fn();
  act(()=>requestNavigation(destination));
  fireEvent.click(screen.getByRole('button',{name:'Сохранить .rebcap'}));
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'New edit'}});
  await act(async()=>resolve('C:/saved.rebcap'));
  expect(destination).not.toHaveBeenCalled();
  expect(screen.getByText('Сохранить монтаж?')).toBeInTheDocument();
});

beforeEach(() => {
  vi.mocked(subscribeImportProgress).mockImplementation(async()=>()=>{});
  localStorage.clear();
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} });
  vi.spyOn(HTMLMediaElement.prototype, 'pause').mockImplementation(() => {});
  vi.spyOn(HTMLMediaElement.prototype, 'play').mockResolvedValue();
  vi.mocked(invoke).mockImplementation(async command => command === 'editor_import' ? {
    id: 'clip', name: 'clip.mp4', path: 'C:/clip.mp4', kind: 'video', duration: 6,
    width: 640, height: 360, audio: [1], url: 'clip.mp4',
  } : null);
});
afterEach(async () => {
  cleanup();
  await new Promise(resolve => setTimeout(resolve, 0));
  vi.restoreAllMocks(); vi.unstubAllGlobals();
});

async function openEditor() {
  render(<ClipEditor clipName="clip.mp4" onClose={vi.fn()} />);
  await waitFor(() => expect(screen.getByRole('button', { name: 'Воспроизвести' })).toBeEnabled());
}

it('returns to the saved state after undo without asking to save again', async () => {
  const onClose=vi.fn();
  render(<ClipEditor clipName="clip.mp4" onClose={onClose}/>);
  await waitFor(()=>expect(screen.getByRole('button',{name:'Воспроизвести'})).toBeEnabled());
  const original=vi.mocked(invoke).getMockImplementation()!;
  vi.mocked(invoke).mockImplementation((command,args)=>command==='editor_save'?Promise.resolve('C:/saved.rebcap') as never:original(command,args));
  await act(async()=>{fireEvent.click(screen.getByRole('button',{name:'Сохранить'}));});
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'Changed'}});
  fireEvent.click(screen.getByRole('button',{name:'Отменить'}));
  fireEvent.click(screen.getByRole('button',{name:'Назад'}));
  expect(onClose).toHaveBeenCalledOnce();
  expect(screen.queryByText('Сохранить монтаж?')).not.toBeInTheDocument();
});

it('clears undo and redo when another project opens', async () => {
  await openEditor();
  const original=vi.mocked(invoke).getMockImplementation()!;
  const project=(await import('./model')).emptyProject();
  project.name='Other project';
  
  vi.mocked(invoke).mockImplementation((command,args)=>command==='editor_open'?Promise.resolve(project) as never:original(command,args));
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'Edited old project'}});
  fireEvent.click(screen.getByRole('button',{name:'Отменить'}));
  await openAnotherProject();
  expect(screen.getByLabelText('Название проекта')).toHaveValue('Other project');
  expect(screen.getByRole('button',{name:'Отменить'})).toBeDisabled();
  expect(screen.getByRole('button',{name:'Повторить'})).toBeDisabled();
});

it('keeps later edits dirty when a pending save completes', async () => {
  await openEditor();
  const original=vi.mocked(invoke).getMockImplementation()!;
  let resolve!:(value:string)=>void;
  vi.mocked(invoke).mockImplementation((command,args)=>command==='editor_save'?new Promise<string>(done=>{resolve=done;}) as never:original(command,args));
  fireEvent.click(screen.getByRole('button',{name:'Сохранить'}));
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'After save request'}});
  await act(async()=>{resolve('C:/saved.rebcap');});
  fireEvent.click(screen.getByRole('button',{name:'Назад'}));
  expect(screen.getByText('Сохранить монтаж?')).toBeInTheDocument();
});

it('recognizes the saved revision after redo and keeps earlier revisions dirty', async () => {
  const onClose=vi.fn();
  render(<ClipEditor clipName="clip.mp4" onClose={onClose}/>);
  await waitFor(()=>expect(screen.getByRole('button',{name:'Воспроизвести'})).toBeEnabled());
  const original=vi.mocked(invoke).getMockImplementation()!;
  vi.mocked(invoke).mockImplementation((command,args)=>command==='editor_save'?Promise.resolve('C:/saved.rebcap') as never:original(command,args));
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'Saved revision'}});
  await act(async()=>{fireEvent.click(screen.getByRole('button',{name:'Сохранить'}));});
  fireEvent.click(screen.getByRole('button',{name:'Отменить'}));
  fireEvent.click(screen.getByRole('button',{name:'Назад'}));
  expect(onClose).not.toHaveBeenCalled();
  expect(screen.getByText('Сохранить монтаж?')).toBeInTheDocument();
  fireEvent.click(screen.getByRole('button',{name:'Продолжить монтаж'}));
  fireEvent.click(screen.getByRole('button',{name:'Повторить'}));
  fireEvent.click(screen.getByRole('button',{name:'Назад'}));
  expect(onClose).toHaveBeenCalledOnce();
});

it.each(['cancel','error','template'] as const)('does not mark content saved after %s', async outcome=>{
  await openEditor();
  const original=vi.mocked(invoke).getMockImplementation()!;
  vi.mocked(invoke).mockImplementation((command,args)=>command==='editor_save'?
    (outcome==='error'?Promise.reject(new Error('Disk full')):Promise.resolve(outcome==='cancel'?null:'C:/template.rebcap')) as never:original(command,args));
  await act(async()=>{fireEvent.click(screen.getByRole('button',{name:outcome==='template'?'Сохранить как шаблон':'Сохранить'}));});
  fireEvent.click(screen.getByRole('button',{name:'Назад'}));
  expect(screen.getByText('Сохранить монтаж?')).toBeInTheDocument();
});

it('ignores an old save completion after another project opens', async()=>{
  const onClose=vi.fn();
  render(<ClipEditor clipName="clip.mp4" onClose={onClose}/>);
  await waitFor(()=>expect(screen.getByRole('button',{name:'Воспроизвести'})).toBeEnabled());
  const original=vi.mocked(invoke).getMockImplementation()!;
  let opened!:(value:unknown)=>void,saved!:(value:string)=>void;
  vi.mocked(invoke).mockImplementation((command,args)=>{
    if(command==='editor_open')return new Promise(resolve=>{opened=resolve;}) as never;
    if(command==='editor_save')return new Promise(resolve=>{saved=resolve;}) as never;
    return original(command,args);
  });
  
  await openAnotherProject();
  // Keyboard save can already be pending when an open command finishes.
  fireEvent.keyDown(document.body,{ctrlKey:true,key:'s'});
  const project=(await import('./model')).emptyProject();
  project.name='New session';
  await act(async()=>{opened(project);});
  await act(async()=>{saved('C:/old.rebcap');});
  expect(screen.getByLabelText('Название проекта')).toHaveValue('New session');
  expect(screen.queryByText('Проект сохранён: C:/old.rebcap')).not.toBeInTheDocument();
  fireEvent.click(screen.getByRole('button',{name:'Назад'}));
  expect(onClose).toHaveBeenCalledOnce();
});

it('keeps history when opening a project is cancelled', async()=>{
  await openEditor();
  
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'Current project'}});
  await openAnotherProject();
  expect(screen.getByLabelText('Название проекта')).toHaveValue('Current project');
  expect(screen.getByRole('button',{name:'Отменить'})).toBeEnabled();
  fireEvent.click(screen.getByRole('button',{name:'Отменить'}));
  expect(screen.getByLabelText('Название проекта')).toHaveValue('clip');
});

it('reports rejected preview playback, stops transport and retries only on explicit play', async () => {
  await openEditor();
  vi.mocked(HTMLMediaElement.prototype.play).mockRejectedValue(new Error('unsupported codec'));
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Воспроизвести' })); });
  expect(screen.getByRole('alert')).toHaveTextContent('Не удалось воспроизвести');
  expect(screen.getByRole('alert')).toHaveTextContent('unsupported codec');
  expect(screen.getByRole('button', { name: 'Воспроизвести' })).toBeInTheDocument();
  const calls = vi.mocked(HTMLMediaElement.prototype.play).mock.calls.length;
  fireEvent.change(screen.getByLabelText('Позиция воспроизведения'), { target: { value: '1' } });
  expect(HTMLMediaElement.prototype.play).toHaveBeenCalledTimes(calls);
  vi.mocked(HTMLMediaElement.prototype.play).mockResolvedValue();
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Воспроизвести' })); });
  expect(HTMLMediaElement.prototype.play).toHaveBeenCalledTimes(calls + 2);
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('does not repeat pending play on animation ticks and ignores its rejection after pause', async () => {
  await openEditor();
  let tick!: FrameRequestCallback;
  vi.stubGlobal('requestAnimationFrame', vi.fn((callback: FrameRequestCallback) => { tick = callback; return 1; }));
  vi.stubGlobal('cancelAnimationFrame', vi.fn());
  const rejects: ((reason: unknown) => void)[] = [];
  vi.mocked(HTMLMediaElement.prototype.play).mockImplementation(() => new Promise((_, reject) => { rejects.push(reject); }));
  fireEvent.click(screen.getByRole('button', { name: 'Воспроизвести' }));
  const now = performance.now();
  act(() => tick(now + 100));
  act(() => tick(now + 200));
  expect(HTMLMediaElement.prototype.play).toHaveBeenCalledTimes(2);
  fireEvent.click(screen.getByRole('button', { name: 'Пауза' }));
  await act(async () => { rejects.forEach(reject => reject(new Error('aborted'))); });
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('pauses a pending play that completes after the window becomes hidden', async () => {
  await openEditor();
  const resolves: (() => void)[] = [];
  vi.mocked(HTMLMediaElement.prototype.play).mockImplementation(() => new Promise(resolve => { resolves.push(resolve); }));
  fireEvent.click(screen.getByRole('button', { name: 'Воспроизвести' }));
  vi.spyOn(document, 'visibilityState', 'get').mockReturnValue('hidden');
  fireEvent(document, new Event('visibilitychange'));
  vi.mocked(HTMLMediaElement.prototype.pause).mockClear();
  await act(async () => { resolves.forEach(resolve => resolve()); });
  expect(HTMLMediaElement.prototype.pause).toHaveBeenCalledTimes(2);
  expect(screen.getByRole('button', { name: 'Воспроизвести' })).toBeInTheDocument();
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('waits for a slow export status before polling again and stops after completion', async () => {
  await openEditor();
  const original = vi.mocked(invoke).getMockImplementation()!;
  let pending: ((value: unknown) => void) | undefined;
  let requests = 0;
  vi.mocked(invoke).mockImplementation((command, args) => {
    if (command === 'editor_export') return Promise.resolve(true) as never;
    if (command === 'editor_status') {
      requests++;
      return new Promise(resolve => { pending = resolve; }) as never;
    }
    return original(command, args);
  });
  fireEvent.click(screen.getByRole('button', { name: 'Экспорт' }));
  vi.useFakeTimers();
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Рендерить и сохранить' })); });
  expect(screen.getByRole('progressbar')).toBeInTheDocument();
  try {
    await act(async () => { await vi.advanceTimersByTimeAsync(1600); });
    // The native request remains unresolved for several polling periods.
    expect(requests).toBe(1);
    await act(async () => { pending!({ state: 'running', progress: .5 }); });
    expect(screen.getByRole('progressbar')).toHaveAttribute('aria-valuenow', '50');
    await act(async () => { await vi.advanceTimersByTimeAsync(400); });
    expect(requests).toBe(2);
    await act(async () => { pending!({ state: 'done', path: 'C:/export.mp4', bytes: 1000000 }); });
    expect(screen.queryByRole('progressbar')).not.toBeInTheDocument();
    expect(screen.getByText('Сохранено · 1.00 МБ')).toBeInTheDocument();
    await act(async () => { await vi.advanceTimersByTimeAsync(1600); });
    expect(requests).toBe(2);
  } finally { vi.useRealTimers(); }
});

it.each(['resolve', 'reject'] as const)('ignores a late status %s after the editor unmounts', async outcome => {
  await openEditor();
  const original = vi.mocked(invoke).getMockImplementation()!;
  let resolve!: (value: unknown) => void;
  let reject!: (reason: unknown) => void;
  let requests = 0;
  vi.mocked(invoke).mockImplementation((command, args) => {
    if (command === 'editor_export') return Promise.resolve(true) as never;
    if (command === 'editor_status') {
      requests++;
      return new Promise((res, rej) => { resolve = res; reject = rej; }) as never;
    }
    return original(command, args);
  });
  fireEvent.click(screen.getByRole('button', { name: 'Экспорт' }));
  vi.useFakeTimers();
  try {
    await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Рендерить и сохранить' })); });
    await act(async () => { await vi.advanceTimersByTimeAsync(400); });
    expect(requests).toBe(1);
    cleanup();
    await act(async () => {
      if (outcome === 'resolve') resolve({ state: 'running', progress: .5 });
      else reject(new Error('late IPC failure'));
      await vi.advanceTimersByTimeAsync(1600);
    });
    expect(requests).toBe(1);
  } finally { vi.useRealTimers(); }
});

it('retries a failed status request and stops polling on an export error', async () => {
  await openEditor();
  const original = vi.mocked(invoke).getMockImplementation()!;
  let requests = 0;
  vi.mocked(invoke).mockImplementation((command, args) => {
    if (command === 'editor_export') return Promise.resolve(true) as never;
    if (command === 'editor_status') {
      requests++;
      return (requests === 1 ? Promise.reject(new Error('IPC unavailable')) :
        Promise.resolve({ state: 'error', error: 'Диск заполнен' })) as never;
    }
    return original(command, args);
  });
  fireEvent.click(screen.getByRole('button', { name: 'Экспорт' }));
  vi.useFakeTimers();
  try {
    await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Рендерить и сохранить' })); });
    await act(async () => { await vi.advanceTimersByTimeAsync(400); });
    expect(screen.getByText('Error: IPC unavailable')).toBeInTheDocument();
    expect(screen.getByRole('progressbar')).toBeInTheDocument();
    await act(async () => { await vi.advanceTimersByTimeAsync(400); });
    expect(screen.queryByRole('progressbar')).not.toBeInTheDocument();
    expect(screen.getAllByText('Диск заполнен')).not.toHaveLength(0);
    await act(async () => { await vi.advanceTimersByTimeAsync(1600); });
    expect(requests).toBe(2);
  } finally { vi.useRealTimers(); }
});

it('keeps imported media alive during StrictMode replay and releases it once on unmount', async () => {
  const view = render(<StrictMode><ClipEditor clipName="clip.mp4" onClose={vi.fn()} /></StrictMode>);
  await waitFor(() => expect(screen.getByRole('button', { name: 'Воспроизвести' })).toBeEnabled());
  await act(async () => { await new Promise(resolve => setTimeout(resolve, 0)); });
  expect(vi.mocked(invoke).mock.calls.filter(([command]) => command === 'editor_import')).toHaveLength(1);
  expect(vi.mocked(invoke).mock.calls.filter(([command]) => command === 'editor_release')).toHaveLength(0);
  view.unmount();
  await waitFor(() => expect(vi.mocked(invoke).mock.calls.filter(([command]) => command === 'editor_release')).toHaveLength(1));
});

it('uses the extended trim interval when seeking an already active clip', async () => {
  await openEditor();
  fireEvent.click(document.querySelector('.ed-item.ed-video')!);
  fireEvent.change(screen.getByLabelText('Длина, с'), { target: { value: '2' } });
  fireEvent.change(screen.getByLabelText('Позиция воспроизведения'), { target: { value: '2' } });
  fireEvent.change(screen.getByLabelText('Позиция воспроизведения'), { target: { value: '1' } });
  expect(document.querySelector('video')!.currentTime).toBeCloseTo(1);
  expect(document.querySelector('audio')!.currentTime).toBeCloseTo(1);
  fireEvent.change(screen.getByLabelText('Длина, с'), { target: { value: '4' } });
  fireEvent.change(screen.getByLabelText('Позиция воспроизведения'), { target: { value: '3' } });
  expect(document.querySelector('video')!.currentTime).toBeCloseTo(3);
  expect(document.querySelector('audio')!.currentTime).toBeCloseTo(3);
});

it('does not seek inactive video or audio after the clip has ended', async () => {
  await openEditor();
  const videoSeek = vi.spyOn(document.querySelector('video')!, 'currentTime', 'set');
  const audioSeek = vi.spyOn(document.querySelector('audio')!, 'currentTime', 'set');
  fireEvent.change(screen.getByLabelText('Позиция воспроизведения'), { target: { value: '6' } });
  expect(videoSeek).not.toHaveBeenCalled();
  expect(audioSeek).not.toHaveBeenCalled();
});

it('updates duration, preview and tracks after editing and undoing the project', async () => {
  await openEditor();
  expect(screen.getByLabelText('Позиция воспроизведения')).toHaveAttribute('max', '6');
  fireEvent.click(screen.getByRole('button', { name: 'Добавить текст' }));
  expect(screen.getByLabelText('Текст', { exact: true })).toHaveValue('Ваш текст');
  expect(document.querySelectorAll('.ed-track')).toHaveLength(3);
  expect(document.querySelector('.ed-text')).toHaveTextContent('Ваш текст');
  fireEvent.change(screen.getByLabelText('Длина, с'), { target: { value: '10' } });
  expect(screen.getByLabelText('Позиция воспроизведения')).toHaveAttribute('max', '10');
  fireEvent.click(screen.getByRole('button', { name: 'Отменить' }));
  expect(screen.getByLabelText('Позиция воспроизведения')).toHaveAttribute('max', '6');
  fireEvent.click(screen.getByRole('button', { name: 'Отменить' }));
  expect(document.querySelector('.ed-text')).not.toBeInTheDocument();
  expect(document.querySelectorAll('.ed-track')).toHaveLength(2);
  fireEvent.click(screen.getByRole('button', { name: 'Повторить' }));
  expect(document.querySelector('.ed-text')).toHaveTextContent('Ваш текст');
  fireEvent.change(screen.getByRole('combobox',{name:'Формат кадра'}).parentElement!.querySelector('select')!, { target: { value: '1080/1920' } });
  fireEvent.click(screen.getByRole('button', { name: 'Экспорт' }));
  expect(screen.getByRole('dialog', { name: 'Экспорт видео' })).toHaveTextContent('1080 × 1920');
});

it('pauses the editor when hidden so returning cannot advance by background wall time', async () => {
  await openEditor();
  fireEvent.click(screen.getByRole('button', { name: 'Воспроизвести' }));
  expect(screen.getByRole('button', { name: 'Пауза' })).toBeInTheDocument();
  const visibility = vi.spyOn(document, 'visibilityState', 'get').mockReturnValue('hidden');
  fireEvent(document, new Event('visibilitychange'));
  expect(screen.getByRole('button', { name: 'Воспроизвести' })).toBeInTheDocument();
  const position = (screen.getByLabelText('Позиция воспроизведения') as HTMLInputElement).value;
  visibility.mockReturnValue('visible');
  fireEvent(document, new Event('visibilitychange'));
  expect(screen.getByLabelText('Позиция воспроизведения')).toHaveValue(position);
  expect(screen.getByRole('button', { name: 'Воспроизвести' })).toBeInTheDocument();
});


it('uses an app dialog and preserves the project and history when opening is declined', async()=>{
  const browserConfirm=vi.spyOn(window,'confirm').mockReturnValue(true);
  await openEditor();
  fireEvent.change(screen.getByLabelText('Название проекта'),{target:{value:'Keep montage'}});
  fireEvent.click(screen.getByRole('button',{name:'Открыть'}));
  expect(screen.getByRole('dialog',{name:'Открыть другой проект?'})).toBeInTheDocument();
  expect(browserConfirm).not.toHaveBeenCalled();
  expect(vi.mocked(invoke).mock.calls.some(([command])=>command==='editor_open')).toBe(false);
  fireEvent.click(screen.getByRole('button',{name:'Отмена'}));
  expect(screen.getByLabelText('Название проекта')).toHaveValue('Keep montage');
  fireEvent.click(screen.getByRole('button',{name:'Отменить'}));
  expect(screen.getByLabelText('Название проекта')).toHaveValue('clip');
});
it('opens another project only after accepting the app dialog', async()=>{
  await openEditor();
  fireEvent.click(screen.getByRole('button',{name:'Открыть'}));
  expect(vi.mocked(invoke).mock.calls.some(([command])=>command==='editor_open')).toBe(false);
  fireEvent.click(screen.getByRole('button',{name:'Открыть проект'}));
  await waitFor(()=>expect(invoke).toHaveBeenCalledWith('editor_open',expect.anything()));
});

async function openAnotherProject(){fireEvent.click(screen.getByRole('button',{name:'Открыть'}));if(screen.queryByRole('dialog',{name:'Открыть другой проект?'})){fireEvent.click(screen.getByRole('button',{name:'Открыть проект'}));}await act(async()=>{});}

it('renders custom export FPS options and keeps transport paused during dropdown keyboard use',async()=>{
 await openEditor();
 fireEvent.click(screen.getByRole('button',{name:'Экспорт'}));
 const fps=screen.getByRole('combobox',{name:'FPS'});
 expect(fps).toHaveTextContent('60');
 fireEvent.keyDown(fps,{key:'ArrowDown'});
 expect(screen.getByRole('listbox')).toBeInTheDocument();
 fireEvent.click(screen.getByRole('option',{name:'30'}));
 expect(fps).toHaveTextContent('30');
 fireEvent.click(screen.getByRole('button',{name:'Закрыть экспорт'}));
 const format=screen.getByRole('combobox',{name:'Формат кадра'});
 fireEvent.keyDown(format,{key:' ',code:'Space'});
 expect(screen.queryByRole('button',{name:'Пауза'})).not.toBeInTheDocument();
});
