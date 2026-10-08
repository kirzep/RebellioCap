use std::sync::OnceLock;
use serde_json::Value;

pub fn recording_contract() -> &'static Value {
    static VALUE: OnceLock<Value> = OnceLock::new();
    VALUE.get_or_init(|| {
        let value: Value = serde_json::from_str(include_str!("../../../../../contracts/recording-settings.v1.json")).expect("Embedded recording contract");
        assert_eq!(value["version"], 1);
        value
    })
}

pub fn recording_range(field: &str) -> (u32, u32) {
    let range = &recording_contract()["ranges"][field];
    (range["min"].as_u64().unwrap().try_into().unwrap(), range["max"].as_u64().unwrap().try_into().unwrap())
}

pub fn invalid_field_message(field: &str) -> String {
    recording_contract()["messages"][field].as_str().map(str::to_owned)
        .unwrap_or_else(|| format!("invalid configuration field: {field}"))
}
