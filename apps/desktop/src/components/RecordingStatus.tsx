import { t, useTranslation } from '../i18n';
import type { EngineSnapshot } from '../config/model';
import styles from './RecordingStatus.module.css';

export function RecordingStatus({ snapshot, unavailable = false }: {
  snapshot: EngineSnapshot | null;
  unavailable?: boolean;
}) {
  useTranslation();
  const known = Boolean(snapshot) && !unavailable;
  const missingAudio = known ? snapshot?.metrics.audioUnavailableSources ?? 0 : 0;
  return <div className={styles.status} role="status" aria-label={t("Состояние записи")}>
    <span className={styles.item} title={t("Мгновенный повтор хранится в буфере и сохраняется только по вашей команде.")}>
      <i className={styles.dot} data-state={known && snapshot?.replayActive ? 'replay' : 'idle'} aria-hidden="true" />
      {t("Повтор:")}{known ? snapshot?.replayActive ? t('включён') : t('выключен') : t('нет данных')}
    </span>
    <span className={styles.item}>
      <i className={styles.dot} data-state={known && snapshot?.continuousRecordingActive ? 'recording' : 'idle'} aria-hidden="true" />
      {t("Запись:")}{known ? snapshot?.continuousRecordingActive ? t('идёт') : t('не идёт') : t('нет данных')}
    </span>
    {known && snapshot?.replayActive && snapshot.metrics.replayMemoryLimited && (
      <span className={`${styles.item} ${styles.memory}`}>
        {t("Replay: доступно {0} с. Буфер сокращён: лимит памяти.", Math.max(0, Math.floor(snapshot.metrics.replayRetainedSeconds ?? 0)))}
      </span>
    )}
    {missingAudio !== 0 && <span className={`${styles.item} ${styles.memory}`}>
      {missingAudio === 3 ? t('Звук системы и микрофон недоступны') : missingAudio & 1 ? t('Звук системы недоступен') : t('Микрофон недоступен')}.
      {t(' Запись продолжается с тишиной; подключение выбранного устройства будет восстановлено автоматически.')}
    </span>}
  </div>;
}
