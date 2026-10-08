import type { EngineSnapshot } from '../config/model';
import styles from './RecordingStatus.module.css';

export function RecordingStatus({ snapshot, unavailable = false }: {
  snapshot: EngineSnapshot | null;
  unavailable?: boolean;
}) {
  const known = Boolean(snapshot) && !unavailable;
  const missingAudio = known ? snapshot?.metrics.audioUnavailableSources ?? 0 : 0;
  return <div className={styles.status} role="status" aria-label="Состояние записи">
    <span className={styles.item} title="Мгновенный повтор хранится в буфере и сохраняется только по вашей команде.">
      <i className={styles.dot} data-state={known && snapshot?.replayActive ? 'replay' : 'idle'} aria-hidden="true" />
      Повтор: {known ? snapshot?.replayActive ? 'включён' : 'выключен' : 'нет данных'}
    </span>
    <span className={styles.item}>
      <i className={styles.dot} data-state={known && snapshot?.continuousRecordingActive ? 'recording' : 'idle'} aria-hidden="true" />
      Запись: {known ? snapshot?.continuousRecordingActive ? 'идёт' : 'не идёт' : 'нет данных'}
    </span>
    {known && snapshot?.replayActive && snapshot.metrics.replayMemoryLimited && (
      <span className={`${styles.item} ${styles.memory}`}>
        {`Replay: доступно ${Math.max(0, Math.floor(snapshot.metrics.replayRetainedSeconds ?? 0))} с. Буфер сокращён: лимит памяти.`}
      </span>
    )}
    {missingAudio !== 0 && <span className={`${styles.item} ${styles.memory}`}>
      {missingAudio === 3 ? 'Звук системы и микрофон недоступны' : missingAudio & 1 ? 'Звук системы недоступен' : 'Микрофон недоступен'}.
      {' Запись продолжается с тишиной; подключение выбранного устройства будет восстановлено автоматически.'}
    </span>}
  </div>;
}
