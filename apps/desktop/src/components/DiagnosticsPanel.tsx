import type { AppError } from '../bridge/contracts';
import { normalizeError } from '../bridge/host';
import type { EngineSnapshot, SystemCheckResult } from '../config/model';
import { Button, ErrorNotice, InlineStatus } from './Primitives';
import styles from './DiagnosticsPanel.module.css';

export interface DiagnosticsPanelProps {
  snapshot: EngineSnapshot;
  result: SystemCheckResult | null;
  checkError: AppError | null;
  openError: AppError | null;
  isChecking: boolean;
  isOpeningReport: boolean;
  commandsDisabled?: boolean;
  onRunCheck: () => Promise<void> | void;
  onOpenReport: () => Promise<void> | void;
}

const stageLabels: Record<string, string> = {
  windows_x64: 'Система Windows x64',
  available_memory: 'Доступная память',
  output_directory_and_space: 'Доступ к служебной папке',
  engine_presence: 'Компонент записи',
  native_hardware_and_runtime: 'Аппаратное кодирование и компоненты захвата',
  monitor_and_audio_catalogs: 'Экраны и аудиоустройства',
};

const stageMessages: Record<string, string> = {
  'Windows x64 is required': 'Требуется Windows 10 или 11 для компьютеров x64.',
  'memory.insufficient_available':
    'Недостаточно доступной оперативной памяти для выбранного Replay.',
  'output.not_canonical_directory':
    'Служебная папка недоступна или имеет неподдерживаемый путь.',
  'output.insufficient_space': 'В служебной папке недостаточно свободного места.',
  'native.hardware_or_runtime_unavailable':
    'Аппаратное кодирование или компонент захвата недоступны.',
  'native.selected_audio_unavailable':
    'Выбранный источник звука недоступен для записи.',
  'monitor.none_available': 'Не найден ни один доступный экран.',
  'monitor.selected_unavailable': 'Сохранённый экран сейчас недоступен.',
  'audio.selected_unavailable': 'Сохранённое аудиоустройство сейчас недоступно.',
};

function stageName(id: string): string {
  return stageLabels[id] ?? 'Дополнительная проверка';
}

function lifecycleLabel(lifecycle: EngineSnapshot['lifecycle']): string {
  const labels: Record<EngineSnapshot['lifecycle'], string> = {
    starting: 'Replay включается',
    ready: 'служба готова',
    stopped: 'служба остановлена',
    recovering: 'идёт восстановление',
    degraded: 'работа с ошибками',
    blocked: 'запуск заблокирован',
    failed: 'остановка из-за ошибки',
  };
  return labels[lifecycle];
}

function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 Б';
  const units = ['Б', 'КиБ', 'МиБ', 'ГиБ'];
  let value = bytes;
  let unitIndex = 0;
  while (value >= 1024 && unitIndex < units.length - 1) {
    value /= 1024;
    unitIndex += 1;
  }
  const precision = value >= 10 || unitIndex === 0 ? 0 : 1;
  return `${value.toLocaleString('ru-RU', {
    maximumFractionDigits: precision,
  })} ${units[unitIndex]}`;
}

function stageDescription(stage: SystemCheckResult['stages'][number]): string {
  if (stage.passed) return 'Проверка пройдена.';
  const message = stage.message.trim();
  if (stageMessages[message]) return stageMessages[message];
  if (/[А-Яа-яЁё]/.test(message)) return message;
  return 'Проверка не пройдена. Код результата доступен в технических подробностях.';
}

function checkTechnicalDetails(result: SystemCheckResult): string {
  return result.stages
    .map((stage) => {
      const message = stage.message.trim() || 'без сообщения';
      return `${stage.id}: ${stage.passed ? 'passed' : 'failed'}\n${message}`;
    })
    .join('\n\n');
}

function engineTechnicalDetails(snapshot: EngineSnapshot): string | undefined {
  if (!snapshot.lastError) return undefined;
  const lines = [`Код: ${snapshot.lastError.code}`];
  if (snapshot.lastError.hresult !== null) {
    lines.push(`HRESULT: ${snapshot.lastError.hresult}`);
  }
  if (snapshot.lastError.message) lines.push(snapshot.lastError.message);
  return lines.join('\n');
}

export function DiagnosticsPanel({
  snapshot,
  result,
  checkError,
  openError,
  isChecking,
  isOpeningReport,
  commandsDisabled = false,
  onRunCheck,
  onOpenReport,
}: DiagnosticsPanelProps) {
  const lastError = snapshot.lastError
    ? normalizeError({
        code: snapshot.lastError.code,
        message: snapshot.lastError.message,
      })
    : null;

  return (
    <section
      id="recording-diagnostics"
      className={styles.panel}
      aria-labelledby="recording-diagnostics-title"
      data-testid="recording-diagnostics"
    >
      <header className={styles.header}>
        <div>
          <p className={styles.eyebrow}>Служебные сведения</p>
          <h2 id="recording-diagnostics-title" className={styles.title}>
            Диагностика
          </h2>
        </div>
        <p className={styles.intro}>
          Проверка не меняет настройки и не запускает мастер заново.
        </p>
      </header>

      <div className={styles.groups}>
        <section className={styles.group} aria-labelledby="diagnostics-engine-title">
          <h3 id="diagnostics-engine-title" className={styles.groupTitle}>
            Состояние службы
          </h3>
          <dl className={styles.summaryList}>
            <div>
              <dt>Режим</dt>
              <dd>{lifecycleLabel(snapshot.lifecycle)}</dd>
            </div>
            <div>
              <dt>Replay</dt>
              <dd>{snapshot.replayActive ? 'включён' : 'выключен'}</dd>
            </div>
            <div>
              <dt>Обычная запись</dt>
              <dd>{snapshot.continuousRecordingActive ? 'идёт' : 'не выполняется'}</dd>
            </div>
          </dl>

          <details className={styles.technicalDetails} data-testid="engine-metrics">
            <summary>Счётчики текущей сессии</summary>
            <p className={styles.counterNote}>
              Это не текущий FPS и не процент потери кадров.
            </p>
            <dl className={styles.counterList}>
              <div>
                <dt>Видеопакеты</dt>
                <dd data-testid="metric-video">{snapshot.metrics.videoPackets}</dd>
              </div>
              <div>
                <dt>Аудиопакеты</dt>
                <dd data-testid="metric-audio">{snapshot.metrics.audioPackets}</dd>
              </div>
              <div>
                <dt>PCM-фреймы, отброшенные при смешивании</dt>
                <dd data-testid="metric-audio-mixing-drops">{snapshot.metrics.audioMixingDroppedFrames ?? 0}</dd>
              </div>
              <div>
                <dt>Пропущенные сроки видеокадра</dt>
                <dd data-testid="metric-drops">{snapshot.metrics.missedVideoDeadlines}</dd>
              </div>
              <div>
                <dt>Данные Replay</dt>
                <dd>{formatBytes(snapshot.metrics.replayBytes)}</dd>
              </div>
              <div>
                <dt>Успешные сохранения</dt>
                <dd data-testid="metric-saves">{snapshot.metrics.completedSaves}</dd>
              </div>
              <div>
                <dt>Неудачные сохранения</dt>
                <dd>{snapshot.metrics.failedSaves}</dd>
              </div>
              <div>
                <dt>Отклонённые сохранения</dt>
                <dd>{snapshot.metrics.rejectedSaves}</dd>
              </div>
              <div>
                <dt>Ошибки конвейера записи</dt>
                <dd>{snapshot.metrics.pipelineErrors}</dd>
              </div>
              <div>
                <dt>Пакеты обычной записи</dt>
                <dd>{snapshot.metrics.continuousPackets}</dd>
              </div>
              <div>
                <dt>Сбои обычной записи</dt>
                <dd>{snapshot.metrics.continuousFailures}</dd>
              </div>
            </dl>
          </details>

          {lastError ? (
            <ErrorNotice
              title="Последняя ошибка службы"
              tone="warning"
              technicalDetails={engineTechnicalDetails(snapshot)}
            >
              {lastError.summary}
            </ErrorNotice>
          ) : (
            <InlineStatus tone="neutral">Служба не сообщала ошибку в текущем состоянии.</InlineStatus>
          )}
        </section>

        <section className={styles.group} aria-labelledby="diagnostics-check-title">
          <div className={styles.groupHeading}>
            <div>
              <h3 id="diagnostics-check-title" className={styles.groupTitle}>
                Проверка компьютера
              </h3>
              <p className={styles.groupCopy}>
                Проверяет доступность компонентов захвата и устройств.
              </p>
            </div>
            <Button
              variant="secondary"
              size="compact"
              busy={isChecking}
              busyLabel="Проверяем…"
              disabled={commandsDisabled || isOpeningReport}
              onClick={() => void onRunCheck()}
            >
              {result || checkError ? 'Повторить проверку' : 'Проверить систему'}
            </Button>
          </div>

          {isChecking && !result && (
            <InlineStatus tone="busy">Проверяем совместимость и доступность устройств…</InlineStatus>
          )}

          {checkError && (
            <ErrorNotice
              title="Не удалось проверить компьютер"
              technicalDetails={`Код: ${checkError.code}\n${checkError.technicalCause}`}
            >
              {checkError.summary}
            </ErrorNotice>
          )}

          {result && (
            <>
              <InlineStatus tone={result.passed ? 'success' : 'warning'}>
                {result.passed
                  ? 'Все обязательные проверки пройдены.'
                  : 'Есть проблема, которую нужно исправить перед надёжной записью.'}
              </InlineStatus>
              <ul className={styles.stageList}>
                {result.stages.map((stage) => (
                  <li key={stage.id} className={styles.stage}>
                    <span
                      className={`${styles.stageMarker} ${
                        stage.passed ? styles.stagePassed : styles.stageFailed
                      }`}
                      aria-hidden="true"
                    />
                    <span>
                      <strong>{stageName(stage.id)}</strong>
                      <small>{stageDescription(stage)}</small>
                    </span>
                  </li>
                ))}
              </ul>
              <details className={styles.technicalDetails}>
                <summary>Технические результаты проверки</summary>
                <pre>{checkTechnicalDetails(result)}</pre>
              </details>
            </>
          )}

          {!isChecking && !result && !checkError && (
            <p className={styles.emptyCopy}>Проверка в этом сеансе ещё не выполнялась.</p>
          )}
        </section>

        <section className={styles.group} aria-labelledby="diagnostics-report-title">
          <div className={styles.groupHeading}>
            <div>
              <h3 id="diagnostics-report-title" className={styles.groupTitle}>
                Технический отчёт
              </h3>
              <p className={styles.groupCopy}>
                Отчёт создаётся службой записи после проверки компьютера.
              </p>
            </div>
            {result?.diagnostics_path && (
              <Button
                variant="tertiary"
                size="compact"
                busy={isOpeningReport}
                busyLabel="Открываем…"
                disabled={commandsDisabled || isChecking}
                onClick={() => void onOpenReport()}
              >
                Открыть отчёт
              </Button>
            )}
          </div>

          {result?.diagnostics_path ? (
            <code className={styles.reportPath}>{result.diagnostics_path}</code>
          ) : (
            <p className={styles.emptyCopy}>Сначала выполните проверку системы.</p>
          )}

          {openError && (
            <ErrorNotice
              title="Не удалось открыть отчёт"
              technicalDetails={`Код: ${openError.code}\n${openError.technicalCause}`}
            >
              {openError.summary}
            </ErrorNotice>
          )}
        </section>
      </div>
    </section>
  );
}
