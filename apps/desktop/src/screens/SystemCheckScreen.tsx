import React, { useCallback, useEffect, useRef, useState } from 'react';
import { ArrowRight, Check, FileText, RefreshCw, X } from 'lucide-react';
import type { AppError } from '../bridge/contracts';
import { normalizeError } from '../bridge/host';
import type { HostBridge } from '../bridge/contracts';
import type { SystemCheckResult } from '../config/model';
import { Button, ErrorNotice, InlineStatus } from '../components/Primitives';
import { SetupShell } from '../onboarding/SetupShell';
import styles from './SetupScreens.module.css';

export interface SystemCheckScreenProps {
  hostBridge: HostBridge;
  onContinue: () => void;
  onBack?: () => void;
}

const stageLabels: Record<string, string> = {
  windows_x64: 'Система Windows x64',
  available_memory: 'Доступная память',
  output_directory_and_space: 'Доступ к служебной папке',
  engine_presence: 'Компонент записи',
  native_hardware_and_runtime: 'Аппаратное кодирование и компоненты захвата',
  monitor_and_audio_catalogs: 'Экраны и аудиоустройства',
};

function stageMessage(stage: SystemCheckResult['stages'][number]): string | null {
  if (stage.passed) return null;
  const message = stage.message.trim();
  const known: Record<string, string> = {
    'Windows x64 is required': 'Требуется Windows 10 или 11 для компьютеров x64.',
    'memory.insufficient_available': 'Недостаточно доступной оперативной памяти для выбранного Replay.',
    'output.not_canonical_directory': 'Выбранная папка недоступна или имеет неподдерживаемый путь.',
    'output.insufficient_space': 'В выбранной папке недостаточно свободного места.',
    'native.hardware_or_runtime_unavailable': 'Аппаратное кодирование или компонент захвата недоступны.',
    'native.selected_audio_unavailable': 'Выбранный источник звука недоступен для записи.',
    'monitor.none_available': 'Не найден ни один доступный экран.',
    'monitor.selected_unavailable': 'Сохранённый экран сейчас недоступен.',
    'audio.selected_unavailable': 'Сохранённое аудиоустройство сейчас недоступно.',
  };
  if (known[message]) return known[message];
  if (/[А-Яа-яЁё]/.test(message)) return message;
  return 'Проверка не пройдена. Откройте технические подробности.';
}

export function SystemCheckScreen({
  hostBridge,
  onContinue,
  onBack,
}: SystemCheckScreenProps) {
  const requestGeneration = useRef(0);
  const checkInFlightRef = useRef(false);
  const checkPromiseRef = useRef<Promise<SystemCheckResult> | null>(null);
  const diagnosticsInFlightRef = useRef(false);
  const [isRunning, setIsRunning] = useState(true);
  const [isOpeningDiagnostics, setIsOpeningDiagnostics] = useState(false);
  const [result, setResult] = useState<SystemCheckResult | null>(null);
  const [error, setError] = useState<AppError | null>(null);
  const [diagnosticsError, setDiagnosticsError] = useState<AppError | null>(null);

  const runCheck = useCallback(async (reuseInFlight = false) => {
    if (checkInFlightRef.current && !reuseInFlight) return;
    const operation =
      reuseInFlight && checkPromiseRef.current
        ? checkPromiseRef.current
        : hostBridge.runSystemCheck();
    checkPromiseRef.current = operation;
    checkInFlightRef.current = true;
    const generation = ++requestGeneration.current;
    setIsRunning(true);
    setResult(null);
    setError(null);
    setDiagnosticsError(null);

    try {
      const nextResult = await operation;
      if (generation !== requestGeneration.current) return;
      setResult(nextResult);
    } catch (cause) {
      if (generation !== requestGeneration.current) return;
      setError(normalizeError(cause));
    } finally {
      if (generation === requestGeneration.current) {
        setIsRunning(false);
        checkInFlightRef.current = false;
        if (checkPromiseRef.current === operation) checkPromiseRef.current = null;
      }
    }
  }, [hostBridge]);

  useEffect(() => {
    void runCheck(true);
    return () => {
      requestGeneration.current += 1;
    };
  }, [runCheck]);

  async function openDiagnostics() {
    if (!result?.diagnostics_path || diagnosticsInFlightRef.current) return;
    diagnosticsInFlightRef.current = true;
    setIsOpeningDiagnostics(true);
    setDiagnosticsError(null);
    try {
      await hostBridge.openDiagnostics();
    } catch (cause) {
      setDiagnosticsError(normalizeError(cause));
    } finally {
      diagnosticsInFlightRef.current = false;
      setIsOpeningDiagnostics(false);
    }
  }

  const failedStage = result?.stages.find((stage) => !stage.passed) ?? null;
  const failedStageMessage = failedStage ? stageMessage(failedStage) : null;
  const succeeded = Boolean(
    result?.passed && result.stages.every((stage) => stage.passed) && !isRunning && !error
  );

  return (
    <SetupShell
      phase="check"
      onBack={onBack}
    >
      <section className={`${styles.page} ${styles.checkPage}`} data-testid="system-check-screen">
        <p className={styles.eyebrow}>Прежде чем начнём</p>
        <h1 className={styles.checkTitle}>{succeeded ? 'Можно начинать' : 'Проверим компьютер'}</h1>
        <p className={styles.screenCopy}>{succeeded ? 'Всё необходимое для записи доступно. Теперь настроим её под вас.' : 'Убедимся, что экран, звук и компоненты записи готовы к работе.'}</p>

        <div className={styles.checkBody}>
          {isRunning && (
            <>
              <div className={styles.progressLine} role="progressbar" aria-label="Идёт проверка" />
              <InlineStatus tone="busy">Проверяем совместимость…</InlineStatus>
            </>
          )}

          {!isRunning && succeeded && (
            <div className={styles.checkSuccess} role="status"><Check size={16} />Компьютер готов к записи</div>
          )}

          {!isRunning && (error || !succeeded) && (
            <ErrorNotice
              title="Не удалось подтвердить совместимость"
              technicalDetails={
                error
                  ? `Код: ${error.code}\n${error.technicalCause}`
                  : failedStage?.message
              }
              action={
                <Button
                  variant="secondary"
                  leadingIcon={<RefreshCw size={17} />}
                  onClick={() => void runCheck()}
                  data-testid="check-retry-button"
                >
                  Повторить проверку
                </Button>
              }
            >
              {error?.summary ?? failedStageMessage ?? 'Одна из обязательных проверок не пройдена.'}
            </ErrorNotice>
          )}

          {result && (
            <details className={styles.details}>
              <summary>Результаты проверки</summary>
            <ul className={styles.stageList} aria-label="Результаты проверки">
              {result.stages.map((stage) => {
                const message = stageMessage(stage);
                const stageLabel = stageLabels[stage.id] ?? 'Дополнительная проверка';
                const technicalLines = [
                  stageLabels[stage.id] ? null : `Идентификатор: ${stage.id}`,
                  !stage.passed && stage.message && message !== stage.message
                    ? stage.message
                    : null,
                ].filter((line): line is string => Boolean(line));
                return (
                  <li key={stage.id} className={styles.stageRow}>
                    {stage.passed ? (
                      <Check className={styles.stageIcon} size={18} aria-hidden="true" />
                    ) : (
                      <X
                        className={`${styles.stageIcon} ${styles.stageIconFailed}`}
                        size={18}
                        aria-hidden="true"
                      />
                    )}
                    <div className={styles.stageText}>
                      <span className={styles.stageName}>{stageLabel}</span>
                      {message && <span className={styles.stageMessage}>{message}</span>}
                      {technicalLines.length > 0 && (
                        <details className={styles.details}>
                          <summary>Технические подробности</summary>
                          <div className={styles.detailsContent}>
                            {technicalLines.join('\n')}
                          </div>
                        </details>
                      )}
                    </div>
                    <span
                      className={`${styles.stageResult} ${
                        stage.passed ? '' : styles.stageResultFailed
                      }`}
                    >
                      {stage.passed ? 'Готово' : 'Требует внимания'}
                    </span>
                  </li>
                );
              })}
            </ul>
            </details>
          )}

          {diagnosticsError && (
            <ErrorNotice
              title="Не удалось открыть отчёт"
              technicalDetails={`Код: ${diagnosticsError.code}\n${diagnosticsError.technicalCause}`}
            >
              {diagnosticsError.summary}
            </ErrorNotice>
          )}
        </div>

        <div className={styles.actions}>
          {succeeded && (
            <Button
              variant="primary"
              trailingIcon={<ArrowRight size={18} />}
              onClick={onContinue}
              data-testid="check-continue-button"
            >
              Выбрать экран
            </Button>
          )}
          {!isRunning && !succeeded && result?.diagnostics_path && (
            <Button
              variant="tertiary"
              busy={isOpeningDiagnostics}
              busyLabel="Открываем…"
              leadingIcon={<FileText size={17} />}
              onClick={() => void openDiagnostics()}
              data-testid="check-diagnostics-button"
            >
              Открыть отчёт
            </Button>
          )}
        </div>
      </section>
    </SetupShell>
  );
}
