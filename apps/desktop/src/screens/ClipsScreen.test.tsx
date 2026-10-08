import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { StrictMode } from 'react';
import type { HostBridge } from '../bridge/contracts';
import { ClipsScreen } from './ClipsScreen';

describe('ClipsScreen', () => {
  beforeEach(() => localStorage.clear());
  it('releases a late registration belonging to a replaced bridge', async () => {
    let registered!: () => void;
    const oldBegin = vi.fn().mockImplementation(() => new Promise<void>(resolve => {registered=resolve;}));
    const oldRelease = vi.fn().mockResolvedValue(undefined); const oldList = vi.fn();
    const oldBridge = {beginClipCatalog:oldBegin,releaseClipCatalog:oldRelease,listClipPage:oldList} as unknown as HostBridge;
    const view = render(<ClipsScreen bridge={oldBridge}/>);
    await waitFor(() => expect(oldBegin).toHaveBeenCalledTimes(1));
    const nextBridge = {beginClipCatalog:vi.fn().mockResolvedValue(undefined),releaseClipCatalog:vi.fn().mockResolvedValue(undefined),listClipPage:vi.fn().mockResolvedValue({items:[{name:'next.mp4',bytes:1,modifiedMs:1}],folders:[],totalClips:1,totalMatches:1,page:0,pageCount:1})} as unknown as HostBridge;
    view.rerender(<ClipsScreen bridge={nextBridge}/>);
    await screen.findByText('next.mp4');
    await waitFor(() => expect(oldRelease).toHaveBeenCalledTimes(1));
    registered();
    await waitFor(() => expect(oldRelease).toHaveBeenCalledTimes(2));
    expect(oldList).not.toHaveBeenCalled();
  });
  it('keeps a native index registered through StrictMode effect replay', async () => {
    const beginClipCatalog = vi.fn().mockResolvedValue(undefined);
    const releaseClipCatalog = vi.fn().mockResolvedValue(undefined);
    const listClipPage = vi.fn().mockResolvedValue({items:[{name:'strict.mp4',bytes:1,modifiedMs:1}],folders:[],totalClips:1,totalMatches:1,page:0,pageCount:1});
    const view = render(<StrictMode><ClipsScreen bridge={{beginClipCatalog,releaseClipCatalog,listClipPage} as unknown as HostBridge}/></StrictMode>);
    await screen.findByText('strict.mp4');
    expect(beginClipCatalog).toHaveBeenCalledTimes(1); expect(releaseClipCatalog).not.toHaveBeenCalled();
    view.unmount(); await waitFor(() => expect(releaseClipCatalog).toHaveBeenCalledTimes(1));
  });
  it('releases a registration that finishes after unmount without starting a scan', async () => {
    let registered!: () => void;
    const beginClipCatalog = vi.fn().mockImplementation(() => new Promise<void>(resolve => {registered=resolve;}));
    const listClipPage = vi.fn(); const releaseClipCatalog = vi.fn().mockResolvedValue(undefined);
    const view = render(<ClipsScreen bridge={{beginClipCatalog,listClipPage,releaseClipCatalog} as unknown as HostBridge}/>);
    await waitFor(() => expect(beginClipCatalog).toHaveBeenCalledTimes(1));
    view.unmount(); registered();
    await waitFor(() => expect(releaseClipCatalog).toHaveBeenCalledWith(beginClipCatalog.mock.calls[0][0]));
    expect(listClipPage).not.toHaveBeenCalled();
  });
  it('keeps refresh invalidation pending when a filter supersedes a rebuild', async () => {
    let resolveRefresh!: (value: unknown) => void;
    const response = (name:string) => ({items:[{name,bytes:1,modifiedMs:1}],folders:[],totalClips:2,totalMatches:1,page:0,pageCount:1});
    const listClipPage = vi.fn().mockResolvedValueOnce(response('old.mp4')).mockImplementationOnce(() => new Promise(resolve => {resolveRefresh=resolve;})).mockResolvedValue(response('new.mp4'));
    const bridge = {listClipPage} as unknown as HostBridge;
    const view = render(<ClipsScreen bridge={bridge} refreshToken="old"/>);
    await screen.findByText('old.mp4');
    view.rerender(<ClipsScreen bridge={bridge} refreshToken="new"/>);
    await waitFor(() => expect(listClipPage).toHaveBeenCalledTimes(2));
    fireEvent.change(screen.getByLabelText('Поиск клипов'),{target:{value:'new'}});
    await screen.findByText('new.mp4');
    expect(listClipPage.mock.calls[2][0].refresh).toBe(true);
    resolveRefresh(response('stale.mp4'));
    await waitFor(() => expect(screen.queryByText('stale.mp4')).not.toBeInTheDocument());
  });
  it('uses native bounded pages and releases the index owner on unmount', async () => {
    const listClipPage = vi.fn().mockImplementation(async request => ({items:[{name:`page-${request.page}.mp4`,bytes:1,modifiedMs:1}],folders:[],totalClips:10_000,totalMatches:10_000,page:request.page,pageCount:167}));
    const listClips = vi.fn(); const releaseClipCatalog = vi.fn().mockResolvedValue(undefined);
    const bridge = {listClipPage,listClips,releaseClipCatalog} as unknown as HostBridge;
    const view = render(<ClipsScreen bridge={bridge}/>);
    await screen.findByText('page-0.mp4');
    expect(listClips).not.toHaveBeenCalled();
    expect(listClipPage).toHaveBeenLastCalledWith(expect.objectContaining({page:0,limit:60,refresh:true}));
    fireEvent.click(screen.getByRole('button',{name:'Следующая страница'}));
    await screen.findByText('page-1.mp4');
    expect(listClipPage).toHaveBeenLastCalledWith(expect.objectContaining({page:1,refresh:false}));
    const owner = listClipPage.mock.calls[0][0].owner;
    view.unmount();
    await waitFor(() => expect(releaseClipCatalog).toHaveBeenCalledWith(owner));
  });
  it('ignores a late native page after search and releases a pending scan on unmount', async () => {
    let resolveOld!: (value: unknown) => void;
    const listClipPage = vi.fn().mockImplementation(request => request.query ? Promise.resolve({items:[{name:'found.mp4',bytes:1,modifiedMs:1}],folders:[],totalClips:10000,totalMatches:1,page:0,pageCount:1}) : new Promise(resolve => { resolveOld = resolve; }));
    const releaseClipCatalog = vi.fn().mockResolvedValue(undefined);
    const view = render(<ClipsScreen bridge={{listClipPage,releaseClipCatalog} as unknown as HostBridge}/>);
    await waitFor(() => expect(listClipPage).toHaveBeenCalledTimes(1));
    fireEvent.change(screen.getByLabelText('Поиск клипов'),{target:{value:'found'}});
    await screen.findByText('found.mp4');
    resolveOld({items:[{name:'stale.mp4',bytes:1,modifiedMs:1}],folders:[],totalClips:10000,totalMatches:10000,page:0,pageCount:167});
    await waitFor(() => expect(screen.queryByText('stale.mp4')).not.toBeInTheDocument());
    fireEvent.change(screen.getByLabelText('Поиск клипов'),{target:{value:''}});
    await waitFor(() => expect(listClipPage).toHaveBeenCalledTimes(3));
    view.unmount();
    await waitFor(() => expect(releaseClipCatalog).toHaveBeenCalledWith(listClipPage.mock.calls[0][0].owner));
  });
  it('labels unfinished recordings and preserves their compound extension when renaming', async () => {
    const bridge = {listClips:vi.fn().mockResolvedValue([{name:'recovered.mp4.partial',bytes:100,modifiedMs:1,recovery:true}])} as unknown as HostBridge;
    render(<ClipsScreen bridge={bridge}/>);
    await screen.findByText('recovered.mp4.partial');
    expect(screen.getByText('Незавершённая запись · конец может отсутствовать')).toBeInTheDocument();
    fireEvent.keyDown(screen.getByRole('button',{name:'Действия с recovered.mp4.partial'}),{key:'Enter'});
    fireEvent.click(await screen.findByRole('menuitem',{name:'Переименовать'}));
    expect(screen.getByLabelText('Название клипа')).toHaveValue('recovered');
  });
  it('bounds gallery cards, paginates and searches the whole catalog', async () => {
    const clips = Array.from({length: 10_000}, (_, n) => ({name:`clip-${n}.mp4`,bytes:1,modifiedMs:n}));
    const bridge = {listClips:vi.fn().mockResolvedValue(clips)} as unknown as HostBridge;
    render(<ClipsScreen bridge={bridge}/>);
    await screen.findByText('clip-9999.mp4');
    expect(document.querySelectorAll('button[aria-label^="Открыть clip-"]').length).toBe(60);
    fireEvent.click(screen.getByRole('button',{name:'Следующая страница'}));
    expect(screen.getByText('clip-9939.mp4')).toBeInTheDocument();
    expect(screen.queryByText('clip-9999.mp4')).not.toBeInTheDocument();
    fireEvent.change(screen.getByLabelText('Поиск клипов'),{target:{value:'clip-0.mp4'}});
    expect(screen.getByText('clip-0.mp4')).toBeInTheDocument();
    expect(document.querySelectorAll('button[aria-label^="Открыть clip-"]').length).toBe(1);
  });
  it('loads a game folder icon, keeps Desktop local and releases icon URLs', async () => {
    localStorage.setItem('scopeclipper.clips.view', 'folders');
    const create = vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:game-icon');
    const revoke = vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
    try {
      const folderIcon = vi.fn().mockResolvedValue([0, 0, 1, 0]);
      const bridge = { folderIcon, listClips: vi.fn().mockResolvedValue([
        { name: 'a.mp4', folder: 'Game', relativePath: 'Game/a.mp4', bytes: 1, modifiedMs: 2 },
        { name: 'b.mp4', folder: 'Desktop', relativePath: 'Desktop/b.mp4', bytes: 1, modifiedMs: 1 },
      ]) } as unknown as HostBridge;
      const view = render(<ClipsScreen bridge={bridge} />);
      expect(await screen.findByRole('img', { name: 'Значок Game' })).toHaveAttribute('src', 'blob:game-icon');
      expect(folderIcon).toHaveBeenCalledExactlyOnceWith('Game');
      expect(screen.getByLabelText('Рабочий стол')).toBeInTheDocument();
      fireEvent.error(screen.getByRole('img', { name: 'Значок Game' }));
      expect(screen.getByLabelText('Папка игры')).toBeInTheDocument();
      view.unmount();
      expect(revoke).toHaveBeenCalledWith('blob:game-icon');
    } finally { create.mockRestore(); revoke.mockRestore(); }
  });
  it('renames clips and requires confirmation before deleting', async () => {
    const renameClip = vi.fn().mockResolvedValue(undefined);
    const deleteClip = vi.fn().mockResolvedValue(undefined);
    const bridge = { listClips: vi.fn().mockResolvedValue([{ name:'sample.mp4', bytes:100, modifiedMs:1 }]), renameClip, deleteClip } as unknown as HostBridge;
    render(<ClipsScreen bridge={bridge} />);
    const trigger = await screen.findByRole('button', { name:'Действия с sample.mp4' });
    fireEvent.keyDown(trigger, { key:'Enter' });
    fireEvent.click(await screen.findByRole('menuitem', {name:'Переименовать'}));
    fireEvent.change(screen.getByLabelText('Название клипа'), { target:{ value:'Новое имя' } });
    fireEvent.click(screen.getByRole('button', {name:'Сохранить'}));
    await waitFor(() => expect(renameClip).toHaveBeenCalledWith('sample.mp4', 'Новое имя'));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    fireEvent.keyDown(screen.getByRole('button', {name:'Действия с sample.mp4'}), {key:'Enter'});
    fireEvent.click(await screen.findByRole('menuitem', {name:'Удалить'}));
    expect(deleteClip).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', {name:'Удалить'}));
    await waitFor(() => expect(deleteClip).toHaveBeenCalledWith('sample.mp4'));
  });
  it('loads video frames and releases their URLs when the gallery closes', async () => {
    const create = vi.fn().mockReturnValue('blob:clip-frame');
    const revoke = vi.fn();
    const originalCreate = URL.createObjectURL;
    const originalRevoke = URL.revokeObjectURL;
    URL.createObjectURL = create; URL.revokeObjectURL = revoke;
    try {
      const clipThumbnail = vi.fn().mockResolvedValue([66, 77]);
      const bridge = { listClips: vi.fn().mockResolvedValue([
        { name: 'clip.mp4', bytes: 1000, modifiedMs: 1000 },
      ]), clipThumbnail } as unknown as HostBridge;
      const view = render(<ClipsScreen bridge={bridge} />);
      await waitFor(() => expect(view.container.querySelector('img')).toHaveAttribute('src', 'blob:clip-frame'));
      expect(clipThumbnail).toHaveBeenCalledWith('clip.mp4');
      expect(create.mock.calls[0][0].type).toBe('image/bmp');
      view.unmount();
      expect(revoke).toHaveBeenCalledWith('blob:clip-frame');
    } finally { URL.createObjectURL = originalCreate; URL.revokeObjectURL = originalRevoke; }
  });
  it('loads host files, filters them and opens the selected file', async () => {
    const prepareClipPlayback = vi.fn().mockResolvedValue({ id: 'test', video: 'video.mp4', tracks: [] });
    const bridge = { listClips: vi.fn().mockResolvedValue([
      { name:'recording-014.mp4', bytes:1048576, modifiedMs:1000 },
      { name:'clip-002.mkv', bytes:2097152, modifiedMs:2000 },
    ]), prepareClipPlayback } as unknown as HostBridge;
    render(<ClipsScreen bridge={bridge} directory="C:/Clips" />);
    expect(await screen.findByText('recording-014.mp4')).toBeInTheDocument();
    fireEvent.change(screen.getByLabelText('Поиск клипов'), { target:{value:'clip-002'} });
    expect(screen.queryByText('recording-014.mp4')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', {name:'Открыть clip-002.mkv'}));
    await waitFor(() => expect(prepareClipPlayback).toHaveBeenCalledWith('clip-002.mkv'));
  });
  it('shows an honest empty state without example clips', async () => {
    const bridge = { listClips:vi.fn().mockResolvedValue([]) } as unknown as HostBridge;
    render(<ClipsScreen bridge={bridge} />);
    expect(await screen.findByText('Здесь появятся ваши клипы')).toBeInTheDocument();
    expect(screen.queryByRole('button', {name:/Открыть/})).not.toBeInTheDocument();
  });
  it('reports a failed catalog request and allows retry', async () => {
    const listClips = vi.fn().mockRejectedValueOnce(new Error('Папка недоступна')).mockResolvedValue([]);
    render(<ClipsScreen bridge={{listClips} as unknown as HostBridge} />);
    expect(await screen.findByText('Папка недоступна')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', {name:'Обновить список'}));
    expect(await screen.findByText('Здесь появятся ваши клипы')).toBeInTheDocument();
  });
  it('sorts all clips by saved time and folders by their newest clip', async () => {
    const clipThumbnail = vi.fn().mockRejectedValue(new Error('No preview'));
    const bridge = { listClips: vi.fn().mockResolvedValue([
      { name: 'old.mp4', folder: 'Game A', relativePath: 'Game A/old.mp4', bytes: 1, modifiedMs: 999, savedMs: 1 },
      { name: 'desktop.mp4', bytes: 1, modifiedMs: 5 },
      { name: 'new.mp4', folder: 'Game B', relativePath: 'Game B/new.mp4', bytes: 1, modifiedMs: 2, savedMs: 10 },
      { name: 'second.mp4', folder: 'Game B', relativePath: 'Game B/second.mp4', bytes: 1, modifiedMs: 8 },
    ]), clipThumbnail } as unknown as HostBridge;
    render(<ClipsScreen bridge={bridge} />);
    await screen.findByText('new.mp4');
    expect(screen.getAllByRole('button', { name: /^Открыть / }).map(button => button.getAttribute('aria-label'))).toEqual([
      'Открыть new.mp4', 'Открыть second.mp4', 'Открыть desktop.mp4', 'Открыть old.mp4',
    ]);
    fireEvent.click(screen.getByRole('button', { name: 'По папкам' }));
    expect(screen.getAllByRole('button', { name: /^Открыть папку/ }).map(button => button.getAttribute('aria-label'))).toEqual([
      'Открыть папку Game B', 'Открыть папку Desktop', 'Открыть папку Game A',
    ]);
    expect(localStorage.getItem('scopeclipper.clips.view')).toBe('folders');
    await waitFor(() => expect(clipThumbnail).toHaveBeenCalledWith('Game B/new.mp4'));
    fireEvent.click(screen.getByRole('button', { name: 'Открыть папку Game B' }));
    expect(screen.getAllByRole('button', { name: /^Открыть / }).map(button => button.getAttribute('aria-label'))).toEqual(['Открыть new.mp4', 'Открыть second.mp4']);
    fireEvent.click(screen.getByRole('button', { name: /Все папки/ }));
    fireEvent.change(screen.getByLabelText('Поиск клипов'), { target: { value: 'Game A' } });
    expect(screen.getByRole('button', { name: 'Открыть папку Game A' })).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Открыть папку Game B' })).not.toBeInTheDocument();
  });
  it('uses relative paths for same-named clips and their mutations', async () => {
    const prepareClipPlayback = vi.fn().mockResolvedValue({ id: 'test', video: 'video.mp4', tracks: [] });
    const renameClip = vi.fn().mockResolvedValue(undefined);
    const deleteClip = vi.fn().mockResolvedValue(undefined);
    const bridge = { listClips: vi.fn().mockResolvedValue([
      { name: 'same.mp4', relativePath: 'A/same.mp4', folder: 'A', bytes: 1, modifiedMs: 2 },
      { name: 'same.mp4', relativePath: 'B/same.mp4', folder: 'B', bytes: 1, modifiedMs: 1 },
    ]), prepareClipPlayback, renameClip, deleteClip } as unknown as HostBridge;
    render(<ClipsScreen bridge={bridge} />);
    await screen.findAllByText('same.mp4');
    fireEvent.click(screen.getAllByRole('button', { name: 'Открыть same.mp4' })[1]);
    await waitFor(() => expect(prepareClipPlayback).toHaveBeenCalledWith('B/same.mp4'));
    fireEvent.click(await screen.findByRole('button', { name: 'Закрыть просмотр' }));
    fireEvent.keyDown(screen.getAllByRole('button', { name: 'Действия с same.mp4' })[1], { key: 'Enter' });
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Переименовать' }));
    fireEvent.change(screen.getByLabelText('Название клипа'), { target: { value: 'new' } });
    fireEvent.click(screen.getByRole('button', { name: 'Сохранить' }));
    await waitFor(() => expect(renameClip).toHaveBeenCalledWith('B/same.mp4', 'new'));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    fireEvent.keyDown(screen.getAllByRole('button', { name: 'Действия с same.mp4' })[0], { key: 'Enter' });
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Удалить' }));
    fireEvent.click(screen.getByRole('button', { name: 'Удалить' }));
    await waitFor(() => expect(deleteClip).toHaveBeenCalledWith('A/same.mp4'));
  });
  it('keeps the compact gallery chronological despite persisted folder mode', async () => {
    localStorage.setItem('scopeclipper.clips.view', 'folders');
    const bridge = { listClips: vi.fn().mockResolvedValue([1, 4, 2, 3].map(value => ({ name: `${value}.mp4`, folder: `Game ${value}`, bytes: 1, modifiedMs: value }))) } as unknown as HostBridge;
    render(<ClipsScreen bridge={bridge} compact />);
    await screen.findByText('4.mp4');
    expect(screen.getAllByRole('button', { name: /^Открыть / }).map(button => button.getAttribute('aria-label'))).toEqual(['Открыть 4.mp4', 'Открыть 3.mp4', 'Открыть 2.mp4']);
    expect(screen.queryByRole('button', { name: 'По папкам' })).not.toBeInTheDocument();
  });

});
