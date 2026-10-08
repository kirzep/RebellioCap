import { useEffect, useRef } from 'react';
import { MODE_FRAMES, paintFrame, resolvePreset } from 'thinking-orbs/engine';

/** Paint the library's composing geometry directly at the displayed pixel resolution. */
export function RecordingOrb({ className }: { className?: string }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    const context = canvas?.getContext('2d');
    if (!canvas || !context) return;

    const { mode, speed, opts } = resolvePreset('composing', 64);
    const motion = window.matchMedia('(prefers-reduced-motion: reduce)');
    let animationFrame = 0;
    let visible = true;

    function paint(time: number) {
      const size = canvas!.getBoundingClientRect().width || 192;
      const resolution = Math.round(size * (window.devicePixelRatio || 1));
      if (canvas!.width !== resolution || canvas!.height !== resolution) {
        canvas!.width = resolution;
        canvas!.height = resolution;
      }
      context!.setTransform(1, 0, 0, 1, 0, 0);
      context!.clearRect(0, 0, resolution, resolution);
      // Scale vector geometry before painting, rather than scaling a small bitmap.
      const scale = resolution / 64;
      context!.setTransform(scale, 0, 0, scale, 0, 0);
      paintFrame(context!, MODE_FRAMES[mode](64, time * speed, opts), true);
    }

    function animate(now: number) {
      paint(now / 1000);
      animationFrame = requestAnimationFrame(animate);
    }

    function syncAnimation() {
      cancelAnimationFrame(animationFrame);
      if (motion.matches) paint(0.6);
      else if (visible && document.visibilityState !== 'hidden') {
        paint(performance.now() / 1000);
        animationFrame = requestAnimationFrame(animate);
      }
    }

    const resizeObserver = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(syncAnimation);
    const intersectionObserver = typeof IntersectionObserver === 'undefined' ? null : new IntersectionObserver(([entry]) => {
      visible = entry.isIntersecting;
      syncAnimation();
    });
    resizeObserver?.observe(canvas);
    intersectionObserver?.observe(canvas);
    motion.addEventListener('change', syncAnimation);
    document.addEventListener('visibilitychange', syncAnimation);
    window.addEventListener('resize', syncAnimation);
    syncAnimation();

    return () => {
      cancelAnimationFrame(animationFrame);
      resizeObserver?.disconnect();
      intersectionObserver?.disconnect();
      motion.removeEventListener('change', syncAnimation);
      document.removeEventListener('visibilitychange', syncAnimation);
      window.removeEventListener('resize', syncAnimation);
    };
  }, []);

  return <canvas ref={canvasRef} className={className} aria-hidden="true"
    style={{ width: 'clamp(144px, 24vh, 192px)', height: 'clamp(144px, 24vh, 192px)', display: 'block' }} />;
}
