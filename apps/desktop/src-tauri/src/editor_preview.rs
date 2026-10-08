use std::{io::{Read, Write}, os::windows::fs::MetadataExt, path::{Path, PathBuf}};

type Result<T> = std::result::Result<T, String>;

pub(crate) fn prepare_source_preview(source: &Path, check: impl Fn() -> Result<()>)
    -> Result<(tempfile::TempDir, PathBuf)> {
    check()?;
    let dir = tempfile::Builder::new().prefix("rebcap-editor-source-").tempdir()
        .map_err(|error| error.to_string())?;
    let mut name = std::ffi::OsString::from("source");
    if let Some(extension) = source.extension() {
        name.push(".");
        name.push(extension);
    }
    let preview = dir.path().join(name);
    let metadata = std::fs::symlink_metadata(source).map_err(|error| error.to_string())?;
    // A reparse point is copied through its selected target, never published as
    // another reparse point in the preview directory.
    let can_link = metadata.file_attributes() & 0x400 == 0;
    if !can_link || std::fs::hard_link(source, &preview).is_err() {
        check()?;
        let input = std::fs::File::open(source).map_err(|error| error.to_string())?;
        let output = std::fs::OpenOptions::new().write(true).create_new(true)
            .open(&preview).map_err(|error| error.to_string())?;
        copy_checked(input, output, &check)?;
    }
    check()?;
    Ok((dir, preview))
}

pub(crate) fn copy_checked(mut input: impl Read, mut output: impl Write, check: impl Fn() -> Result<()>) -> Result<()> {
    let mut buffer = [0; 64 * 1024];
    loop {
        check()?;
        let count = input.read(&mut buffer).map_err(|error| error.to_string())?;
        check()?;
        if count == 0 { return Ok(()); }
        output.write_all(&buffer[..count]).map_err(|error| error.to_string())?;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn repeated_original_imports_have_independent_revocable_paths() {
        let source_dir = tempfile::tempdir().unwrap();
        let source = source_dir.path().join("пример.wav");
        std::fs::write(&source, b"original audio").unwrap();
        let (first_dir, first) = prepare_source_preview(&source, || Ok(())).unwrap();
        let (second_dir, second) = prepare_source_preview(&source, || Ok(())).unwrap();
        assert_ne!(first, source);
        assert_ne!(second, source);
        assert_ne!(first, second);
        assert_eq!(first.parent(), Some(first_dir.path()));
        assert_eq!(second.parent(), Some(second_dir.path()));
        assert_eq!(first.extension(), source.extension());
        assert_eq!(std::fs::read(&first).unwrap(), b"original audio");
        drop(first_dir);
        assert!(!first.exists());
        assert_eq!(std::fs::read(&second).unwrap(), b"original audio");
        drop(second_dir);
        assert_eq!(std::fs::read(source).unwrap(), b"original audio");
    }

    #[test]
    fn cancellation_before_preparation_does_not_open_the_source() {
        let result = prepare_source_preview(Path::new("missing.png"), || Err("editor.operation_cancelled".into()));
        assert_eq!(result.unwrap_err(), "editor.operation_cancelled");
    }

    #[test]
    fn fallback_copy_preserves_multiple_chunks_without_reencoding() {
        let bytes = vec![42; 150_000];
        let mut copied = Vec::new();
        copy_checked(std::io::Cursor::new(&bytes), &mut copied, || Ok(())).unwrap();
        assert_eq!(copied, bytes);
    }

    #[test]
    fn fallback_copy_checks_cancellation_after_a_blocking_read() {
        let checks = std::cell::Cell::new(0);
        let mut copied = Vec::new();
        let result = copy_checked(std::io::Cursor::new(vec![42; 150_000]), &mut copied, || {
            checks.set(checks.get() + 1);
            if checks.get() >= 2 { Err("editor.operation_cancelled".into()) } else { Ok(()) }
        });
        assert_eq!(result.unwrap_err(), "editor.operation_cancelled");
        assert!(copied.is_empty());
    }

    #[test]
    fn cancellation_after_alias_creation_preserves_the_original() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("original.png");
        std::fs::write(&source, b"image").unwrap();
        let checks = std::cell::Cell::new(0);
        let result = prepare_source_preview(&source, || {
            checks.set(checks.get() + 1);
            if checks.get() == 1 { Ok(()) } else { Err("editor.operation_cancelled".into()) }
        });
        assert_eq!(result.unwrap_err(), "editor.operation_cancelled");
        assert_eq!(std::fs::read(source).unwrap(), b"image");
    }
}
