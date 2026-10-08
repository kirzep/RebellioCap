import { Component, lazy, Suspense, type ComponentProps, type ReactNode } from 'react';

const Editor = lazy(() => import('./ClipEditor').then(module => ({ default: module.ClipEditor })));
type Props = ComponentProps<typeof Editor>;

class EditorLoadBoundary extends Component<{ children: ReactNode; onClose: () => void }, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError() { return { failed: true }; }
  render() {
    if (this.state.failed) return <section role="alert">
      <p>Не удалось открыть редактор. Перезапустите приложение и попробуйте снова.</p>
      <button type="button" onClick={this.props.onClose}>Назад</button>
    </section>;
    return this.props.children;
  }
}

/** Keep the editor, its styles and artwork out of the recording/overlay startup path. */
export function LazyClipEditor(props: Props) {
  return <EditorLoadBoundary onClose={props.onClose}>
    <Suspense fallback={<section aria-label="Загрузка редактора">
      <p role="status">Открываем редактор…</p>
      <button type="button" onClick={props.onClose}>Назад</button>
    </section>}>
      <Editor {...props} />
    </Suspense>
  </EditorLoadBoundary>;
}
