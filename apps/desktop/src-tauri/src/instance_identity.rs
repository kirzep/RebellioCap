use std::path::Path;
use sha2::{Digest, Sha256};

pub(crate) fn identifier(base: &str, test_root: Option<&Path>, allow_debug: bool) -> Result<String, String> {
    let Some(root) = test_root else { return Ok(base.to_owned()); };
    let canonical = crate::config::resolve_config_root(root, Some(root), allow_debug)
        .map_err(|error| error.to_string())?;
    let key = canonical.to_string_lossy().to_lowercase();
    Ok(format!("{base}.test.{:x}", Sha256::digest(key.as_bytes())))
}

#[cfg(test)]
mod tests {
    use super::identifier;
    #[cfg(debug_assertions)]
    #[test]
    fn debug_roots_isolate_instances_but_aliases_share_the_same_mutex() {
        let first = tempfile::tempdir().unwrap();
        let second = tempfile::tempdir().unwrap();
        let a = identifier("com.rebelliocap.desktop", Some(first.path()), true).unwrap();
        let b = identifier("com.rebelliocap.desktop", Some(second.path()), true).unwrap();
        assert_ne!(a, "com.rebelliocap.desktop");
        assert_ne!(a, b);
        assert_eq!(a, identifier("com.rebelliocap.desktop", Some(&first.path().join(".")), true).unwrap());
    }
    #[test]
    fn ordinary_launch_preserves_identifier_and_release_override_is_rejected() {
        assert_eq!(identifier("com.rebelliocap.desktop", None, true).unwrap(), "com.rebelliocap.desktop");
        let root = tempfile::tempdir().unwrap();
        assert!(identifier("com.rebelliocap.desktop", Some(root.path()), false).is_err());
        assert!(identifier("com.rebelliocap.desktop", Some(Path::new("relative-test-root")), true).is_err());
    }
    use std::path::Path;
}
