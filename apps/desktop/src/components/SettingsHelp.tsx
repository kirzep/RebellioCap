import type { ReactNode } from 'react';
import { Info } from 'lucide-react';
import { useTranslation } from '../i18n';
import { settingsHelp, type SettingsHelpTopic } from '../i18n/settingsHelp';
import { Tooltip, TooltipContent, TooltipProvider, TooltipTrigger } from './ui/tooltip';
import styles from './SettingsHelp.module.css';

/** Keep the help button outside labels so it cannot activate the setting. */
export function SettingsHelp({ topic }: { topic: SettingsHelpTopic }) {
  const { t } = useTranslation();
  const help = settingsHelp[topic];
  return (
    <TooltipProvider delayDuration={300}>
      <Tooltip>
        <TooltipTrigger asChild>
          <button type="button" className={styles.trigger} aria-label={t('Подсказка: {0}', t(help.label))}>
            <Info size={15} aria-hidden="true" />
          </button>
        </TooltipTrigger>
        <TooltipContent className={styles.content} sideOffset={6} collisionPadding={16}
          onEscapeKeyDown={event => event.stopPropagation()}>
          {t(help.text)}
        </TooltipContent>
      </Tooltip>
    </TooltipProvider>
  );
}

export function SettingsLabel({ topic, htmlFor, className, children }: {
  topic: SettingsHelpTopic;
  htmlFor: string;
  className?: string;
  children: ReactNode;
}) {
  return <div className={`${styles.labelRow} ${className ?? ''}`}>
    <label htmlFor={htmlFor}>{children}</label>
    <SettingsHelp topic={topic} />
  </div>;
}
