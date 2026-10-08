import { t, useTranslation } from '../../i18n';
import React, { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { Check, Film } from 'lucide-react';
import { RecordingOrb } from '../../components/RecordingOrb';
import type { AppError, HostBridge } from '../../bridge/contracts';
import { normalizeError } from '../../bridge/host';
import { Button, ErrorNotice, InlineStatus } from '../../components/Primitives';
import type { OnboardingDraft, RecordingTestSummary } from '../../config/model';
import styles from './steps.module.css';

export interface TestStepProps {
  draft: OnboardingDraft;
  hostBridge: HostBridge;
  onBack?: () => void;
  onComplete: () => void | Promise<void>;
  onBusyChange?: (busy: boolean) => void;
  footerTarget?: HTMLElement | null;
}

type TestPhase = 'idle' | 'running' | 'success' | 'failure' | 'completing' | 'complete';

function technicalDetails(error: AppError): string {
  return t("Код: {0}\n{1}", error.code, error.technicalCause);
}

function invalidEvidenceError(summary: RecordingTestSummary): AppError {
  return normalizeError({
    code: 'recording_test.invalid_evidence',
    summary: summary.succeeded
      ? 'Служба записи вернула неполное подтверждение тестового клипа.'
      : 'Тестовая запись не завершилась успешно.',
    subsystem: 'engine',
    technicalCause: [
      `succeeded=${summary.succeeded}`,
      `fingerprint=${summary.fingerprint.trim() ? 'present' : 'missing'}`,
      `clip_path=${summary.clip_path.trim() ? 'present' : 'missing'}`,
      `video_packets=${summary.video_packets}`,
      `audio_packets=${summary.audio_packets}`,
    ].join('; '),
    retryable: true,
    actions: ['retry', 'diagnostics'],
  });
}

function hasValidEvidence(
  summary: RecordingTestSummary,
  draft: OnboardingDraft
): boolean {
  const audioEnabled =
    typeof draft.system_audio === 'object' ||
    typeof draft.microphone === 'object';
  return Boolean(
    summary.succeeded &&
    /^[a-f\d]{64}$/i.test(summary.fingerprint.trim()) &&
    summary.clip_path.trim() &&
    Number.isInteger(summary.video_packets) &&
    summary.video_packets > 0 &&
    Number.isInteger(summary.audio_packets) &&
    summary.audio_packets >= 0 &&
    (!audioEnabled || summary.audio_packets > 0)
  );
}

export function TestStep({
  draft,
  hostBridge,
  onBack,
  onComplete,
  onBusyChange,
  footerTarget,
}: TestStepProps) {
  useTranslation();
  const [phase, setPhase] = useState<TestPhase>('idle');
  const [summary, setSummary] = useState<RecordingTestSummary | null>(null);
  const [testError, setTestError] = useState<AppError | null>(null);
  const [openError, setOpenError] = useState<AppError | null>(null);
  const [diagnosticsError, setDiagnosticsError] = useState<AppError | null>(null);
  const [completionError, setCompletionError] = useState<AppError | null>(null);
  const testRunningRef = useRef(false);
  const completionStartedRef = useRef(false);

  const busy = phase === 'running' || phase === 'completing';
  const recordingWithoutAudio = draft.system_audio === 'disabled' && draft.microphone === 'disabled';

  useEffect(() => {
    onBusyChange?.(busy);
    return () => onBusyChange?.(false);
    // The parent commonly passes an inline callback; only phase changes should retrigger this.
  }, [busy]);

  async function handleRunTest() {
    if (busy || testRunningRef.current) return;
    testRunningRef.current = true;
    setPhase('running');
    setSummary(null);
    setTestError(null);
    setOpenError(null);
    setDiagnosticsError(null);
    setCompletionError(null);

    try {
      const result = await hostBridge.runRecordingTest();
      if (!hasValidEvidence(result, draft)) {
        testRunningRef.current = false;
        setPhase('failure');
        setTestError(invalidEvidenceError(result));
        return;
      }

      setSummary(result);
      testRunningRef.current = false;
      setPhase('success');
    } catch (error) {
      testRunningRef.current = false;
      setPhase('failure');
      setTestError(normalizeError(error));
    }
  }

  async function handleOpenClip() {
    setOpenError(null);
    try {
      await hostBridge.openTestClip();
    } catch (error) {
      setOpenError(normalizeError(error));
    }
  }

  async function handleOpenDiagnostics() {
    setDiagnosticsError(null);
    try {
      const result = await hostBridge.runSystemCheck();
      if (!result.diagnostics_path) {
        throw {
          code: 'diagnostics.report_missing',
          summary: 'Диагностический отчёт не был создан.',
          subsystem: 'system',
          technicalCause: 'runSystemCheck returned an empty diagnostics_path',
          retryable: true,
          actions: ['diagnostics'],
        } satisfies AppError;
      }
      await hostBridge.openDiagnostics();
    } catch (error) {
      setDiagnosticsError(normalizeError(error));
    }
  }

  async function handleComplete() {
    if (phase !== 'success' || completionStartedRef.current) return;
    completionStartedRef.current = true;
    setPhase('completing');
    setCompletionError(null);
    try {
      await onComplete();
      setPhase('complete');
    } catch (error) {
      completionStartedRef.current = false;
      setPhase('success');
      setCompletionError(normalizeError(error));
    }
  }

  return (
    <div className={styles.stepContent} data-testid="test-step" aria-busy={busy}>
      <div className={styles.stepIntro}>
        <h2 className={styles.title}>{t("Проверим запись")}</h2>
        <p className={styles.copy}>
          {t("Создадим короткий клип, чтобы проверить изображение и звук.")}</p>
      </div>

      {phase === 'idle' && (
        <div className={styles.testPrompt}>
          <Film size={28} strokeWidth={1.3} aria-hidden="true" />
          <strong>{t("Всего 5 секунд")}</strong>
          <p>{t("Запишем выбранный экран и включённые источники звука. Клип появится в вашей папке.")}</p>
          <span className={styles.testPath}>{draft.output_directory || t('Папка клипов')}</span>
        </div>
      )}

      {phase === 'running' && (
        <div className={styles.testProgress} role="status">
          <RecordingOrb className={styles.recordingOrb} />
          <div>
            <strong>{t("Создаём тестовый клип…")}</strong>
            <p>{t("Захват длится 5 секунд; подготовка и сохранение могут занять больше времени.")}</p>
          </div>
        </div>
      )}

      {(phase === 'success' || phase === 'completing') && summary && (
        <div className={styles.successPanel} data-testid="test-success-summary" role="status">
          <Check size={26} strokeWidth={1.5} aria-hidden="true" />
          <strong>{t("Тестовый клип готов")}</strong>
          <p>
            {recordingWithoutAudio
              ? t('Тест выполнен без звука — оба источника отключены в настройках.')
              : t('Изображение и звук записаны. Можно открыть клип и проверить результат.')}
          </p>
          <details className={styles.technicalDetails}>
            <summary>{t("Технические подробности")}</summary>
            <span>{t("Видео-пакетов:")}{' '}{summary.video_packets}</span>
            <span>{t("Аудио-пакетов:")}{' '}{summary.audio_packets}</span>
          </details>
          {phase === 'success' && <Button variant="tertiary" onClick={() => void handleOpenClip()}
            data-testid="open-test-clip-button">{t("Открыть клип")}</Button>}
        </div>
      )}

      {phase === 'complete' && (
        <InlineStatus tone="success">{t("Настройка завершена. Replay включён.")}</InlineStatus>
      )}

      {phase === 'failure' && testError && (
        <ErrorNotice
          title={t("Тестовая запись не готова")}
          technicalDetails={technicalDetails(testError)}
          action={
            <Button
              variant="tertiary"
              onClick={() => void handleOpenDiagnostics()}
              data-testid="test-diagnostics-button"
            >
              {t("Открыть диагностику")}</Button>
          }
          data-testid="test-error-message"
        >
          <p>{t(testError.summary)}</p>
          <p>
            {t("Проверьте выбранный экран, аудиоустройства, папку клипов и параметры видео,\n            затем повторите тест.")}</p>
        </ErrorNotice>
      )}

      {openError && (
        <ErrorNotice
          title={t("Клип сохранён, но не открылся")}
          tone="warning"
          technicalDetails={technicalDetails(openError)}
          action={
            <Button variant="tertiary" onClick={() => void handleOpenClip()}>
              {t("Попробовать снова")}</Button>
          }
        >
          {t(openError.summary)}
        </ErrorNotice>
      )}
      {diagnosticsError && (
        <ErrorNotice
          title={t("Не удалось открыть диагностику")}
          tone="warning"
          technicalDetails={technicalDetails(diagnosticsError)}
          action={
            <Button variant="tertiary" onClick={() => void handleOpenDiagnostics()}>
              {t("Попробовать снова")}</Button>
          }
        >
          {t(diagnosticsError.summary)}
        </ErrorNotice>
      )}
      {completionError && (
        <ErrorNotice
          title={t("Не удалось включить Replay")}
          technicalDetails={technicalDetails(completionError)}
        >
          {t(completionError.summary)}
        </ErrorNotice>
      )}

      {((actions) => footerTarget ? createPortal(actions, footerTarget) : actions)(<div className={styles.testActions}>
        <Button
          variant="tertiary"
          onClick={() => onBack?.()}
          disabled={busy || phase === 'complete' || !onBack}
          data-testid="test-back-button"
        >
          {t("Назад")}</Button>

        <div className={styles.actionCluster}>
          {phase !== 'complete' && (
            <Button
              variant={phase === 'success' || phase === 'completing' ? 'tertiary' : 'primary'}
              onClick={() => void handleRunTest()}
              busy={phase === 'running'}
              busyLabel={t("Создаём клип…")}
              disabled={phase === 'completing'}
              data-testid="run-test-button"
            >
              {phase === 'success' || phase === 'failure' || phase === 'completing'
                ? t('Записать снова')
                : t('Записать 5 секунд')}
            </Button>
          )}

          {(phase === 'success' || phase === 'completing') && (
            <Button
              variant="primary"
              onClick={() => void handleComplete()}
              busy={phase === 'completing'}
              busyLabel={t("Включаем Replay…")}
              disabled={phase !== 'success'}
              data-testid="complete-onboarding-button"
            >
              {t("Включить Replay")}</Button>
          )}
        </div>
      </div>)}
    </div>
  );
}
