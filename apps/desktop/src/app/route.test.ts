import { describe, expect, it } from 'vitest';
import { isSameRoute, type Route } from './route';

describe('route helpers', () => {
  it('correctly compares routes', () => {
    expect(isSameRoute({ kind: 'loading' }, { kind: 'loading' })).toBe(true);
    expect(isSameRoute({ kind: 'loading' }, { kind: 'start' })).toBe(false);
    expect(isSameRoute({ kind: 'onboarding', step: 1 }, { kind: 'onboarding', step: 1 })).toBe(true);
    expect(isSameRoute({ kind: 'onboarding', step: 1 }, { kind: 'onboarding', step: 2 })).toBe(false);
    expect(isSameRoute({ kind: 'home' }, { kind: 'home' })).toBe(true);
    expect(isSameRoute({ kind: 'blocked', error: 'e1' }, { kind: 'blocked', error: 'e1' })).toBe(true);
    expect(isSameRoute({ kind: 'blocked', error: 'e1' }, { kind: 'blocked', error: 'e2' })).toBe(false);
  });
});
