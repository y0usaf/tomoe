pub mod tty;
pub mod winit;

use smithay::backend::renderer::gles::GlesRenderer;

pub use tty::TtyData;
pub use winit::WinitData;

/// Active backend: winit (nested dev window) or TTY (DRM/GBM real hardware).
pub enum Backend {
    Uninit,
    Winit(WinitData),
    Tty(TtyData),
}

impl Backend {
    pub fn winit(&mut self) -> &mut WinitData {
        match self {
            Backend::Winit(data) => data,
            _ => panic!("winit backend not initialized"),
        }
    }

    #[allow(dead_code)]
    pub fn tty(&mut self) -> &mut TtyData {
        match self {
            Backend::Tty(data) => data,
            _ => panic!("tty backend not initialized"),
        }
    }

    pub fn renderer(&mut self) -> Option<&mut GlesRenderer> {
        match self {
            Backend::Uninit => None,
            Backend::Winit(data) => Some(data.backend.renderer()),
            Backend::Tty(data) => Some(&mut data.renderer),
        }
    }
}
