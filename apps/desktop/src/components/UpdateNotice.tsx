import { useRef, useState } from 'react';
import { ArrowUpToLine } from 'lucide-react';
import type { AvailableUpdate, UpdateProgress } from '../bridge/contracts';
import { SidebarMenuButton, SidebarMenuItem } from './ui/sidebar';
import { ConfirmDialog, ErrorNotice } from './Primitives';
import { normalizeError } from '../bridge/host';
import { cn } from 'cn';
import styles from './WindowShell.module.css';

export interface UpdateNoticeProps {
  update: AvailableUpdate;
  onInstall: (onProgress: (progress: UpdateProgress) => void) => Promise<void>;
  disabled?: boolean;
}

export function UpdateNotice({ update, onInstall, disabled }: UpdateNoticeProps) {
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [progress, setProgress] = useState<UpdateProgress | null>(null);
  const pending = useRef(false);
  async function install() {
    if (pending.current) return;
    pending.current = true;
    setBusy(true);
    setError(null);
    setProgress(null);
    try {
      await onInstall(setProgress);
      setOpen(false);
    } catch (cause) {
      setError(normalizeError(cause).summary);
    } finally {
      pending.current = false;
      setBusy(false);
    }
  }
  return <>
    <SidebarMenuItem>
      <SidebarMenuButton type="button" size="lg" className={cn(styles.navButton, styles.updateButton)}
        aria-label={`Доступно обновление ${update.version}`} title={`Доступно обновление ${update.version}`}
        disabled={disabled || busy} onClick={() => { setError(null); setOpen(true); }}>
        <span className={styles.updateIcon}><ArrowUpToLine aria-hidden="true" /><span className={styles.updateDot} /></span>
        <span className={cn(styles.navLabel, styles.updateLabel)}>
          <span>Доступно обновление</span><span className={styles.updateVersion}>Версия {update.version}</span>
        </span>
      </SidebarMenuButton>
    </SidebarMenuItem>
    <ConfirmDialog open={open} title={`Обновить RebellioCap до версии ${update.version}?`}
      description="Сначала скачаем и проверим обновление. Текущая запись остановится с сохранением файла, Replay будет выключен, а несохранённый повтор будет потерян. После установки обновления приложение перезапустится."
      confirmLabel="Обновить и перезапустить" cancelLabel="Позже" initialFocus="cancel"
      busy={busy} busyLabel="Обновляем…" onConfirm={() => void install()} onCancel={() => setOpen(false)}>
      {update.releaseNotes && <div className="whitespace-pre-wrap text-sm text-muted-foreground">{update.releaseNotes}</div>}
      {busy && <div className="mt-3 space-y-2">
        <p role="status">{progress?.phase === 'installing' ? 'Устанавливаем обновление…' : progress?.phase === 'preparing' ? 'Завершаем запись перед обновлением…' : 'Скачиваем обновление…'}</p>
        {(!progress || progress.phase === 'downloading') && <>
          <progress aria-label="Загрузка обновления" className="w-full" max={progress?.totalBytes || undefined}
            value={progress?.totalBytes ? progress.downloadedBytes : undefined} />
          {progress && <p className="text-sm text-muted-foreground">
            {(progress.downloadedBytes / 1048576).toFixed(1)} МБ{progress.totalBytes ? ` из ${(progress.totalBytes / 1048576).toFixed(1)} МБ` : ''}
          </p>}
        </>}
      </div>}
      {error && <ErrorNotice title="Не удалось обновить приложение">{error}</ErrorNotice>}
    </ConfirmDialog>
  </>;
}
