import {describe,it,expect} from 'vitest';
import {PhysicalPosition,PhysicalSize} from '@tauri-apps/api/dpi';
import {captureEditorWindow} from './editorWindow';
function fixture(maximized=false){
 let size=new PhysicalSize(1120,780),position=new PhysicalPosition(120,80),fullscreen=false,max=maximized;
 const calls:string[]=[];
 const window={innerSize:async()=>size,outerPosition:async()=>position,isMaximized:async()=>max,isFullscreen:async()=>fullscreen,setFullscreen:async(value:boolean)=>{calls.push('fullscreen');fullscreen=value;size=new PhysicalSize(1920,1080);max=true;},unmaximize:async()=>{calls.push('unmaximize');max=false;},maximize:async()=>{calls.push('maximize');max=true;},setSize:async(value:unknown)=>{calls.push('size');size=value as PhysicalSize;},setPosition:async(value:unknown)=>{calls.push('position');position=value as PhysicalPosition;}};
 return {window,calls,state:()=>({size,position,fullscreen,max})};
}
describe('editor window restoration',()=>{
 it('restores original size after leaving fullscreen even if the OS maximized it',async()=>{const f=fixture(),session=captureEditorWindow(f.window);expect(await session.toggleFullscreen()).toBe(true);expect(await session.toggleFullscreen()).toBe(false);expect(f.state()).toMatchObject({size:{width:1120,height:780},position:{x:120,y:80},fullscreen:false,max:false});expect(f.calls.slice(-4)).toEqual(['fullscreen','unmaximize','size','position']);});
 it('restores when exiting directly from fullscreen',async()=>{const f=fixture(),session=captureEditorWindow(f.window);await session.toggleFullscreen();await session.restore();expect(f.state().size.width).toBe(1120);expect(f.state().fullscreen).toBe(false);});
 it('preserves a previously maximized host window',async()=>{const f=fixture(true),session=captureEditorWindow(f.window);await session.toggleFullscreen();await session.restore();expect(f.state().max).toBe(true);});
});
