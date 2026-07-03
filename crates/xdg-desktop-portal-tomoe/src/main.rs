//! xdg-desktop-portal-tomoe — tomoe's own ScreenCast portal backend.
//!
//! Exists because xdg-desktop-portal-wlr lost `PW_STREAM_FLAG_DRIVER` and its
//! screencast stream ends up paced by the audio graph quantum (~30 fps); see
//! `ref/ShojiWM/knowledges/screencast-30fps-xdpw-bug.md`. This backend keeps
//! the stream as the PipeWire graph driver and paces it off the compositor's
//! wlr-screencopy `ready` events, i.e. at output refresh.

mod outputs;
mod pipewire_stream;
mod screencast;
mod toplevel_stream;
mod toplevels;

use tracing_subscriber::EnvFilter;

const BUS_NAME: &str = "org.freedesktop.impl.portal.desktop.tomoe";

#[tokio::main(worker_threads = 2)]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .init();

    let connection = zbus::connection::Builder::session()?
        .name(BUS_NAME)?
        .serve_at(
            "/org/freedesktop/portal/desktop",
            screencast::ScreenCast::new(),
        )?
        .build()
        .await?;
    tracing::info!(bus = BUS_NAME, "claimed D-Bus name; serving ScreenCast");

    tokio::signal::ctrl_c().await?;
    tracing::info!("shutting down (ctrl-c)");
    drop(connection);
    Ok(())
}
