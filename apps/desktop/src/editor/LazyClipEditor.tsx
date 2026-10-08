import {useTranslation} from '../i18n';
import { Component, lazy, Suspense, type ComponentProps, type ReactNode } from 'react';

const Editor = lazy(() => import('./ClipEditor').then(module => ({ default: module.ClipEditor })));
type Props = ComponentProps<typeof Editor>;

class EditorLoadBoundary extends Component<{ children: ReactNode; onClose: () => void }, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError() { return { failed: true }; }
  render() {
    if (this.state.failed) return <EditorLoadError onClose={this.props.onClose}/>;
    return this.props.children;
  }
}
function EditorLoadError({onClose}:{onClose:()=>void}) {
 const {t}=useTranslation();
 return <section role="alert">
      <p>{t("Не удалось открыть редактор. Перезапустите приложение и попробуйте снова.")}</p>
      <button type="button" onClick={onClose}>{t("Назад")}</button>
    </section>;
}

/** Keep the editor, its styles and artwork out of the recording/overlay startup path. */
export function LazyClipEditor(props: Props) {
 const {t}=useTranslation();
  return <EditorLoadBoundary onClose={props.onClose}>
    <Suspense fallback={<section aria-label={t("Загрузка редактора")}>
      <p role="status">{t("Открываем редактор…")}</p>
      <button type="button" onClick={props.onClose}>{t("Назад")}</button>
    </section>}>
      <Editor {...props} />
    </Suspense>
  </EditorLoadBoundary>;
}
