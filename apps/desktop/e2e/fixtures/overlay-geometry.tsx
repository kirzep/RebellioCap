import { useState } from 'react';
import { createRoot } from 'react-dom/client';
import { OverlayEditor } from '../../src/components/OverlayEditor';
import { OverlayIndicator, OverlayNotification } from '../../src/components/OverlayVisuals';
import { defaultOverlayLayout, overlayPosition } from '../../src/components/overlayLayout';
import '../../src/styles/global.css';
const params = new URLSearchParams(location.search);
const display = { width: Number(params.get('width')) || 1920, height: Number(params.get('height')) || 1080 };
function GeometryFixture() {
 const [layout, setLayout] = useState({ ...defaultOverlayLayout, recording: { x: .965, y: .98 }, replay: { x: .99, y: .98 } });
 return <>
  <div style={{ width: 640 }}><OverlayEditor display={display} layout={layout} disabled={false} onSave={setLayout}/></div>
  <div data-runtime style={{ position: 'relative', width: display.width, height: display.height }}>
   <div data-runtime-recording className="capture-indicator-position" style={overlayPosition(layout.recording)}><OverlayIndicator recording/></div>
   <div data-runtime-replay className="capture-indicator-position" style={overlayPosition(layout.replay)}><OverlayIndicator/></div>
   <div data-runtime-notification className="capture-notification-position" style={overlayPosition(layout.messages)}><OverlayNotification kind="replay_saved" preview/></div>
  </div>
 </>;
}
createRoot(document.getElementById('root')!).render(<GeometryFixture/>);
