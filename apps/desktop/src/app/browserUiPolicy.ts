/** Suppress browser chrome while allowing the app's own context menus. */
export function installBrowserUiPolicy(){
 const context=(event:MouseEvent)=>event.preventDefault();
 const key=(event:KeyboardEvent)=>{
  if(event.key==='F12'||((event.ctrlKey||event.metaKey)&&event.shiftKey&&['i','j','c'].includes(event.key.toLowerCase()))){
   event.preventDefault();event.stopImmediatePropagation();
  }
 };
 window.addEventListener('contextmenu',context,true);
 window.addEventListener('keydown',key,true);
 return ()=>{window.removeEventListener('contextmenu',context,true);window.removeEventListener('keydown',key,true);};
}
