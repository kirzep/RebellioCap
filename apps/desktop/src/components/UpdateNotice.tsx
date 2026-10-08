import { t, useTranslation } from '../i18n';
import { useRef, useState } from 'react';
import { ArrowUpToLine } from 'lucide-react';
import type { AvailableUpdate, UpdateProgress } from '../bridge/contracts';
import { SidebarMenuButton, SidebarMenuItem } from './ui/sidebar';
import { ConfirmDialog, ErrorNotice } from './Primitives';
import { normalizeError } from '../bridge/host';
import { cn } from 'cn';
import styles from './WindowShell.module.css';
import Markdown from 'react-markdown';
import updateStyles from './UpdateNotice.module.css';

export interface UpdateNoticeProps {
  update: AvailableUpdate;
  onInstall: (onProgress: (progress: UpdateProgress) => void) => Promise<void>;
  disabled?: boolean;
}

export function UpdateNotice({ update, onInstall, disabled }: UpdateNoticeProps) {
  useTranslation();
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
        aria-label={t("Доступно обновление {0}", update.version)} title={t("Доступно обновление {0}", update.version)}
        disabled={disabled || busy} onClick={() => { setError(null); setOpen(true); }}>
        <span className={styles.updateIcon}><ArrowUpToLine aria-hidden="true" /><span className={styles.updateDot} /></span>
        <span className={cn(styles.navLabel, styles.updateLabel)}>
          <span>{t("Доступно обновление")}</span><span className={styles.updateVersion}>{t("Версия")}{' '}{update.version}</span>
        </span>
      </SidebarMenuButton>
    </SidebarMenuItem>
    <ConfirmDialog open={open} title={t("Обновить RebellioCap до версии {0}?", update.version)}
      description={t("Сначала скачаем и проверим обновление. Текущая запись остановится с сохранением файла, Replay будет выключен, а несохранённый повтор будет потерян. После установки обновления приложение перезапустится.")}
      confirmLabel={t("Обновить и перезапустить")} cancelLabel={t("Позже")} initialFocus="cancel"
      busy={busy} busyLabel={t("Обновляем…")} onConfirm={() => void install()} onCancel={() => setOpen(false)}>
      {update.releaseNotes && <div className={updateStyles.notes}><Markdown skipHtml
        components={{ a: ({ children }) => <span>{children}</span>, img: () => null }}>{update.releaseNotes}</Markdown></div>}
      {busy && <div className="mt-3 space-y-2">
        <p role="status">{progress?.phase === 'installing' ? t('Устанавливаем обновление…') : progress?.phase === 'preparing' ? t('Завершаем запись перед обновлением…') : t('Скачиваем обновление…')}</p>
        {(!progress || progress.phase === 'downloading') && <>
          <progress aria-label={t("Загрузка обновления")} className={updateStyles.progress} max={progress?.totalBytes || undefined}
            value={progress?.totalBytes ? progress.downloadedBytes : undefined} />
          {progress && <p className="text-sm text-muted-foreground">
            {(progress.downloadedBytes / 1048576).toFixed(1)} {' '}{t("МБ")}{progress.totalBytes ? t(" из {0} МБ", (progress.totalBytes / 1048576).toFixed(1)) : ''}
          </p>}
        </>}
      </div>}
      {error && <ErrorNotice title={t("Не удалось обновить приложение")}>{t(error)}</ErrorNotice>}
    </ConfirmDialog>
  </>;
}
