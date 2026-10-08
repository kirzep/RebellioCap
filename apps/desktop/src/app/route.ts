import type { AppError } from '../bridge/contracts';

export type RepairSection = 'video' | 'audio' | 'replay' | 'hotkeys';

export interface RepairContext {
  section: RepairSection;
  error: AppError;
}

export type Route =
  | { kind: 'loading' }
  | { kind: 'start' }
  | { kind: 'system-check' }
  | { kind: 'onboarding'; step: number }
  | { kind: 'home'; repair?: RepairContext }
  | { kind: 'blocked'; error?: AppError | string };

export function repairSectionForIssue(
  issue: AppError | string | null,
  action?: 'settings' | 'choose-folder'
): RepairSection {
  if (action === 'choose-folder') return 'replay';

  const hint =
    typeof issue === 'object' && issue
      ? `${issue.code} ${issue.technicalCause}`.toLowerCase()
      : String(issue ?? '').toLowerCase();

  if (
    (typeof issue === 'object' && issue?.subsystem === 'filesystem') ||
    /output_directory|output directory|\bdirectory\b|\bpath\b/.test(hint)
  ) {
    return 'replay';
  }
  if (/hotkey/.test(hint)) return 'hotkeys';
  if (
    (typeof issue === 'object' && issue?.subsystem === 'audio') ||
    /system_audio|microphone|audio endpoint|wasapi/.test(hint)
  ) {
    return 'audio';
  }
  return 'video';
}

export function isSameRoute(a: Route, b: Route): boolean {
  if (a.kind !== b.kind) return false;
  if (a.kind === 'onboarding' && b.kind === 'onboarding') {
    return a.step === b.step;
  }
  if (a.kind === 'home' && b.kind === 'home') {
    return (
      a.repair?.section === b.repair?.section &&
      a.repair?.error.code === b.repair?.error.code &&
      a.repair?.error.technicalCause === b.repair?.error.technicalCause
    );
  }
  if (a.kind === 'blocked' && b.kind === 'blocked') {
    return a.error === b.error;
  }
  return true;
}
