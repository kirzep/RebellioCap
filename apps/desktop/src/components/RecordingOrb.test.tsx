import { render } from '@testing-library/react';
import { afterEach, expect, test, vi } from 'vitest';
import { RecordingOrb } from './RecordingOrb';

vi.mock('thinking-orbs/engine', () => ({
  resolvePreset: () => ({ mode: 'ribbon', speed: 1, opts: {} }),
  MODE_FRAMES: { ribbon: () => ({ dots: [], lines: [] }) },
  paintFrame: vi.fn(),
}));

afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals(); });

test('paints at displayed size and device pixel ratio, then cancels animation on unmount', () => {
  const context = { setTransform: vi.fn(), clearRect: vi.fn() };
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(context as unknown as CanvasRenderingContext2D);
  vi.spyOn(HTMLCanvasElement.prototype, 'getBoundingClientRect').mockReturnValue({ width: 192 } as DOMRect);
  vi.stubGlobal('devicePixelRatio', 2);
  const request = vi.fn().mockReturnValue(42);
  const cancel = vi.fn();
  vi.stubGlobal('requestAnimationFrame', request);
  vi.stubGlobal('cancelAnimationFrame', cancel);
  const view = render(<RecordingOrb />);
  const canvas = view.container.querySelector('canvas')!;
  expect(canvas.width).toBe(384);
  expect(canvas.height).toBe(384);
  expect(context.setTransform).toHaveBeenLastCalledWith(6, 0, 0, 6, 0, 0);
  view.unmount();
  expect(cancel).toHaveBeenLastCalledWith(42);
});

test('renders a static frame when reduced motion is enabled', () => {
  const context = { setTransform: vi.fn(), clearRect: vi.fn() };
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(context as unknown as CanvasRenderingContext2D);
  const original = window.matchMedia('(prefers-reduced-motion: reduce)');
  vi.spyOn(window, 'matchMedia').mockReturnValue({ ...original, matches: true });
  const request = vi.fn();
  vi.stubGlobal('requestAnimationFrame', request);
  render(<RecordingOrb />);
  expect(context.clearRect).toHaveBeenCalled();
  expect(request).not.toHaveBeenCalled();
});
