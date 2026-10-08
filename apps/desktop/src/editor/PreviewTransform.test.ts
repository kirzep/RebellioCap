import {describe,it,expect} from 'vitest';
import {transformItem} from './PreviewTransform';
import {newItem} from './model';
describe('preview transforms',()=>{
 it('moves the full source beyond the frame without changing its size',()=>{const item=newItem('video',0,0,5);const next=transformItem(item,'move',-.5,.2);expect(next.x).toBe(-.5);expect(next.width).toBe(1);expect(item.x).toBe(0);});
 it('shrinks proportionally and keeps the opposite corner anchored',()=>{const item=newItem('video',0,0,5),next=transformItem(item,'nw',.3,.1);expect(next.width).toBeCloseTo(.7);expect(next.height).toBeCloseTo(.7);expect(next.x+next.width).toBe(1);expect(next.y+next.height).toBe(1);});
 it('allows free stretching and scales text size',()=>{const item=newItem('image',0,0,5);expect(transformItem(item,'se',.5,0,true).height).toBe(1);const text=newItem('text',0,0,5);expect(transformItem(text,'se',.5,0).fontSize).toBe(108);});
});
