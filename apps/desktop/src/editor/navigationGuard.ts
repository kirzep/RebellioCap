type Action = () => void;
const guards: Array<(action: Action) => void> = [];

export function registerNavigationGuard(guard: (action: Action) => void) {
  guards.push(guard);
  return () => { const index = guards.indexOf(guard); if (index >= 0) guards.splice(index, 1); };
}

export function requestNavigation(action: Action) {
  const guard = guards.at(-1);
  if (guard) guard(action); else action();
}
