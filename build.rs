fn main() {
    // Node-API symbols are resolved dynamically. napi-build's GNU Windows helper
    // expects libnode.dll, which is unnecessary here (and unavailable for cross-checks).
    if !(std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows")
        && std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("gnu"))
    {
        napi_build::setup();
    }
}
