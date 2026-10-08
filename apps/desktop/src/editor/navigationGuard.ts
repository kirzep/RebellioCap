type Action = () => void;
const guards: Array<(action: Action, preserveEditor: boolean) => void> = [];

export function registerNavigationGuard(guard: (action: Action, preserveEditor: boolean) => void) {
  guards.push(guard);
  return () => { const index = guards.indexOf(guard); if (index >= 0) guards.splice(index, 1); };
}

/** preserveEditor is only for section changes that keep the montage mounted. */
export function requestNavigation(action: Action, preserveEditor = false) {
  const guard = guards.at(-1);
  if (guard) guard(action, preserveEditor); else action();
}
