//! V13 Configurator core.
//!
//! The Rust side owns the serial port exclusively and is the only place that speaks the wire
//! protocol. The frontend receives typed data and never parses a line of serial text, so the
//! feature's highest-consequence failure (misreading a motor's electrical parameters) lives in one
//! unit-tested pure function.
//!
//! Contracts: specs/003-v13-configurator/contracts/serial-protocol.md
//!            specs/003-v13-configurator/contracts/app-ipc.md

pub mod commands;
pub mod protocol;
pub mod session;
pub mod storage;

use commands::AppState;

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_serialplugin::init())
        .manage(AppState::default())
        .invoke_handler(tauri::generate_handler![
            commands::list_ports,
            commands::connect,
            commands::disconnect,
            commands::abort,
            commands::read_all,
            commands::set_telemetry,
            commands::select_motor,
            commands::arm,
            commands::disarm,
            commands::apply_impedance,
            commands::calibrate_start,
            commands::calibrate_accept,
            commands::calibrate_reject,
            commands::write_setting,
            commands::change_log_read,
            commands::confirm_intent,
            commands::operator_get,
            commands::operator_set,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the V13 Configurator");
}
