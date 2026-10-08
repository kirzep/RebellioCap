import {it,expect} from 'vitest';
import {installBrowserUiPolicy} from './browserUiPolicy';
it('blocks browser menus and devtools shortcuts without blocking custom menus',()=>{
 const remove=installBrowserUiPolicy();let opened=false;
 const element=document.createElement('button');document.body.append(element);element.addEventListener('contextmenu',()=>{opened=true;});
 const menu=new MouseEvent('contextmenu',{bubbles:true,cancelable:true});element.dispatchEvent(menu);expect(menu.defaultPrevented).toBe(true);expect(opened).toBe(true);
 for(const event of [new KeyboardEvent('keydown',{key:'F12',cancelable:true}),new KeyboardEvent('keydown',{key:'I',ctrlKey:true,shiftKey:true,cancelable:true})]){window.dispatchEvent(event);expect(event.defaultPrevented).toBe(true);}
 remove();element.remove();
});
