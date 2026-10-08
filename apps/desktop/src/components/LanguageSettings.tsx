import { useState } from 'react';
import { setLanguagePreference, useLanguage, useTranslation, type LanguagePreference } from '../i18n';

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
  return <section className="rounded-2xl border p-5">
    <label htmlFor="application-language" className="block font-medium">{t('Язык интерфейса')}</label>
    <select id="application-language" className="mt-3 w-full rounded-lg border bg-background p-3"
      value={preference} disabled={busy || !ready} onChange={event => void change(event.target.value as LanguagePreference)}>
      <option value="system">{t('Как в Windows')}</option>
      <option value="ru">Русский</option>
      <option value="en">English</option>
    </select>
    <p className="mt-2 text-sm text-muted-foreground">{t('Для русского языка Windows используется русский интерфейс, для остальных — английский.')}</p>
    <p className="mt-2 text-sm text-muted-foreground">{t('Язык меняется сразу и сохраняется для следующего запуска.')}</p>
    {busy && <p role="status" className="mt-2 text-sm">{t('Сохраняем язык…')}</p>}
    {error && <p role="alert" className="mt-2 text-sm text-destructive">{t('Не удалось сохранить язык. Попробуйте ещё раз.')}</p>}
  </section>;
}
