import { SettingsHelp, SettingsLabel } from '../../components/SettingsHelp';
import { t } from '../../i18n';
import type { OnboardingDraft } from '../../config/model';
import type { ValidationError } from '../../config/validation';
import { Field } from '../../components/ui/field';
import { Input } from '../../components/ui/input';
import { Checkbox } from '../../components/ui/checkbox';
import { NativeSelect, NativeSelectOption } from '../../components/ui/native-select';
import styles from './steps.module.css';

export function VideoEncodingSettings({ draft, onChange, errors }: {
  draft: OnboardingDraft;
  onChange: (patch: Partial<OnboardingDraft>) => void;
  errors: ValidationError[];
}) {
  const cqpMode=draft.rate_control==='cqp';
  const error=errors.find(error=>error.field==='cqp');
  return <>
    <Field className={styles.fieldPlain}>
      <SettingsLabel topic="compression" htmlFor="video-rate-control" className={styles.fieldLabel}>{t('Режим сжатия')}</SettingsLabel>
      <NativeSelect id="video-rate-control" className="w-full" value={draft.rate_control ?? 'vbr'}
        onChange={event=>onChange({ rate_control:event.target.value==='cqp'?'cqp':'vbr',
          cqp: Number.isInteger(draft.cqp) && draft.cqp!>=0 && draft.cqp!<=51 ? draft.cqp : 23,
          bitrate: Number.isInteger(draft.bitrate) && draft.bitrate!>=1000000 && draft.bitrate!<=200000000 ? draft.bitrate : 12000000 })}>
        <NativeSelectOption value="vbr">{t('По битрейту (VBR)')}</NativeSelectOption>
        <NativeSelectOption value="cqp">{t('Постоянное качество (CQP)')}</NativeSelectOption>
      </NativeSelect>
      {cqpMode && <>
        <SettingsLabel topic="cqp" htmlFor="video-cqp" className={styles.fieldLabel}>{t('Уровень CQP')}</SettingsLabel>
        <Input id="video-cqp" type="number" min={0} max={51} step={1}
          value={draft.cqp===null?'':draft.cqp??23} aria-invalid={Boolean(error)} aria-describedby="video-cqp-help"
          onChange={event=>onChange({ cqp:event.target.value===''?null:Number(event.target.value) })} />
        {error && <p className={styles.fieldError} role="alert">{t(error.message)}</p>}
        <p id="video-cqp-help" className={styles.fieldHint}>{t('От 0 до 51. Меньше число — выше качество и больше размер файла. Начните с 23. Размер файлов и расход памяти Replay зависят от сцены; при лимите памяти доступная длина повтора может сокращаться.')}</p>
      </>}
    </Field>
    <div className={styles.checkboxRow}>
      <label className="flex flex-1 cursor-pointer items-start gap-3">
        <Checkbox id="video-recording-priority" checked={draft.recording_priority??false}
          onCheckedChange={checked=>onChange({ recording_priority:checked===true })} />
        <span><strong>{t('Приоритет записи')}</strong>
          <small>{t('Повышает приоритет подготовки кадров на видеокарте. Может улучшить плавность записи под нагрузкой и немного снизить FPS игры. Если приоритет недоступен, запись продолжится.')}</small>
        </span>
      </label>
      <SettingsHelp topic="priority" />
    </div>
  </>;
}
