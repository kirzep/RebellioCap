import { t, useTranslation } from '../i18n';
import { Card, CardHeader, CardTitle, CardFooter } from './ui/card';
import { Bookmark, Disc, Power, PowerOff } from 'lucide-react';
import type { AppError } from '../bridge/contracts';
import type { ActiveConfig, EngineSnapshot } from '../config/model';
import { formatHotkey, formatReplaySaveLabel } from '../config/format';
import { Button, ErrorNotice, InlineStatus } from './Primitives';
import styles from './RecordingControls.module.css';

export interface RecordingControlsProps {
  snapshot: EngineSnapshot;
  activeConfig: ActiveConfig;
  disabled?: boolean;
  isSavingReplay?: boolean;
  isTogglingContinuous?: boolean;
  isStartingReplay?: boolean;
  isStoppingReplay?: boolean;
  saveReplayError?: AppError | string | null;
  saveReplaySuccess?: string | null;
  toggleContinuousError?: AppError | string | null;
  toggleContinuousSuccess?: string | null;
  startReplayError?: AppError | string | null;
  startReplaySuccess?: string | null;
  stopReplayError?: AppError | string | null;
  stopReplaySuccess?: string | null;
  onSaveReplay: () => Promise<void> | void;
  onToggleContinuous: () => Promise<void> | void;
  onStartReplay: () => Promise<void> | void;
  onStopReplay: () => Promise<void> | void;
  onOpenSettings?: (section?: 'video' | 'audio' | 'replay' | 'hotkeys') => void;
}

function settingsSectionForError(
  error: AppError
): 'video' | 'audio' | 'replay' | 'hotkeys' {
  const hint = `${error.code} ${error.technicalCause}`.toLowerCase();
  if (error.actions.includes('choose-folder') || error.subsystem === 'filesystem') {
    return 'replay';
  }
  if (/hotkey/.test(hint)) return 'hotkeys';
  if (error.subsystem === 'audio') return 'audio';
  return 'video';
}

function ActionFeedback({
  error,
  errorTitle,
  success,
  onOpenSettings,
}: {
  error?: AppError | string | null;
  errorTitle: string;
  success?: string | null;
  onOpenSettings?: (section?: 'video' | 'audio' | 'replay' | 'hotkeys') => void;
}) {
  useTranslation();
  if (error) {
    const structuredError = typeof error === 'string' ? null : error;
    const canOpenSettings = Boolean(
      structuredError &&
      onOpenSettings &&
      (structuredError.actions.includes('settings') ||
        structuredError.actions.includes('choose-folder'))
    );
    const settingsAction =
      canOpenSettings && structuredError && onOpenSettings ? (
        <Button
          variant="tertiary"
          size="compact"
          onClick={() => onOpenSettings(settingsSectionForError(structuredError))}
        >
          {structuredError.actions.includes('choose-folder')
            ? t('Выбрать папку')
            : t('Изменить параметры')}
        </Button>
      ) : undefined;
    return (
      <ErrorNotice
        title={errorTitle}
        className={styles.actionError}
        action={settingsAction}
        technicalDetails={
          structuredError
            ? t("Код: {0}\n{1}", structuredError.code, structuredError.technicalCause)
            : undefined
        }
      >
        {typeof error === 'string' ? error : error.summary}
      </ErrorNotice>
    );
  }

  return success ? <InlineStatus tone="success">{success}</InlineStatus> : null;
}

export function RecordingControls({
  snapshot,
  activeConfig,
  disabled = false,
  isSavingReplay = false,
  isTogglingContinuous = false,
  isStartingReplay = false,
  isStoppingReplay = false,
  saveReplayError,
  saveReplaySuccess,
  toggleContinuousError,
  toggleContinuousSuccess,
  startReplayError,
  startReplaySuccess,
  stopReplayError,
  stopReplaySuccess,
  onSaveReplay,
  onToggleContinuous,
  onStartReplay,
  onStopReplay,
  onOpenSettings,
}: RecordingControlsProps) {
  useTranslation();
  const replaySaveLabel = snapshot.replayActive && snapshot.replaySeconds > 0
    ? formatReplaySaveLabel(snapshot.replaySeconds)
    : t('Сохранить повтор');
  const saveHotkey = formatHotkey(activeConfig.save_replay_hotkey);
  const recordingHotkey = formatHotkey(activeConfig.toggle_recording_hotkey);
  const lifecycleTransitioning =
    snapshot.lifecycle === 'starting' || snapshot.lifecycle === 'recovering';
  const anyCommandPending =
    isSavingReplay || isTogglingContinuous || isStartingReplay || isStoppingReplay;
  const replayReady = snapshot.lifecycle === 'ready' && snapshot.replayActive;
  const continuousRecordingCanStart =
    activeConfig.continuous_recording_enabled && !lifecycleTransitioning;
  const continuousRecordingCanStop =
    snapshot.continuousRecordingActive && !lifecycleTransitioning;
  const canStartReplay =
    !snapshot.replayActive &&
    ['ready', 'stopped', 'blocked', 'failed'].includes(snapshot.lifecycle);
  const canStopReplay = snapshot.replayActive && !lifecycleTransitioning;
  const saveBlockedByError =
    typeof saveReplayError === 'object' &&
    saveReplayError !== null &&
    !saveReplayError.retryable;
  const continuousStartBlockedByError =
    !snapshot.continuousRecordingActive &&
    typeof toggleContinuousError === 'object' &&
    toggleContinuousError !== null &&
    !toggleContinuousError.retryable;
  const startBlockedByError =
    typeof startReplayError === 'object' &&
    startReplayError !== null &&
    !startReplayError.retryable;

  return (
    <section className={styles.controls} aria-label={t("Управление записью")} data-testid="recording-controls">
      {snapshot.metrics.continuousRecoveryPath && (
        <ErrorNotice title={t("Сохранена аварийная копия записи")}>
          {t("Запись прервалась. Файл сохранён для восстановления; последний фрагмент может быть неполным.")}<span style={{ display: 'block', overflowWrap: 'anywhere' }}>{snapshot.metrics.continuousRecoveryPath}</span>
        </ErrorNotice>
      )}
      <div className={styles.actionGrid}>
        <Card className={styles.replayAction} role="region" aria-labelledby="replay-action-title">
          <CardHeader className="flex flex-wrap items-start justify-between gap-3">
            <div>
              <CardTitle id="replay-action-title" role="heading" aria-level={2}>
                {t("Мгновенный повтор")}</CardTitle>
              <p className={styles.actionDescription}>{t("Сохраните последние")}{' '}{activeConfig.replay_seconds} {' '}{t("секунд одним нажатием.")}</p>
            </div>
            <kbd className={styles.hotkey}>{saveHotkey}</kbd>
          </CardHeader>

          <CardFooter className="flex-col items-stretch gap-4">

          <Button
            variant="primary"
            fullWidth
            busy={isSavingReplay}
            busyLabel={t("Сохраняем…")}
            leadingIcon={<Bookmark aria-hidden="true" />}
            disabled={
              disabled || !replayReady || anyCommandPending || saveBlockedByError
            }
            onClick={() => void onSaveReplay()}
            data-testid="save-replay-button"
          >
            {replaySaveLabel}
          </Button>

          {!replayReady && !saveReplayError && (
            <InlineStatus tone={disabled ? 'warning' : 'neutral'}>
              {disabled
                ? t('Нет связи со службой.')
                : snapshot.lifecycle === 'blocked' || snapshot.lifecycle === 'failed'
                  ? t('Повтор станет доступен после исправления ошибки.')
                  : t('Сначала включите Replay.')}
            </InlineStatus>
          )}
          <ActionFeedback
            error={saveReplayError}
            errorTitle={t("Не удалось сохранить повтор")}
            success={saveReplaySuccess}
            onOpenSettings={onOpenSettings}
          />
        </CardFooter>
        </Card>

        <Card className={styles.continuousAction} role="region"
          aria-labelledby="continuous-action-title"
        >
          <CardHeader className="flex flex-wrap items-start justify-between gap-3">
            <div>
              <CardTitle id="continuous-action-title" role="heading" aria-level={2}>
                {t("Запись экрана")}</CardTitle>
              <p className={styles.actionDescription}>{t("Запись от начала до остановки.")}</p>
            </div>
            <kbd className={styles.hotkey}>{recordingHotkey}</kbd>
          </CardHeader>

          <CardFooter className="flex-col items-stretch gap-4">

          <Button
            variant={snapshot.continuousRecordingActive ? 'danger' : 'secondary'}
            fullWidth
            busy={isTogglingContinuous}
            busyLabel={snapshot.continuousRecordingActive ? t('Останавливаем…') : t('Запускаем…')}
            leadingIcon={
              snapshot.continuousRecordingActive ? (
                <span className={styles.recordingDot} aria-hidden="true" />
              ) : (
                <Disc aria-hidden="true" />
              )
            }
            disabled={
              disabled ||
              anyCommandPending ||
              continuousStartBlockedByError ||
              (!continuousRecordingCanStart && !continuousRecordingCanStop)
            }
            onClick={() => void onToggleContinuous()}
            data-testid="toggle-recording-button"
          >
            {snapshot.continuousRecordingActive ? t('Остановить запись') : t('Начать запись')}
          </Button>


          {!activeConfig.continuous_recording_enabled &&
            !snapshot.continuousRecordingActive &&
            !toggleContinuousError && (
              <div className={styles.disabledExplanation}>
                <InlineStatus tone="neutral">{t("Отключена в настройках.")}</InlineStatus>
                {onOpenSettings && (
                  <Button
                    variant="tertiary"
                    size="compact"
                    onClick={() => onOpenSettings('hotkeys')}
                  >
                    {t("Настроить")}</Button>
                )}
              </div>
            )}
          <ActionFeedback
            error={toggleContinuousError}
            errorTitle={
              snapshot.continuousRecordingActive
                ? t('Не удалось остановить обычную запись')
                : t('Не удалось начать обычную запись')
            }
            onOpenSettings={onOpenSettings}
          />
        </CardFooter>
        </Card>
      </div>

      <section className={styles.engineActions} aria-label={t("Управление Replay")}>

        <div className={styles.engineButtons}>
          {lifecycleTransitioning ? (
            <InlineStatus tone="busy">
              {snapshot.lifecycle === 'starting'
                ? t('Replay включается…')
                : t('Служба восстанавливает запись…')}
            </InlineStatus>
          ) : canStopReplay ? (
            <Button
              variant="tertiary"
              busy={isStoppingReplay}
              busyLabel={t("Выключаем…")}
              leadingIcon={<PowerOff aria-hidden="true" />}
              disabled={disabled || anyCommandPending}
              onClick={() => void onStopReplay()}
              title={t("Выключить до следующего запуска приложения")}
              data-testid="stop-replay-button"
            >
              {t("Выключить Replay")}</Button>
          ) : canStartReplay ? (
            <Button
              variant="secondary"
              busy={isStartingReplay}
              busyLabel={t("Включаем…")}
              leadingIcon={<Power aria-hidden="true" />}
              disabled={disabled || anyCommandPending || startBlockedByError}
              onClick={() => void onStartReplay()}
              data-testid="start-replay-button"
            >
              {snapshot.lifecycle === 'blocked' || snapshot.lifecycle === 'failed'
                ? t('Повторить запуск')
                : t('Включить Replay')}
            </Button>
          ) : null}
        </div>

        {(startReplayError || stopReplayError) && (
          <div className={styles.engineFeedback}>
            <ActionFeedback
              error={startReplayError ?? stopReplayError}
              errorTitle={
                startReplayError
                  ? t('Не удалось включить Replay')
                  : t('Не удалось выключить Replay')
              }
              onOpenSettings={onOpenSettings}
            />
          </div>
        )}
      </section>

    </section>
  );
}
