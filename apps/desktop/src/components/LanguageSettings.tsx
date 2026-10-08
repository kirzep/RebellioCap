import { useState } from 'react';
import { ChevronDown, Globe } from 'lucide-react';
import { setLanguagePreference, useLanguage, useTranslation, type LanguagePreference } from '../i18n';
import styles from './WindowShell.module.css';

export function LanguageSettings() {
  const { preference, ready } = useLanguage();
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
    <Globe className={styles.languageIcon} aria-hidden="true" />
    <select id="application-language" className={styles.languageSelect}
      aria-label={t('Язык интерфейса')} title={t('Язык интерфейса')}
      value={preference} disabled={busy || !ready} onChange={event => void change(event.target.value as LanguagePreference)}>
      <option value="system">{t('Как в Windows')}</option>
      <option value="ru">Русский</option>
      <option value="en">English</option>
    </select>
    <ChevronDown className={styles.languageChevron} aria-hidden="true" />
    </div>
    {busy && <p role="status" className="sr-only">{t('Сохраняем язык…')}</p>}
    {error && <p role="alert" className={styles.languageError} title={t('Не удалось сохранить язык. Попробуйте ещё раз.')}>
      <span aria-hidden="true">!</span><span className={styles.languageErrorText}>{t('Не удалось сохранить язык. Попробуйте ещё раз.')}</span>
    </p>}
  </div>;
}
