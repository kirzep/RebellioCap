import type {Window} from '@tauri-apps/api/window';

type EditorWindow=Pick<Window,'innerSize'|'outerPosition'|'isMaximized'|'isFullscreen'|'setFullscreen'|'unmaximize'|'maximize'|'setSize'|'setPosition'>;
/** Keep the host window geometry from before entering the editor. */
export function captureEditorWindow(window:EditorWindow) {
 const original=Promise.all([window.innerSize(),window.outerPosition(),window.isMaximized()]);
 async function restore(){
  const [size,position,maximized]=await original;
  await window.setFullscreen(false);
  await window.unmaximize();
  await window.setSize(size);
  await window.setPosition(position);
  if(maximized)await window.maximize();
 }
 async function toggleFullscreen(){
  await original;
  if(await window.isFullscreen()){await restore();return false;}
  await window.setFullscreen(true);return true;
 }
 return {restore,toggleFullscreen};
}
