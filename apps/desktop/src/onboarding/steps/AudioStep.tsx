import { t, useTranslation } from '../../i18n';
import { FieldGroup, FieldLabel } from '../../components/ui/field';
import { NativeSelect, NativeSelectOption } from '../../components/ui/native-select';
import React, { useEffect, useRef, useState } from 'react';
import type { AppError, HostBridge } from '../../bridge/contracts';
import { normalizeError } from '../../bridge/host';
import { audioCatalogCache } from '../../bridge/deviceCatalogCache';
import { Button, ErrorNotice } from '../../components/Primitives';
import type { AudioCatalog, AudioChoice, AudioSelection, OnboardingDraft } from '../../config/model';
import type { ValidationError } from '../../config/validation';
import styles from './steps.module.css';
import { LiveAudioMeter } from '../../components/LiveAudioMeter';

export interface AudioStepProps {
  liveLevels?: boolean;
  draft: OnboardingDraft;
  hostBridge: HostBridge;
  onChange: (patch: Partial<OnboardingDraft>) => void;
  errors?: ValidationError[];
  onAvailabilityIssueChange?: (message: string | null) => void;
  onCatalogBusyChange?: (busy: boolean) => void;
}

type AudioSource = 'system' | 'microphone';
type MeasurementPhase =
  | 'untested'
  | 'measuring'
  | 'signal'
  | 'silence'
  | 'cancelled'
  | 'unavailable'
  | 'error';

interface MeasurementState {
  phase: MeasurementPhase;
  peak: number;
  error?: AppError;
}

const EMPTY_CATALOG: AudioCatalog = { system_audio: [], microphones: [] };
const INITIAL_MEASUREMENT: MeasurementState = { phase: 'untested', peak: 0 };

function endpointId(selection?: AudioSelection | null): string | null {
  return selection && typeof selection === 'object' ? selection.endpoint : null;
}

function measurementText(state: MeasurementState): string {
  switch (state.phase) {
    case 'measuring':
      return t('Слушаем примерно 3 секунды…');
    case 'signal':
      return t('Сигнал обнаружен.');
    case 'silence':
      return t('Сигнал не обнаружен. Проверьте устройство и его громкость.');
    case 'cancelled':
      return t('Проверка отменена.');
    case 'unavailable':
      return t('Устройство недоступно.');
    case 'error':
      return state.error?.summary ?? t('Не удалось проверить звук.');
    default:
      return t('Звук ещё не проверяли.');
  }
}

export function AudioStep({
  liveLevels = false,
  draft,
  hostBridge,
  onChange,
  errors = [],
  onAvailabilityIssueChange,
  onCatalogBusyChange,
}: AudioStepProps) {
  useTranslation();
  const [catalog, setCatalog] = useState<AudioCatalog>(() => audioCatalogCache.get(hostBridge) ?? EMPTY_CATALOG);
  const [catalogState, setCatalogState] = useState<'loading' | 'ready' | 'error'>(() =>
    audioCatalogCache.has(hostBridge) ? 'ready' : 'loading');
  const [catalogError, setCatalogError] = useState<AppError | null>(null);
  const [diagnosticsError, setDiagnosticsError] = useState<AppError | null>(null);
  const [reloadToken, setReloadToken] = useState(0);
  const [measurements, setMeasurements] = useState<Record<AudioSource, MeasurementState>>({
    system: INITIAL_MEASUREMENT,
    microphone: INITIAL_MEASUREMENT,
  });
  const [cancellationPending, setCancellationPending] = useState(false);
  const activeSourceRef = useRef<AudioSource | null>(null);
  const requestSerialRef = useRef(0);
  const cancellationPromiseRef = useRef<Promise<void> | null>(null);
  const mountedRef = useRef(true);
  const selectedEndpointsRef = useRef<Record<AudioSource, string | null>>({
    system: endpointId(draft.system_audio),
    microphone: endpointId(draft.microphone),
  });
  selectedEndpointsRef.current = {
    system: endpointId(draft.system_audio),
    microphone: endpointId(draft.microphone),
  };

  useEffect(() => {
    let cancelled = false;
    const cached = audioCatalogCache.get(hostBridge);
    setCatalog(cached ?? EMPTY_CATALOG);
    setCatalogState(cached ? 'ready' : 'loading');
    setCatalogError(null);

    async function loadCatalog() {
      try {
        const nextCatalog = await hostBridge.listAudioEndpoints();
        audioCatalogCache.set(hostBridge, nextCatalog);
        if (cancelled) return;

        setCatalog(nextCatalog);
        setCatalogState('ready');

        const patch: Partial<OnboardingDraft> = {};
        if (draft.system_audio == null) {
          const availableOutputs = nextCatalog.system_audio.filter((choice) => choice.available);
          const preferred =
            availableOutputs.find((choice) => choice.is_default) ?? availableOutputs[0];
          patch.system_audio = preferred ? { endpoint: preferred.id } : 'disabled';
        }
        if (draft.microphone == null) {
          patch.microphone = 'disabled';
        }
        if (Object.keys(patch).length > 0) onChange(patch);
      } catch (error) {
        audioCatalogCache.delete(hostBridge);
        if (cancelled) return;
        setCatalog(EMPTY_CATALOG);
        setCatalogState('error');
        setCatalogError(normalizeError(error));
      }
    }

    void loadCatalog();
    return () => {
      cancelled = true;
    };
    // onChange is intentionally omitted because the parent callback changes after every draft patch.
  }, [hostBridge, reloadToken]);

  useEffect(() => {
    mountedRef.current = true;
    return () => {
      mountedRef.current = false;
      requestSerialRef.current += 1;
      activeSourceRef.current = null;
      void hostBridge.measureAudioLevel(null).catch(() => undefined);
    };
  }, [hostBridge]);

  useEffect(() => {
    onCatalogBusyChange?.(catalogState === 'loading');
    return () => onCatalogBusyChange?.(false);
  }, [catalogState, onCatalogBusyChange]);

  useEffect(() => {
    const systemEndpoint = endpointId(draft.system_audio);
    const microphoneEndpoint = endpointId(draft.microphone);
    const systemUnavailable =
      catalogState === 'ready' &&
      Boolean(systemEndpoint) &&
      !catalog.system_audio.some(
        (choice) => choice.id === systemEndpoint && choice.available
      );
    const microphoneUnavailable =
      catalogState === 'ready' &&
      Boolean(microphoneEndpoint) &&
      !catalog.microphones.some(
        (choice) => choice.id === microphoneEndpoint && choice.available
      );
    const selectedDeviceNeedsCatalog = Boolean(systemEndpoint || microphoneEndpoint);
    const issue =
      catalogState === 'error' && selectedDeviceNeedsCatalog
        ? catalogError?.summary ?? t('Не удалось проверить выбранные аудиоустройства.')
        : systemUnavailable
          ? t('Выбранное устройство звука компьютера недоступно. Выберите другое или отключите звук.')
          : microphoneUnavailable
            ? t('Выбранный микрофон недоступен. Выберите другой или отключите микрофон.')
            : null;

    onAvailabilityIssueChange?.(issue);
    return () => onAvailabilityIssueChange?.(null);
  }, [
    catalog.microphones,
    catalog.system_audio,
    catalogError,
    catalogState,
    draft.microphone,
    draft.system_audio,
    onAvailabilityIssueChange,
  ]);

  function choicesFor(source: AudioSource): AudioChoice[] {
    return source === 'system' ? catalog.system_audio : catalog.microphones;
  }

  function selectionFor(source: AudioSource): AudioSelection | null | undefined {
    return source === 'system' ? draft.system_audio : draft.microphone;
  }

  function requestNativeCancellation(): Promise<void> {
    const pending = cancellationPromiseRef.current;
    if (pending) return pending;

    setCancellationPending(true);
    const request = hostBridge.measureAudioLevel(null).then(
      () => undefined,
      () => undefined
    );
    cancellationPromiseRef.current = request;
    void request.then(() => {
      if (cancellationPromiseRef.current !== request) return;
      cancellationPromiseRef.current = null;
      if (mountedRef.current) setCancellationPending(false);
    });
    return request;
  }

  function cancelMeasurement(nextPhase: MeasurementPhase = 'cancelled') {
    const currentSource = activeSourceRef.current;
    requestSerialRef.current += 1;
    activeSourceRef.current = null;
    if (currentSource) {
      setMeasurements((current) => ({
        ...current,
        [currentSource]: { phase: nextPhase, peak: 0 },
      }));
    }
    void requestNativeCancellation();
  }

  function changeSelection(source: AudioSource, value: string) {
    if (activeSourceRef.current) cancelMeasurement();
    setMeasurements((current) => ({
      ...current,
      [source]: INITIAL_MEASUREMENT,
    }));
    const selection: AudioSelection = value === 'disabled' ? 'disabled' : { endpoint: value };
    onChange(source === 'system' ? { system_audio: selection } : { microphone: selection });
  }

  function disableAllAudio() {
    if (activeSourceRef.current) cancelMeasurement();
    setMeasurements({
      system: INITIAL_MEASUREMENT,
      microphone: INITIAL_MEASUREMENT,
    });
    onChange({ system_audio: 'disabled', microphone: 'disabled' });
  }

  async function handleOpenDiagnostics() {
    setDiagnosticsError(null);
    try {
      const result = await hostBridge.runSystemCheck();
      if (!result.diagnostics_path) {
        throw new Error('Диагностический отчёт не был создан.');
      }
      await hostBridge.openDiagnostics();
    } catch (error) {
      setDiagnosticsError(normalizeError(error));
    }
  }

  async function startMeasurement(source: AudioSource) {
    if (activeSourceRef.current === source) {
      cancelMeasurement();
      return;
    }
    if (activeSourceRef.current || cancellationPromiseRef.current) return;

    const id = endpointId(selectionFor(source));
    const selectedChoice = choicesFor(source).find((choice) => choice.id === id);
    if (!id || !selectedChoice || !selectedChoice.available) {
      setMeasurements((current) => ({
        ...current,
        [source]: { phase: 'unavailable', peak: 0 },
      }));
      return;
    }

    const serial = ++requestSerialRef.current;
    activeSourceRef.current = source;
    setMeasurements((current) => ({
      ...current,
      [source]: { phase: 'measuring', peak: 0 },
    }));

    try {
      const result = await hostBridge.measureAudioLevel(id);
      if (
        serial !== requestSerialRef.current ||
        selectedEndpointsRef.current[source] !== id
      ) {
        return;
      }

      activeSourceRef.current = null;
      if (result.cancelled) {
        setMeasurements((current) => ({
          ...current,
          [source]: { phase: 'cancelled', peak: 0 },
        }));
        return;
      }

      const peak = result.samples.reduce(
        (maximum, sample) => Math.max(maximum, Number.isFinite(sample.level) ? sample.level : 0),
        0
      );
      setMeasurements((current) => ({
        ...current,
        [source]: {
          phase: result.samples.length > 0 && peak > 0 ? 'signal' : 'silence',
          peak: Math.min(1, Math.max(0, peak)),
        },
      }));
    } catch (error) {
      if (
        serial !== requestSerialRef.current ||
        selectedEndpointsRef.current[source] !== id
      ) {
        return;
      }
      activeSourceRef.current = null;
      setMeasurements((current) => ({
        ...current,
        [source]: { phase: 'error', peak: 0, error: normalizeError(error) },
      }));
    }
  }

  function renderSource(
    source: AudioSource,
    title: string,
    description: string,
    selectId: string,
    testId: string
  ) {
    const choices = choicesFor(source);
    const selection = selectionFor(source);
    const selectedId = endpointId(selection);
    const selectedChoice = choices.find((choice) => choice.id === selectedId);
    const savedChoiceMissing = Boolean(selectedId) && !selectedChoice;
    const unavailable = catalogState === 'ready' && Boolean(selectedId) && (!selectedChoice || !selectedChoice.available);
    const measurement = measurements[source];
    const isMeasuring = activeSourceRef.current === source;
    const anotherMeasurementRunning = Boolean(activeSourceRef.current) && !isMeasuring;
    const validationField = source === 'system' ? 'system_audio' : 'microphone';
    const validation = errors.find((error) => error.field === validationField);
    const descriptionId = `${selectId}-description`;
    const errorId = `${selectId}-error`;

    return (
      <section className={styles.audioGroup} aria-labelledby={`${selectId}-title`}>
        <div>
          <h3 id={`${selectId}-title`} className={styles.sectionTitle}>{title}</h3>
          <p id={descriptionId} className={styles.fieldHint}>{description}</p>
        </div>

        <FieldLabel className={styles.srOnly} htmlFor={selectId}>{title}</FieldLabel>
        <NativeSelect
          id={selectId}
          className="w-full"
          value={selectedId ?? 'disabled'}
          disabled={catalogState !== 'ready'}
          aria-invalid={Boolean(validation)}
          aria-describedby={validation ? `${descriptionId} ${errorId}` : descriptionId}
          onChange={(event) => changeSelection(source, event.target.value)}
          data-testid={testId}
        >
          <NativeSelectOption value="disabled">{t("Не записывать")}</NativeSelectOption>
          {savedChoiceMissing && (
            <NativeSelectOption value={selectedId ?? ''} disabled>
              {catalogState === 'loading' ? t('Получаем список устройств…') : t('Сохранённое устройство недоступно')}
            </NativeSelectOption>
          )}
          {choices.map((choice) => (
            <NativeSelectOption key={choice.id} value={choice.id} disabled={!choice.available}>
              {choice.name}
              {choice.is_default ? t(' · По умолчанию') : ''}
              {!choice.available ? t(' · Недоступно') : ''}
            </NativeSelectOption>
          ))}
        </NativeSelect>

        {catalogState === 'ready' && choices.length === 0 && (
          <div className={styles.compactActions} role="status">
            <span className={styles.fieldHint}>
              {t("Доступных устройств нет. Можно оставить «Не записывать».")}</span>
            <Button
              size="compact"
              variant="tertiary"
              onClick={() => setReloadToken((value) => value + 1)}
            >
              {t("Обновить список")}</Button>
          </div>
        )}

        {validation && (
          <p id={errorId} className={styles.fieldError} role="alert">
            {t(validation.message)}
          </p>
        )}

        {unavailable && (
          <p className={styles.inlineWarning} role="alert">
            {t("Выбранное устройство сейчас недоступно. Выберите другое или отключите источник.")}</p>
        )}

        {liveLevels && selectedId && <LiveAudioMeter bridge={hostBridge} endpointId={selectedId} label={title} available={catalogState === 'ready' && !unavailable} />}
        {!liveLevels && selection !== 'disabled' && selection != null && (
          <div className={styles.measurementBox}>
            <div className={styles.measurementHeader}>
              <span
                className={`${styles.measurementStatus} ${
                  measurement.phase === 'signal'
                    ? styles.statusSuccess
                    : measurement.phase === 'unavailable' || measurement.phase === 'error'
                      ? styles.statusDanger
                      : ''
                }`}
                role="status"
                data-testid={source === 'microphone' ? 'audio-status' : 'audio-system-status'}
              >
                {measurementText(measurement)}
              </span>
              <Button
                type="button"
                variant="secondary"
                onClick={() => void startMeasurement(source)}
                disabled={
                  anotherMeasurementRunning ||
                  cancellationPending ||
                  unavailable ||
                  catalogState !== 'ready'
                }
                data-testid={source === 'microphone' ? 'audio-check-button' : 'audio-system-check-button'}
              >
                {isMeasuring
                  ? t('Отменить')
                  : cancellationPending
                    ? t('Отменяем…')
                    : t('Проверить звук')}
              </Button>
            </div>

            {measurement.phase === 'error' && measurement.error && (
              <ErrorNotice
                title={t("Проверка звука не выполнена")}
                technicalDetails={t("Код: {0}\n{1}", measurement.error.code, measurement.error.technicalCause)}
              >
                {measurement.error.summary}
              </ErrorNotice>
            )}

            {(measurement.phase === 'signal' || measurement.phase === 'silence') && (
              <div
                className={styles.audioMeter}
                role="meter"
                aria-label={t("Максимальный уровень: {0}%", Math.round(measurement.peak * 100))}
                aria-valuemin={0}
                aria-valuemax={100}
                aria-valuenow={Math.round(measurement.peak * 100)}
              >
                <span
                  className={styles.audioMeterFill}
                  style={{ width: `${Math.round(measurement.peak * 100)}%` }}
                  data-testid={source === 'microphone' ? 'audio-meter-fill' : undefined}
                />
              </div>
            )}
          </div>
        )}

        {!liveLevels && source === 'microphone' && selection !== 'disabled' && (
          <Button
            type="button"
            variant="tertiary" size="compact"
            onClick={() => changeSelection('microphone', 'disabled')}
            data-testid="audio-disable-mic-button"
          >
            {t("Не записывать микрофон")}</Button>
        )}
      </section>
    );
  }

  return (
    <div className={styles.stepContent} data-testid="audio-step">
      <div className={styles.stepIntro}>
        <h2 className={styles.title}>{t("Звук")}</h2>
        <p className={styles.copy}>{t("Выберите источники. Уровень звука поможет проверить сигнал.")}</p>
      </div>

      {catalogState === 'loading' && (
        <p className={styles.srOnly} role="status">{t("Получаем список аудиоустройств…")}</p>
      )}
      {catalogState === 'error' && (
        <ErrorNotice
          title={t("Не удалось получить список аудиоустройств")}
          technicalDetails={catalogError
            ? t("Код: {0}\n{1}", catalogError.code, catalogError.technicalCause)
            : undefined}
          action={(
            <div className={styles.compactActions}>
              <Button size="compact" variant="tertiary" onClick={() => setReloadToken((value) => value + 1)}>
                {t("Повторить")}</Button>
              <Button size="compact" variant="tertiary" onClick={() => void handleOpenDiagnostics()}>
                {t("Диагностика")}</Button>
              <Button
                size="compact"
                variant="tertiary"
                onClick={disableAllAudio}
              >
                {t("Продолжить без звука")}</Button>
            </div>
          )}
        >
          {catalogError?.summary}
        </ErrorNotice>
      )}
      {diagnosticsError && (
        <ErrorNotice
          title={t("Не удалось открыть диагностику")}
          technicalDetails={t("Код: {0}\n{1}", diagnosticsError.code, diagnosticsError.technicalCause)}
          action={(
            <Button size="compact" variant="tertiary" onClick={() => void handleOpenDiagnostics()}>
              {t("Повторить")}</Button>
          )}
        >
          {t(diagnosticsError.summary)}
        </ErrorNotice>
      )}

      <FieldGroup className={styles.audioGroups}>
        {renderSource(
          'system',
          t('Звук компьютера'),
          t('Записывается звук выбранного устройства вывода, не только звук игры.'),
          'audio-system',
          'audio-system-select'
        )}
        {renderSource(
          'microphone',
          t('Микрофон'),
          t('Если включить микрофон, ваш голос попадёт в запись.'),
          'audio-microphone',
          'audio-mic-select'
        )}
      </FieldGroup>
    </div>
  );
}
