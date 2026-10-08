import { FileSearch } from 'lucide-react';
import { normalizeError } from '../bridge/host';
import type { EngineSnapshot } from '../config/model';
import { Button, ErrorNotice, InlineStatus, type InlineStatusTone } from './Primitives';
import styles from './EngineState.module.css';

export interface EngineStateProps {
  snapshot: EngineSnapshot;
  isStale?: boolean;
  onOpenDiagnostics?: () => Promise<void> | void;
  onOpenSettings?: () => void;
  isOpeningDiagnostics?: boolean;
  diagnosticsDisabled?: boolean;
  diagnosticsExpanded?: boolean;
}
function presentLifecycle(snapshot: EngineSnapshot, stale: boolean): { title: string; tone: InlineStatusTone } {
  if (stale) return { title: 'Нет связи со службой записи', tone: 'warning' };
  switch (snapshot.lifecycle) {
    case 'starting': return { title: 'Включаем Replay…', tone: 'busy' };
    case 'recovering': return { title: 'Восстанавливаем запись…', tone: 'busy' };
    case 'ready': return { title: snapshot.replayActive ? 'Replay включён' : 'Replay выключен', tone: snapshot.replayActive ? 'success' : 'neutral' };
    case 'stopped': return { title: 'Replay выключен', tone: 'neutral' };
    case 'degraded': return { title: 'Запись работает с ошибками', tone: 'warning' };
    case 'blocked': return { title: 'Запуск заблокирован', tone: 'danger' };
    default: return { title: 'Запись остановлена из-за ошибки', tone: 'danger' };
  }
}
export function EngineState({ snapshot, isStale = false, onOpenDiagnostics, onOpenSettings,
  isOpeningDiagnostics = false, diagnosticsDisabled = false, diagnosticsExpanded = false }: EngineStateProps) {
  const presentation = presentLifecycle(snapshot, isStale);
  const error = snapshot.lastError ? normalizeError({ code: snapshot.lastError.code, message: snapshot.lastError.message }) : null;
  return (
    <section className={styles.container} aria-labelledby="engine-state-title" data-testid="engine-state">
      <header className={styles.header}>
        <div className={styles.headerCopy}>
          <h2 id="engine-state-title" className={styles.title} data-tone={presentation.tone}
            data-testid="engine-lifecycle-badge" aria-live="polite">{presentation.title}</h2>
          {snapshot.continuousRecordingActive && <InlineStatus tone={isStale ? 'warning' : 'danger'}>
            {isStale ? 'Состояние обычной записи неизвестно' : 'Обычная запись идёт'}
          </InlineStatus>}
        </div>
        {onOpenDiagnostics && <Button variant="tertiary" size="icon" busy={isOpeningDiagnostics}
          title="Диагностика" aria-label={diagnosticsExpanded ? 'Скрыть диагностику' : 'Диагностика'}
          disabled={diagnosticsDisabled} onClick={() => void onOpenDiagnostics()}
          aria-expanded={diagnosticsExpanded} aria-controls="recording-diagnostics" data-testid="engine-diagnostics-button">
          <FileSearch aria-hidden="true" />
        </Button>}
      </header>
      {error && <ErrorNotice title={error.summary}
        technicalDetails={`Код: ${error.code}\n${error.technicalCause}${snapshot.lastError?.hresult != null ? `\nHRESULT: ${snapshot.lastError.hresult}` : ''}`}
        action={onOpenSettings ? <Button variant="tertiary" size="compact" onClick={onOpenSettings}>Исправить настройки</Button> : undefined}
        data-testid="engine-error-banner" />}
    </section>
  );
}
