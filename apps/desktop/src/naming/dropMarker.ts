export function dropMarker(range: Range, root: HTMLElement) {
  let rect = range.getBoundingClientRect();
  let left = rect.left;
  if (!rect.height) {
    const container = range.startContainer;
    if (container.nodeType === Node.TEXT_NODE && container.textContent?.length) {
      const probe = range.cloneRange();
      const offset = range.startOffset;
      probe.setStart(container, Math.max(0, offset - 1));
      probe.setEnd(container, offset || 1);
      rect = probe.getBoundingClientRect();
      left = offset ? rect.right : rect.left;
    } else {
      const next = container.childNodes[range.startOffset];
      const previous = container.childNodes[range.startOffset - 1];
      const neighbour = next ?? previous;
      if (!neighbour) return undefined;
      const probe = range.cloneRange();
      probe.selectNode(neighbour);
      rect = probe.getBoundingClientRect();
      left = next ? rect.left : rect.right;
    }
  }
  if (!rect.height) return undefined;
  const bounds = root.getBoundingClientRect();
  return {left: Math.max(0, Math.min(bounds.width - 2, left - bounds.left)),
    top: Math.max(0, rect.top - bounds.top), height: Math.max(24, rect.height)};
}
