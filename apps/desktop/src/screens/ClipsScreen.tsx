import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Play, RefreshCw, Search, MoreHorizontal, Pencil, Trash2, Folder, Monitor, ArrowLeft } from 'lucide-react';
import { AppIcon } from '../components/AppIcon';
import { DropdownMenu } from 'radix-ui';
import type { ClipCatalogPage, ClipPlayback, HostBridge, SavedClip } from '../bridge/contracts';
import { ClipPlayer } from '../components/ClipPlayer';
import { LazyClipEditor as ClipEditor } from '../editor/LazyClipEditor';
import { normalizeError } from '../bridge/host';
import { Button, ConfirmDialog, ErrorNotice, InlineStatus } from '../components/Primitives';
import styles from './ClipsScreen.module.css';

const viewStorageKey = 'scopeclipper.clips.view';
const pageSize = 60;
function clipId(clip: SavedClip) { return clip.relativePath ?? clip.name; }
function clipFolder(clip: SavedClip) { return clip.folder || 'Desktop'; }
function clipTime(clip: SavedClip) { return clip.savedMs ?? clip.modifiedMs; }
function initialView(): 'all' | 'folders' {
  try { return localStorage.getItem(viewStorageKey) === 'folders' ? 'folders' : 'all'; } catch { return 'all'; }
}
let thumbnailWorkers = 0;
const thumbnailQueue: (() => void)[] = [];
function enqueueThumbnail(task: () => Promise<void>) {
  const run = () => {
    thumbnailWorkers++;
    void task().finally(() => {
      thumbnailWorkers--;
      thumbnailQueue.shift()?.();
    });
  };
  if (thumbnailWorkers < 2) run(); else thumbnailQueue.push(run);
  return () => {
    const index = thumbnailQueue.indexOf(run);
    if (index >= 0) thumbnailQueue.splice(index, 1);
  };
}

function ClipThumbnail({ bridge, clip }: { bridge: HostBridge; clip: SavedClip }) {
  const anchor = useRef<HTMLSpanElement>(null);
  const [src, setSrc] = useState<string>();
  const [unavailable, setUnavailable] = useState(false);
  useEffect(() => {
    let active = true;
    let started = false;
    let objectUrl: string | undefined;
    let cancelQueued = () => {};
    setSrc(undefined); setUnavailable(false);
    const load = () => {
      if (started) return;
      started = true;
      cancelQueued = enqueueThumbnail(async () => {
        if (!active) return;
        try {
          if (!bridge.clipThumbnail) throw new Error('Unavailable');
          const bytes = await bridge.clipThumbnail(clipId(clip));
          if (!active) return;
          objectUrl = URL.createObjectURL(new Blob([new Uint8Array(bytes)], { type: 'image/bmp' }));
          setSrc(objectUrl);
        } catch { if (active) setUnavailable(true); }
      });
    };
    const observer = typeof IntersectionObserver !== 'undefined' ? new IntersectionObserver(entries => {
      if (entries.some(entry => entry.isIntersecting)) { load(); observer?.disconnect(); }
    }, { rootMargin: '150px' }) : undefined;
    if (observer && anchor.current) observer.observe(anchor.current); else load();
    return () => { active = false; cancelQueued(); observer?.disconnect(); if (objectUrl) URL.revokeObjectURL(objectUrl); };
  }, [bridge, clip.name, clip.relativePath, clip.bytes, clip.modifiedMs]);
  return <span ref={anchor} className={styles.preview}>
    {src && <img src={src} alt="" onError={() => { setSrc(undefined); setUnavailable(true); }} />}
    {unavailable && <span className={styles.noPreview}>Превью недоступно</span>}
  </span>;
}

function FolderIcon({ bridge, folder, revision }: { bridge: HostBridge; folder: string; revision: string }) {
  const [src, setSrc] = useState<string>();
  useEffect(() => {
    let active = true;
    let url: string | undefined;
    setSrc(undefined);
    if (folder !== 'Desktop' && bridge.folderIcon) {
      void bridge.folderIcon(folder).then(bytes => {
        if (!active) return;
        url = URL.createObjectURL(new Blob([new Uint8Array(bytes)], { type: 'image/vnd.microsoft.icon' }));
        setSrc(url);
      }).catch(() => { /* Missing or unreadable artwork uses the folder symbol. */ });
    }
    return () => { active = false; if (url) URL.revokeObjectURL(url); };
  }, [bridge, folder, revision]);
  return <span className={styles.folderIcon}>{folder === 'Desktop'
    ? <Monitor size={24} aria-label="Рабочий стол" />
    : src ? <img src={src} alt={`Значок ${folder}`} onError={() => setSrc(undefined)} />
    : <Folder size={24} aria-label="Папка игры" />}</span>;
}

export function ClipsScreen({ bridge, directory, compact = false, onShowAll, refreshToken }: {
  bridge: HostBridge; directory?: string; compact?: boolean; onShowAll?: () => void; refreshToken?: string;
}) {
  const libraryRef = useRef<HTMLElement>(null);
  const [clips, setClips] = useState<SavedClip[]>([]);
  const [nativePage, setNativePage] = useState<ClipCatalogPage | null>(null);
  const catalogOwner = useMemo(() => crypto.randomUUID(), [bridge]);
  const activeCatalogOwner = useRef(catalogOwner);
  activeCatalogOwner.current = catalogOwner;
  const registration = useRef<{owner: string; promise: Promise<void>} | null>(null);
  const releaseTimer = useRef<{owner: string; timer: ReturnType<typeof setTimeout>} | null>(null);
  useEffect(() => {
    if (releaseTimer.current?.owner === catalogOwner) { clearTimeout(releaseTimer.current.timer); releaseTimer.current = null; }
    return () => { releaseTimer.current = {owner: catalogOwner, timer: setTimeout(() => { void bridge.releaseClipCatalog?.(catalogOwner).catch(() => {}); }, 0)}; };
  }, [bridge, catalogOwner]);
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [errorTitle, setErrorTitle] = useState('Не удалось загрузить клипы');
  const requestRef = useRef(0);
  const [query, setQuery] = useState('');
  const [view, setView] = useState(initialView);
  const [selectedFolder, setSelectedFolder] = useState<string | null>(null);
  const [pageIndex, setPageIndex] = useState(0);
  const catalogFilters = useRef({query, view, selectedFolder, pageIndex});
  catalogFilters.current = {query, view, selectedFolder, pageIndex};
  const catalogRoot = useRef<string | null>(null);
  const catalogInvalidation = useRef(0);
  useEffect(() => { setPageIndex(0); }, [query, view, selectedFolder, directory, refreshToken]);
  function changeView(next: 'all' | 'folders') {
    setView(next); setSelectedFolder(null);
    try { localStorage.setItem(viewStorageKey, next); } catch { /* Storage can be disabled. */ }
  }
  const [opening, setOpening] = useState<string | null>(null);
  const [playback, setPlayback] = useState<{ clip: SavedClip; media: ClipPlayback } | null>(null);
  const [editor, setEditor] = useState<{name?: string} | null>(null);
  const mounted = useRef(true);
  useEffect(() => { mounted.current = true; return () => { mounted.current = false; }; }, []);
  const [editing, setEditing] = useState<{ clip: SavedClip; action: 'rename' | 'delete' } | null>(null);
  const [dialogOpen, setDialogOpen] = useState(false);
  const [newName, setNewName] = useState('');
  const [mutationBusy, setMutationBusy] = useState(false);
  const [mutationError, setMutationError] = useState<string | null>(null);
  function edit(clip: SavedClip, action: 'rename' | 'delete') {
    setNewName(clip.name.replace(clip.recovery?/\.(mp4|mkv)\.partial$/i:/\.[^.]+$/, '')); setMutationError(null); setEditing({ clip, action }); setDialogOpen(true);
  }
  async function mutate() {
    if (!editing || mutationBusy) return;
    setMutationBusy(true); setMutationError(null);
    try {
      if (editing.action === 'rename') {
        if (!bridge.renameClip) throw new Error('Переименование недоступно.');
        await bridge.renameClip(clipId(editing.clip), newName);
      } else {
        if (!bridge.deleteClip) throw new Error('Удаление недоступно.');
        await bridge.deleteClip(clipId(editing.clip));
      }
      setDialogOpen(false); await load();
    } catch (error) { setMutationError(normalizeError(error).summary); }
    finally { setMutationBusy(false); }
  }
  const load = useCallback(async (refresh = true) => {
    const request = ++requestRef.current;
    if (bridge.listClipPage && refresh) catalogInvalidation.current++;
    setBusy(true); setError(null);
    try {
      if (bridge.listClipPage) {
        if (registration.current?.owner !== catalogOwner) {
          const promise = (bridge.beginClipCatalog?.(catalogOwner) ?? Promise.resolve()).catch(error => {
            if (registration.current?.owner === catalogOwner) registration.current = null;
            throw error;
          });
          registration.current = {owner:catalogOwner,promise};
        }
        await registration.current.promise;
        if (!mounted.current || activeCatalogOwner.current !== catalogOwner) { await bridge.releaseClipCatalog?.(catalogOwner); return; }
        if (request !== requestRef.current) return;
        const filters = catalogFilters.current;
        const root = `${catalogOwner}\0${directory ?? ''}\0${refreshToken ?? ''}\0${catalogInvalidation.current}`;
        const rebuild = refresh || catalogRoot.current !== root;
        const result = await bridge.listClipPage({owner:catalogOwner,query:compact?'':filters.query,folder:compact?null:filters.selectedFolder,folders:!compact&&filters.view==='folders'&&!filters.selectedFolder,page:compact?0:filters.pageIndex,limit:compact?3:pageSize,refresh:rebuild});
        if (request === requestRef.current) { catalogRoot.current = root; setClips(result.items); setNativePage(result); }
        return;
      }
      if (!bridge.listClips) throw new Error('Каталог клипов недоступен в этой версии приложения.');
      const result = await bridge.listClips();
      if (request === requestRef.current) { setClips(result); setNativePage(null); }
    } catch (error) { if (request === requestRef.current) { setErrorTitle('Не удалось загрузить клипы'); setError(normalizeError(error).summary); } }
    finally { if (request === requestRef.current) setBusy(false); }
  }, [bridge, directory, catalogOwner, compact, refreshToken]);
  useEffect(() => {
    const timer = bridge.listClipPage ? setTimeout(() => { void load(false); }, 200) : undefined;
    if (!bridge.listClipPage) void load();
    return () => { requestRef.current++; if (timer !== undefined) clearTimeout(timer); };
  }, [load, refreshToken, bridge.listClipPage ? query : null, bridge.listClipPage ? view : null, bridge.listClipPage ? selectedFolder : null, bridge.listClipPage ? pageIndex : null]);
  const sorted = useMemo(() => [...clips].sort((a, b) => clipTime(b) - clipTime(a) || clipId(a).localeCompare(clipId(b))), [clips]);
  const search = query.trim().toLocaleLowerCase();
  const matches = useCallback((clip: SavedClip) => clip.name.toLocaleLowerCase().includes(search) || clipFolder(clip).toLocaleLowerCase().includes(search), [search]);
  const filtered = useMemo(() => sorted.filter(clip => matches(clip) && (!selectedFolder || clipFolder(clip) === selectedFolder)), [sorted, matches, selectedFolder]);
  const groups = useMemo(() => {
    const groups = new Map<string, SavedClip[]>();
    for (const clip of sorted) {
    const folder = clipFolder(clip);
    const group = groups.get(folder);
    if (group) group.push(clip); else groups.set(folder, [clip]);
    }
    return groups;
  }, [sorted]);
  const folders = useMemo(() => nativePage ? nativePage.folders.map(folder => [folder.name, [folder.latest]] as [string,SavedClip[]]) : [...groups.entries()].filter(([, items]) => items.some(matches)), [groups, matches, nativePage]);
  const showFolders = !compact && view === 'folders' && !selectedFolder;
  const pageCount = nativePage?.pageCount ?? Math.max(1, Math.ceil((showFolders ? folders.length : filtered.length) / pageSize));
  const page = nativePage?.page ?? Math.min(pageIndex, pageCount - 1);
  function goToPage(next: number) {
    setPageIndex(next);
    libraryRef.current?.scrollIntoView?.({ block: 'start' });
  }
  const visible = nativePage ? clips : compact ? sorted.slice(0, 3) : filtered.slice(page * pageSize, (page + 1) * pageSize);
  const visibleFolders = nativePage ? folders : folders.slice(page * pageSize, (page + 1) * pageSize);
  const empty = showFolders ? folders.length === 0 : visible.length === 0;
  async function open(clip: SavedClip) {
    setOpening(clipId(clip)); setError(null);
    try {
      if (!bridge.prepareClipPlayback) throw new Error('Встроенный просмотр недоступен. Обновите приложение.');
      const media = await bridge.prepareClipPlayback(clipId(clip));
      if (!mounted.current) { await bridge.releaseClipPlayback?.(media.id); return; }
      setPlayback({ clip, media });
    } catch (error) { setErrorTitle('Не удалось открыть клип'); setError(normalizeError(error).summary); }
    finally { setOpening(null); }
  }
  return <section ref={libraryRef} className={styles.library} aria-label={compact ? 'Последние клипы' : 'Все клипы'}>
    {editor && <ClipEditor clipName={editor.name} onClose={() => setEditor(null)} />}
    {playback && <ClipPlayer key={playback.media.id} name={playback.clip.name} media={playback.media} onClose={() => setPlayback(null)} onRelease={id => { void bridge.releaseClipPlayback?.(id).catch(() => {}); }} />}
    {opening && <InlineStatus tone="busy">Подготавливаем клип для просмотра…</InlineStatus>}
    <header className={styles.header}>
      <div><h2>{compact ? 'Последние клипы' : busy ? 'Загружаем библиотеку' : `Файлы · ${nativePage?.totalClips ?? clips.length}`}</h2></div>
      {compact ? <Button variant="tertiary" size="compact" onClick={onShowAll}>Все клипы →</Button>
        : <Button variant="tertiary" size="compact" leadingIcon={<RefreshCw />} busy={busy} onClick={() => void load()}>Обновить</Button>}
    </header>
    {!compact && <div className={styles.toolbar}>
      <div className={styles.viewSwitch} aria-label="Вид библиотеки">
        <button aria-pressed={view === 'all'} onClick={() => changeView('all')}>Все клипы</button>
        <button aria-pressed={view === 'folders'} onClick={() => changeView('folders')}>По папкам</button>
      </div>
      {view === 'folders' && selectedFolder && <button className={styles.back} onClick={() => setSelectedFolder(null)}><ArrowLeft size={16} />Все папки<span>{selectedFolder}</span></button>}
      <label className={styles.search}><Search size={16} /><input value={query} onChange={e => setQuery(e.target.value)} placeholder="Найти клип или папку" aria-label="Поиск клипов" /></label></div>}
    {!compact && <Button variant="tertiary" size="compact" leadingIcon={<Pencil />} onClick={() => setEditor({})}>Редактор клипов · Новый проект / .rebcap</Button>}
    {error && <ErrorNotice title={errorTitle} action={<Button size="compact" onClick={() => void load()}>Обновить список</Button>}>{error}</ErrorNotice>}
    {busy ? <InlineStatus tone="busy">Загружаем клипы…</InlineStatus> : !error && empty ?
      <div className={styles.empty}><AppIcon name="clips" size={28} /><h3>{query ? 'Клипы не найдены' : 'Здесь появятся ваши клипы'}</h3><p>{query ? 'Попробуйте другое название.' : 'Сохраните повтор или завершите запись. Файлы из папки записи появятся здесь.'}</p></div>
      : showFolders ? <div className={styles.grid}>{visibleFolders.map(([folder, items]) => <article className={styles.clip} key={folder}>
        <button className={styles.thumbnail} onClick={() => setSelectedFolder(folder)} aria-label={`Открыть папку ${folder}`}>
          <ClipThumbnail bridge={bridge} clip={items[0]} />
        </button>
        <div className={styles.folderTitle}><FolderIcon bridge={bridge} folder={folder} revision={`${clipId(items[0])}:${clipTime(items[0])}:${refreshToken ?? ''}`} /><h3 title={folder}>{folder}</h3></div>
        <p>{nativePage?.folders.find(item => item.name === folder)?.count ?? items.length} клипов · {new Date(clipTime(items[0])).toLocaleDateString('ru-RU', { day: 'numeric', month: 'short' })}</p>
      </article>)}</div>
      : <div className={styles.grid}>{visible.map(clip => <article className={styles.clip} key={clipId(clip)}>
        <button className={styles.thumbnail} disabled={opening !== null} onClick={() => void open(clip)} aria-label={`Открыть ${clip.name}`}>
          <ClipThumbnail bridge={bridge} clip={clip} />
          <span className={styles.play}><Play size={20} /></span><span className={styles.extension}>{clip.recovery?'Копия':clip.name.split('.').pop()?.toUpperCase()}</span>
        </button><div className={styles.clipTitle}><h3 title={clip.name}>{clip.name}</h3>
          <DropdownMenu.Root><DropdownMenu.Trigger asChild><button className={styles.more} aria-label={`Действия с ${clip.name}`}><MoreHorizontal size={18} /></button></DropdownMenu.Trigger>
            <DropdownMenu.Portal><DropdownMenu.Content className={styles.menu} align="end" sideOffset={5} collisionPadding={12}>
              <DropdownMenu.Item className={styles.menuItem} onSelect={() => setEditor({name: clipId(clip)})}><Pencil size={15} />Редактировать клип</DropdownMenu.Item>
              <DropdownMenu.Item className={styles.menuItem} onSelect={() => edit(clip, 'rename')}><Pencil size={15} />Переименовать</DropdownMenu.Item>
              <DropdownMenu.Item className={`${styles.menuItem} ${styles.deleteItem}`} onSelect={() => edit(clip, 'delete')}><Trash2 size={15} />Удалить</DropdownMenu.Item>
            </DropdownMenu.Content></DropdownMenu.Portal>
          </DropdownMenu.Root>
        </div>{clip.recovery&&<p>Незавершённая запись · конец может отсутствовать</p>}<p>{!compact && view === 'all' && <span>{clipFolder(clip)} · </span>}{new Date(clipTime(clip)).toLocaleDateString('ru-RU', {day:'numeric', month:'short'})} · {(clip.bytes / 1024 / 1024).toLocaleString('ru-RU', {maximumFractionDigits:1})} МБ</p>
      </article>)}</div>}
    {!compact && !busy && !error && pageCount > 1 && <nav className={styles.pagination} aria-label="Страницы библиотеки">
      <Button variant="tertiary" size="compact" disabled={page === 0} onClick={() => goToPage(page - 1)}>Предыдущая страница</Button>
      <span role="status">Страница {page + 1} из {pageCount}</span>
      <Button variant="tertiary" size="compact" disabled={page === pageCount - 1} onClick={() => goToPage(page + 1)}>Следующая страница</Button>
    </nav>}
    {!compact && directory && <p className={styles.directory}>Папка записи <span>{directory}</span></p>}
    <ConfirmDialog open={dialogOpen} title={editing?.action === 'delete' ? 'Удалить клип?' : 'Переименовать клип'}
      description={editing?.action === 'delete' ? `«${editing.clip.name}» будет перемещён в корзину Windows.` : 'Расширение файла сохранится автоматически.'}
      confirmLabel={editing?.action === 'delete' ? 'Удалить' : 'Сохранить'} destructive={editing?.action === 'delete'} busy={mutationBusy} busyLabel={editing?.action === 'delete' ? 'Удаляем…' : 'Сохраняем…'}
      onConfirm={() => void mutate()} onCancel={() => setDialogOpen(false)}>
      {editing?.action === 'rename' && <label className={styles.renameLabel}>Название клипа<input className={styles.renameInput} value={newName} onChange={e => setNewName(e.target.value)} maxLength={180} onKeyDown={e => { if (e.key === 'Enter') { e.preventDefault(); void mutate(); } }} /></label>}
      {mutationError && <p role="alert" className={styles.mutationError}>{mutationError}</p>}
    </ConfirmDialog>
  </section>;
}
