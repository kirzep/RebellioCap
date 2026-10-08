import { t, useTranslation } from '../../i18n';
import { Field, FieldGroup, FieldSet, FieldLegend, FieldLabel } from '../../components/ui/field';
import { Input } from '../../components/ui/input';
import { ToggleGroup, ToggleGroupItem } from '../../components/ui/toggle-group';
import { NativeSelect, NativeSelectOption } from '../../components/ui/native-select';
import React, { useEffect, useMemo, useState } from 'react';
import type { AppError, HostBridge } from '../../bridge/contracts';
import { normalizeError } from '../../bridge/host';
import { monitorCatalogCache } from '../../bridge/deviceCatalogCache';
import { Button, ErrorNotice } from '../../components/Primitives';
import { balancedVideoForMonitor } from '../../config/defaults';
import { formatVideoMode, friendlyMonitorName } from '../../config/format';
import type { MonitorChoice, OnboardingDraft } from '../../config/model';
import type { ValidationError } from '../../config/validation';
import styles from './steps.module.css';

export interface VideoStepProps {
  draft: OnboardingDraft;
  hostBridge: HostBridge;
  onChange: (patch: Partial<OnboardingDraft>) => void;
  errors?: ValidationError[];
  onAvailabilityIssueChange?: (message: string | null) => void;
  onCatalogBusyChange?: (busy: boolean) => void;
  guided?: boolean;
}

type CatalogState = 'loading' | 'ready' | 'empty' | 'error';
type QualityMode = 'balanced' | 'native' | 'manual';

function evenDimension(value: number): number {
  return Math.max(64, Math.floor(value / 2) * 2);
}

function nativeSize(monitor: MonitorChoice): { width: number; height: number } {
  return {
    width: evenDimension(monitor.width),
    height: evenDimension(monitor.height),
  };
}

function formatBitrateInput(bitrate?: number | null): string {
  if (bitrate == null || !Number.isFinite(bitrate)) return '';
  return String(bitrate / 1_000_000).replace('.', ',');
}

function formatIntegerInput(value?: number | null): string {
  return value == null || !Number.isFinite(value) ? '' : String(value);
}

export function VideoStep({
  draft,
  hostBridge,
  onChange,
  errors = [],
  onAvailabilityIssueChange,
  onCatalogBusyChange,
  guided = false,
}: VideoStepProps) {
  useTranslation();
  const [monitors, setMonitors] = useState<MonitorChoice[]>(() => monitorCatalogCache.get(hostBridge) ?? []);
  const [catalogState, setCatalogState] = useState<CatalogState>(() =>
    monitorCatalogCache.get(hostBridge)?.length ? 'ready' : 'loading');
  const [catalogError, setCatalogError] = useState<AppError | null>(null);
  const [diagnosticsError, setDiagnosticsError] = useState<AppError | null>(null);
  const [reloadToken, setReloadToken] = useState(0);
  const [quality, setQuality] = useState<QualityMode>(() =>
    draft.width != null && draft.height != null ? 'manual' : 'balanced'
  );
  const [advancedOpen, setAdvancedOpen] = useState(
    !guided && draft.width != null && draft.height != null
  );
  const [customFps, setCustomFps] = useState(
    draft.fps != null && ![30, 60, 120].includes(draft.fps)
  );
  const [bitrateInput, setBitrateInput] = useState(() =>
    formatBitrateInput(draft.bitrate)
  );
  const [bitrateEditing, setBitrateEditing] = useState(false);
  const [integerInputs, setIntegerInputs] = useState(() => ({
    width: formatIntegerInput(draft.width),
    height: formatIntegerInput(draft.height),
    fps: formatIntegerInput(draft.fps),
  }));
  const [editingInteger, setEditingInteger] = useState<
    'width' | 'height' | 'fps' | null
  >(null);

  useEffect(() => {
    let cancelled = false;
    const cached = monitorCatalogCache.get(hostBridge);
    setMonitors(cached ?? []);
    setCatalogState(cached?.length ? 'ready' : 'loading');
    setCatalogError(null);

    async function loadMonitors() {
      try {
        const list = await hostBridge.listMonitors();
        monitorCatalogCache.set(hostBridge, list);
        if (cancelled) return;

        setMonitors(list);
        if (list.length === 0) {
          setCatalogState('empty');
          return;
        }

        setCatalogState('ready');
        const selected = draft.monitor_id
          ? list.find((monitor) => monitor.id === draft.monitor_id)
          : undefined;
        const initialMonitor = selected ?? (
          draft.monitor_id
            ? undefined
            : list.find((monitor) => monitor.primary) ?? list[0]
        );
        const patch: Partial<OnboardingDraft> = {};

        if (initialMonitor) {
          const initialVideo = balancedVideoForMonitor(initialMonitor);
          if (guided && draft.width != null && draft.height != null) {
            const balanced = draft.width === initialVideo.width && draft.height === initialVideo.height &&
              draft.fps === initialVideo.fps && draft.bitrate === initialVideo.bitrate;
            const native = draft.width === initialMonitor.width && draft.height === initialMonitor.height;
            setQuality(balanced ? 'balanced' : native ? 'native' : 'manual');
          }
          if (!draft.monitor_id) patch.monitor_id = initialMonitor.id;
          if (draft.width == null) patch.width = initialVideo.width;
          if (draft.height == null) patch.height = initialVideo.height;
          if (draft.fps == null) patch.fps = initialVideo.fps;
          if (draft.bitrate == null) patch.bitrate = initialVideo.bitrate;
        }

        if (Object.keys(patch).length > 0) onChange(patch);
      } catch (error) {
        monitorCatalogCache.delete(hostBridge);
        if (cancelled) return;
        setMonitors([]);
        setCatalogState('error');
        setCatalogError(normalizeError(error));
      }
    }

    void loadMonitors();
    return () => {
      cancelled = true;
    };
    // onChange is intentionally omitted: OnboardingScreen provides an inline callback.
    // Reloading the host catalog after every draft patch would discard the loading state.
  }, [hostBridge, reloadToken]);

  useEffect(() => {
    if (!bitrateEditing) setBitrateInput(formatBitrateInput(draft.bitrate));
  }, [bitrateEditing, draft.bitrate]);

  useEffect(() => {
    setIntegerInputs((current) => ({
      width:
        editingInteger === 'width'
          ? current.width
          : formatIntegerInput(draft.width),
      height:
        editingInteger === 'height'
          ? current.height
          : formatIntegerInput(draft.height),
      fps:
        editingInteger === 'fps'
          ? current.fps
          : formatIntegerInput(draft.fps),
    }));
  }, [draft.fps, draft.height, draft.width, editingInteger]);

  useEffect(() => {
    if (errors.some((error) => ['width', 'height', 'fps', 'bitrate'].includes(error.field))) {
      setAdvancedOpen(true);
    }
  }, [errors]);

  const selectedMonitor = useMemo(
    () => monitors.find((monitor) => monitor.id === draft.monitor_id),
    [draft.monitor_id, monitors]
  );

  const selectedMonitorMissing =
    catalogState === 'ready' && Boolean(draft.monitor_id) && !selectedMonitor;
  const monitorValidation = errors.find((error) => error.field === 'monitor_id');
  const widthValidation = errors.find((error) => error.field === 'width');
  const heightValidation = errors.find((error) => error.field === 'height');
  const fpsValidation = errors.find((error) => error.field === 'fps');
  const bitrateValidation = errors.find((error) => error.field === 'bitrate');

  useEffect(() => {
    onCatalogBusyChange?.(catalogState === 'loading');
    return () => onCatalogBusyChange?.(false);
  }, [catalogState, onCatalogBusyChange]);

  useEffect(() => {
    const issue =
      catalogState === 'empty'
        ? t('Не найден доступный экран.')
        : catalogState === 'error'
          ? catalogError?.summary ?? t('Не удалось проверить доступные экраны.')
          : selectedMonitorMissing
            ? t('Сохранённый экран сейчас недоступен. Выберите другой экран.')
            : null;
    onAvailabilityIssueChange?.(issue);
    return () => onAvailabilityIssueChange?.(null);
  }, [
    catalogError,
    catalogState,
    onAvailabilityIssueChange,
    selectedMonitorMissing,
  ]);

  function selectQuality(nextQuality: QualityMode) {
    setQuality(nextQuality);
    if (nextQuality === 'manual') {
      setAdvancedOpen(true);
      return;
    }
    if (!selectedMonitor) return;

    if (nextQuality === 'balanced') setCustomFps(false);

    onChange(
      nextQuality === 'balanced'
        ? balancedVideoForMonitor(selectedMonitor)
        : nativeSize(selectedMonitor)
    );
  }

  function updateManual(patch: Partial<OnboardingDraft>) {
    setQuality('manual');
    setAdvancedOpen(true);
    onChange(patch);
  }

  function updateInteger(
    field: 'width' | 'height' | 'fps',
    input: string
  ) {
    setIntegerInputs((current) => ({ ...current, [field]: input }));
    const normalized = input.trim();
    const value = /^\d+$/.test(normalized) ? Number(normalized) : null;
    if (field === 'width') updateManual({ width: value });
    if (field === 'height') updateManual({ height: value });
    if (field === 'fps') updateManual({ fps: value });
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

  const currentFps = draft.fps;
  const fpsSelectValue =
    !customFps && currentFps != null && [30, 60, 120].includes(currentFps)
      ? String(currentFps)
      : 'custom';
  const sourceRatio = selectedMonitor ? selectedMonitor.width / selectedMonitor.height : null;
  const outputRatio =
    draft.width != null && draft.height != null && draft.height > 0
      ? draft.width / draft.height
      : null;
  const stretchesImage =
    quality === 'manual' &&
    sourceRatio != null &&
    outputRatio != null &&
    Math.abs(sourceRatio - outputRatio) / sourceRatio > 0.015;

  return (
    <div className={styles.stepContent} data-testid="video-step">
      <div className={styles.stepIntro}>
        <h2 className={styles.title}>{guided ? t('Что будем записывать?') : t('Экран')}</h2>
        <p className={styles.copy}>
          {guided ? t('Выберите экран и качество. Остальное мы уже настроили.') : t('Записывается всё содержимое выбранного экрана.')}
        </p>
      </div>

      <FieldGroup className={styles.formStack}>
        <Field className={styles.fieldPlain}>
          <FieldLabel htmlFor="video-monitor" className={styles.fieldLabel}>{t("Экран")}</FieldLabel>
          <NativeSelect
            id="video-monitor"
            className="w-full"
            value={draft.monitor_id ?? ''}
            disabled={catalogState !== 'ready' || monitors.length === 0}
            aria-invalid={Boolean(monitorValidation)}
            aria-describedby={monitorValidation ? 'video-monitor-error' : undefined}
              onChange={(event) => {
                const monitor = monitors.find((item) => item.id === event.target.value);
                if (!monitor) return;
                if (quality === 'balanced') setCustomFps(false);
                const size =
                quality === 'balanced'
                  ? balancedVideoForMonitor(monitor)
                  : quality === 'native'
                    ? nativeSize(monitor)
                    : {};
              onChange({ monitor_id: monitor.id, ...size });
            }}
            data-testid="video-monitor-select"
          >
            {catalogState === 'loading' && <NativeSelectOption value="">{t("Ищем доступные экраны…")}</NativeSelectOption>}
            {(catalogState === 'empty' || catalogState === 'error') && (
              <NativeSelectOption value="">{t("Доступных экранов нет")}</NativeSelectOption>
            )}
            {selectedMonitorMissing && (
              <NativeSelectOption value={draft.monitor_id ?? ''} disabled>
                {t("Сохранённый экран недоступен")}</NativeSelectOption>
            )}
            {monitors.map((monitor) => (
              <NativeSelectOption key={monitor.id} value={monitor.id}>
                {friendlyMonitorName(monitor.name)} · {monitor.width}×{monitor.height}
                {monitor.primary ? t(' · Основной') : ''}
              </NativeSelectOption>
            ))}
          </NativeSelect>

          {monitorValidation && (
            <p id="video-monitor-error" className={styles.fieldError} role="alert">
              {t(monitorValidation.message)}
            </p>
          )}

          {catalogState === 'loading' && (
            <p className={styles.srOnly} role="status">{t("Получаем список экранов…")}</p>
          )}
          {catalogState === 'empty' && (
            <ErrorNotice
              title={t("Доступные экраны не найдены")}
              action={(
                <div className={styles.compactActions}>
                  <Button size="compact" variant="tertiary" onClick={() => setReloadToken((value) => value + 1)}>
                    {t("Повторить")}</Button>
                  <Button size="compact" variant="tertiary" onClick={() => void handleOpenDiagnostics()}>
                    {t("Диагностика")}</Button>
                </div>
              )}
            >
              {t("Windows не сообщил ни об одном доступном экране.")}</ErrorNotice>
          )}
          {catalogState === 'error' && (
            <ErrorNotice
              title={t("Не удалось получить список экранов")}
              technicalDetails={catalogError
                ? t("Код: {0}\n{1}", catalogError.code, catalogError.technicalCause)
                : undefined}
              action={(
                <div className={styles.compactActions}>
                  <Button size="compact" variant="tertiary" onClick={() => setReloadToken((value) => value + 1)}>
                    {t("Повторить")}</Button>
                  <Button size="compact" variant="tertiary" onClick={() => void handleOpenDiagnostics()}>
                    {t("Диагностика")}</Button>
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
          {selectedMonitorMissing && (
            <p className={styles.inlineWarning} role="alert">
              {t("Этот экран сейчас недоступен. Выберите устройство из списка.")}</p>
          )}
        </Field>

        <FieldSet className={styles.fieldset}>
          <FieldLegend className={styles.fieldLabel}>{t("Качество записи")}</FieldLegend>
          <p className={styles.fieldHint}>
            {t("Запись сохраняется в SDR. HDR и преобразование HDR в SDR пока не поддерживаются;\n            для корректных цветов отключите HDR в настройках экрана Windows перед записью.")}</p>
          <ToggleGroup type="single" value={quality} variant="outline" spacing={2} className="flex w-full flex-wrap"
            onValueChange={(value) => {
              if (value === 'balanced' || value === 'native' || value === 'manual') selectQuality(value);
            }} aria-label={t("Качество записи")}>
            {([
              ['balanced', t('Сбалансированное')],
              ['native', t('Как у экрана')],
              ['manual', t('Вручную')],
            ] as const).map(([value, label]) => (
              <ToggleGroupItem
                key={value}
                value={value}
                className="min-h-11 h-auto flex-1 px-3 py-2 whitespace-normal"
              >
                {t(label)}
              </ToggleGroupItem>
            ))}
          </ToggleGroup>
        </FieldSet>

        <div className={styles.resultRow} aria-live="polite">
          <div>
            <span className={styles.eyebrow}>{t("Итог записи")}</span>
            <strong className={styles.resultValue}>
              {formatVideoMode(draft.width, draft.height, draft.fps, draft.bitrate)}
            </strong>
          </div>
          <Button
            type="button"
            variant="tertiary" size="compact"
            aria-expanded={advancedOpen}
            aria-controls="video-advanced-settings"
            onClick={() => setAdvancedOpen((value) => !value)}
          >
            {advancedOpen ? t('Скрыть параметры') : t('Дополнительные параметры')}
          </Button>
        </div>

        {advancedOpen && (
          <div id="video-advanced-settings" className={styles.advancedPanel}>
            <div className={styles.inputGrid}>
              <FieldLabel className={styles.inputLabel} htmlFor="video-width">
                {t("Ширина")}<Input
                  id="video-width"
                  type="text"
                  inputMode="numeric"
                  className="w-full"
                  value={integerInputs.width}
                  aria-invalid={Boolean(widthValidation)}
                  aria-describedby={widthValidation ? 'video-width-error' : undefined}
                  onFocus={() => setEditingInteger('width')}
                  onBlur={() => setEditingInteger(null)}
                  onChange={(event) => updateInteger('width', event.target.value)}
                  data-testid="video-width-input"
                />
                {widthValidation && (
                  <span id="video-width-error" className={styles.fieldError} role="alert">
                    {t(widthValidation.message)}
                  </span>
                )}
              </FieldLabel>
              <FieldLabel className={styles.inputLabel} htmlFor="video-height">
                {t("Высота")}<Input
                  id="video-height"
                  type="text"
                  inputMode="numeric"
                  className="w-full"
                  value={integerInputs.height}
                  aria-invalid={Boolean(heightValidation)}
                  aria-describedby={heightValidation ? 'video-height-error' : undefined}
                  onFocus={() => setEditingInteger('height')}
                  onBlur={() => setEditingInteger(null)}
                  onChange={(event) => updateInteger('height', event.target.value)}
                  data-testid="video-height-input"
                />
                {heightValidation && (
                  <span id="video-height-error" className={styles.fieldError} role="alert">
                    {t(heightValidation.message)}
                  </span>
                )}
              </FieldLabel>
              <FieldLabel className={styles.inputLabel} htmlFor="video-fps">
                {t("Частота кадров")}<NativeSelect
                  id="video-fps"
                  className="w-full"
                  value={fpsSelectValue}
                  aria-invalid={Boolean(fpsValidation)}
                  aria-describedby={fpsValidation ? 'video-fps-error' : undefined}
                  onChange={(event) => {
                    if (event.target.value === 'custom') {
                      setCustomFps(true);
                      return;
                    }
                    setCustomFps(false);
                    updateManual({ fps: Number(event.target.value) });
                  }}
                  data-testid="video-fps-select"
                >
                  <NativeSelectOption value="30">{t("30 кадров/с")}</NativeSelectOption>
                  <NativeSelectOption value="60">{t("60 кадров/с")}</NativeSelectOption>
                  <NativeSelectOption value="120">{t("120 кадров/с")}</NativeSelectOption>
                  <NativeSelectOption value="custom">{t("Другое значение")}</NativeSelectOption>
                </NativeSelect>
                {fpsValidation && fpsSelectValue !== 'custom' && (
                  <span id="video-fps-error" className={styles.fieldError} role="alert">
                    {t(fpsValidation.message)}
                  </span>
                )}
              </FieldLabel>
              <FieldLabel className={styles.inputLabel} htmlFor="video-bitrate">
                {t("Битрейт, Мбит/с")}<Input
                  id="video-bitrate"
                  type="text"
                  inputMode="decimal"
                  className="w-full"
                  value={bitrateInput}
                  aria-invalid={Boolean(bitrateValidation)}
                  onFocus={() => setBitrateEditing(true)}
                  onBlur={() => setBitrateEditing(false)}
                  onChange={(event) => {
                    const nextValue = event.target.value;
                    const normalized = nextValue.trim().replace(',', '.');
                    const isDecimal = /^(?:\d+(?:\.\d*)?|\.\d+)$/.test(normalized);
                    const megabits = normalized === '' || !isDecimal ? null : Number(normalized);
                    setBitrateInput(nextValue);
                    updateManual({
                      bitrate:
                        megabits !== null && Number.isFinite(megabits)
                          ? Math.round(megabits * 1_000_000)
                          : null,
                    });
                  }}
                  aria-describedby={
                    bitrateValidation
                      ? 'video-bitrate-hint video-bitrate-error'
                      : 'video-bitrate-hint'
                  }
                  data-testid="video-bitrate-input"
                />
                {bitrateValidation && (
                  <span id="video-bitrate-error" className={styles.fieldError} role="alert">
                    {t(bitrateValidation.message)}
                  </span>
                )}
              </FieldLabel>
            </div>

            {fpsSelectValue === 'custom' && (
              <FieldLabel className={styles.inputLabel} htmlFor="video-custom-fps">
                {t("FPS вручную")}<Input
                  id="video-custom-fps"
                  type="text"
                  inputMode="numeric"
                  className="w-full"
                  value={integerInputs.fps}
                  aria-invalid={Boolean(fpsValidation)}
                  aria-describedby={fpsValidation ? 'video-fps-error' : undefined}
                  onFocus={() => setEditingInteger('fps')}
                  onBlur={() => setEditingInteger(null)}
                  onChange={(event) => updateInteger('fps', event.target.value)}
                  data-testid="video-custom-fps-input"
                />
                {fpsValidation && (
                  <span id="video-fps-error" className={styles.fieldError} role="alert">
                    {t(fpsValidation.message)}
                  </span>
                )}
              </FieldLabel>
            )}

            <p id="video-bitrate-hint" className={styles.fieldHint}>
              {t("Размеры — чётные числа от 64 до 16384, FPS — от 1 до 240, битрейт — от 1 до 200 Мбит/с.")}</p>
            {stretchesImage && (
              <p className={styles.inlineWarning} role="status">
                {t("Пропорции отличаются от выбранного экрана. Изображение будет растянуто до указанного размера.")}</p>
            )}
          </div>
        )}
      </FieldGroup>
    </div>
  );
}
