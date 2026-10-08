use crate::engine::Result;
use std::path::Path;
use windows::{core::HSTRING, Win32::{Foundation::SIZE, UI::Shell::{SHCreateItemFromParsingName, IShellItemImageFactory, SIIGBF_THUMBNAILONLY}}};
use windows_sys::Win32::Graphics::Gdi::*;

/// Windows' video provider supplies a frame, never a generic file icon.
pub fn extract(path: &Path) -> Result<Vec<u8>> {
    let _apartment = super::Apartment::new(false)?;
    unsafe {
        let item: IShellItemImageFactory = SHCreateItemFromParsingName(&HSTRING::from(path.to_string_lossy().as_ref()), None)
            .map_err(|e| e.to_string())?;
        let bitmap = item.GetImage(SIZE { cx: 320, cy: 180 }, SIIGBF_THUMBNAILONLY)
            .map_err(|e| e.to_string())?;
        let handle = bitmap.0;
        let result = encode_bitmap(handle);
        DeleteObject(handle);
        result
    }
}

unsafe fn encode_bitmap(bitmap: HBITMAP) -> Result<Vec<u8>> {
    let mut dimensions: BITMAP = std::mem::zeroed();
    if GetObjectW(bitmap, std::mem::size_of::<BITMAP>() as i32, (&mut dimensions as *mut BITMAP).cast()) == 0 {
        return Err("clips.thumbnail_unavailable".into());
    }
    let (width, height) = (dimensions.bmWidth, dimensions.bmHeight.abs());
    if width <= 0 || height <= 0 || width > 1024 || height > 1024 { return Err("clips.thumbnail_invalid_size".into()); }
    let stride = ((width as usize * 3) + 3) & !3;
    let mut pixels = vec![0u8; stride * height as usize];
    let mut info: BITMAPINFO = std::mem::zeroed();
    info.bmiHeader = BITMAPINFOHEADER {
        biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
        biWidth: width, biHeight: height, biPlanes: 1, biBitCount: 24,
        biCompression: BI_RGB, ..std::mem::zeroed()
    };
    let dc = CreateCompatibleDC(std::ptr::null_mut());
    if dc.is_null() { return Err("clips.thumbnail_unavailable".into()); }
    let rows = GetDIBits(dc, bitmap, 0, height as u32, pixels.as_mut_ptr().cast(), &mut info, DIB_RGB_COLORS);
    DeleteDC(dc);
    if rows != height { return Err("clips.thumbnail_unavailable".into()); }
    let mut bytes = Vec::with_capacity(54 + pixels.len());
    bytes.extend_from_slice(b"BM");
    bytes.extend_from_slice(&((54 + pixels.len()) as u32).to_le_bytes());
    bytes.extend_from_slice(&[0; 4]);
    bytes.extend_from_slice(&54u32.to_le_bytes());
    bytes.extend_from_slice(&40u32.to_le_bytes());
    bytes.extend_from_slice(&width.to_le_bytes());
    bytes.extend_from_slice(&height.to_le_bytes());
    bytes.extend_from_slice(&1u16.to_le_bytes());
    bytes.extend_from_slice(&24u16.to_le_bytes());
    bytes.extend_from_slice(&[0; 24]);
    bytes.extend_from_slice(&pixels);
    Ok(bytes)
}
