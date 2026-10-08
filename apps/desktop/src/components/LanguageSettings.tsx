import { useState } from 'react';
import { NativeSelect } from './ui/native-select';
import { setLanguagePreference, useLanguage, useTranslation, type LanguagePreference } from '../i18n';
import styles from './WindowShell.module.css';

export function LanguageSettings() {
  const { preference, language, ready } = useLanguage();
  const { t } = useTranslation();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState(false);
  async function change(next: LanguagePreference) {
    setBusy(true);
    setError(false);
    try { await setLanguagePreference(next); }
    catch { setError(true); }
    finally { setBusy(false); }
  }
  return <div>
    <div className={styles.languageControl}>
    <NativeSelect id="application-language" className={styles.languageSelect} contentClassName={styles.languageMenu}
      renderOption={(value, label) => <span className={styles.languageOption}>
        <svg className={styles.languageFlag} viewBox="0 0 24 16" aria-hidden="true">
          {(value === 'system' ? language : value) === 'ru' ? <>
            <path fill="#fff" d="M0 0h24v16H0z" /><path fill="#2254ad" d="M0 5.33h24v5.34H0z" /><path fill="#d84149" d="M0 10.67h24V16H0z" />
          </> : <>
            <path fill="#234578" d="M0 0h24v16H0z" /><path stroke="#fff" strokeWidth="4" d="m0 0 24 16m0-16L0 16" />
            <path stroke="#ce3544" strokeWidth="1.5" d="m0 0 24 16m0-16L0 16" /><path stroke="#fff" strokeWidth="6" d="M12 0v16M0 8h24" />
            <path stroke="#ce3544" strokeWidth="3" d="M12 0v16M0 8h24" />
          </>}
        </svg><span className={styles.languageText}>{label}</span>
      </span>}
      aria-label={t('Язык интерфейса')} title={t('Язык интерфейса')}
      value={preference} disabled={busy || !ready} onChange={event => void change(event.target.value as LanguagePreference)}>
      <option value="system">{t('Как в Windows')}</option>
      <option value="ru">Русский</option>
      <option value="en">English</option>
    </NativeSelect>
    </div>
    {busy && <p role="status" className="sr-only">{t('Сохраняем язык…')}</p>}
    {error && <p role="alert" className={styles.languageError} title={t('Не удалось сохранить язык. Попробуйте ещё раз.')}>
      <span aria-hidden="true">!</span><span className={styles.languageErrorText}>{t('Не удалось сохранить язык. Попробуйте ещё раз.')}</span>
    </p>}
  </div>;
}
