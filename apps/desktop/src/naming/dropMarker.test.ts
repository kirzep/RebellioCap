import {expect, it, vi} from 'vitest';
import {dropMarker} from './dropMarker';

it('positions a caret beside a block when its collapsed range has no geometry', () => {
  const root = document.createElement('div');
  const block = document.createElement('span');
  root.append(block);
  vi.spyOn(root, 'getBoundingClientRect').mockReturnValue(new DOMRect(100, 200, 500, 92));
  const range = document.createRange();
  range.setStart(root, 0);
  range.collapse(true);
  range.getBoundingClientRect = vi.fn(() => new DOMRect());
  const probe = document.createRange();
  probe.getBoundingClientRect = vi.fn(() => new DOMRect(120, 220, 80, 26));
  vi.spyOn(range, 'cloneRange').mockReturnValue(probe);
  expect(dropMarker(range, root)).toEqual({left:20, top:20, height:26});
  range.setStart(root, 1);
  range.collapse(true);
  expect(dropMarker(range, root)).toEqual({left:100, top:20, height:26});
  probe.getBoundingClientRect = vi.fn(() => new DOMRect());
  expect(dropMarker(range, root)).toBeUndefined();
});
