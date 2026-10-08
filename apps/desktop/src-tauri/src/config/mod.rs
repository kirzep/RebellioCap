mod model;
mod contract;
pub use contract::{recording_contract, recording_range};
mod store;
mod validation;

pub use model::*;
pub use store::{resolve_config_root, ConfigStore};
pub use validation::{validate_active, validate_draft};

#[derive(Debug, thiserror::Error)]
pub enum ConfigError {
    #[error("configuration I/O failed: {0}")]
    Io(#[from] std::io::Error),
    #[error("configuration JSON is invalid: {0}")]
    Json(#[from] serde_json::Error),
    #[error("{}", contract::invalid_field_message(.0))]
    Invalid(&'static str),
    #[error("unsupported configuration schema: {0}")]
    Schema(u32),
    #[error("successful recording test and explicit completion are required")]
    CompletionRequired,
    #[error("test configuration override is prohibited in release builds")]
    ReleaseOverride,
}

pub type Result<T> = std::result::Result<T, ConfigError>;
