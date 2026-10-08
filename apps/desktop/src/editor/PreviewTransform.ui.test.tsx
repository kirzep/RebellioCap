import {render,screen,fireEvent} from '@testing-library/react';
import {describe,it,expect,vi,afterEach} from 'vitest';
import {PreviewTransform} from './PreviewTransform';
import {newItem} from './model';

afterEach(()=>vi.restoreAllMocks());
function setup(disabled=false){
 const item={...newItem('image',0,0,5),x:.2,y:.2,width:.4,height:.4};
 const callbacks={onSelect:vi.fn(),onBegin:vi.fn(),onChange:vi.fn(),onEnd:vi.fn()};
 vi.spyOn(HTMLElement.prototype,'getBoundingClientRect').mockReturnValue({width:1000,height:500} as DOMRect);
 render(<div className="ed-screen"><PreviewTransform item={item} selected disabled={disabled} {...callbacks}/></div>);
 return {item,...callbacks};
}
describe('canvas transform keyboard controls',()=>{
 it('offers named corner buttons and keyboard movement as one Undo action',()=>{
  const result=setup();
  expect(screen.getAllByRole('button')).toHaveLength(4);
  const target=screen.getByLabelText('Переместить изображение');
  expect(target).toHaveAttribute('tabindex','0');
  fireEvent.keyDown(target,{key:'ArrowRight',shiftKey:true});
  expect(result.onChange.mock.calls[0][0].x).toBeCloseTo(.21);
  expect(result.onChange.mock.calls[0][0].y).toBe(.2);
  expect(result.onSelect).toHaveBeenCalledOnce();expect(result.onBegin).toHaveBeenCalledOnce();expect(result.onEnd).toHaveBeenCalledOnce();
 });
 it('resizes from a focused corner without also moving the parent',()=>{
  const result=setup();
  fireEvent.keyDown(screen.getByRole('button',{name:'Масштабировать: правый нижний угол'}),{key:'ArrowRight'});
  expect(result.onChange).toHaveBeenCalledOnce();
  expect(result.onChange).toHaveBeenCalledWith(expect.objectContaining({x:.2,y:.2,width:.401,height:.401}));
 });
 it('disables all controls during export',()=>{
  const result=setup(true);
  for(const button of screen.getAllByRole('button'))expect(button).toBeDisabled();
  fireEvent.keyDown(screen.getByLabelText('Переместить изображение'),{key:'ArrowRight'});
  expect(result.onChange).not.toHaveBeenCalled();
 });
});
