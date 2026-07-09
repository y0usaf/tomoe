//! Custom shader programs, compiled once per Gles context (niri shape).
//!
//! [`init`] compiles everything and stashes the [`Shaders`] struct in the
//! EGL context's user data; elements look programs up through
//! [`Shaders::get`]. Compilation failures degrade gracefully — the effect
//! that needs the program simply doesn't render — so a broken driver
//! can't take the compositor down.

use glam::Mat3;
use smithay::backend::renderer::gles::{
    GlesRenderer, GlesTexProgram, Uniform, UniformName, UniformType, UniformValue,
};
use tracing::warn;

use super::renderer::AsGlesRenderer;
use super::shader_element::ShaderProgram;

#[derive(Debug, Clone, Copy)]
pub enum ProgramType {
    Border,
    Shadow,
}

pub struct Shaders {
    /// Clips a window surface to its rounded-corner geometry
    /// (`clipped_surface.frag` overriding the default texture program).
    pub clipped_surface: Option<GlesTexProgram>,
    pub border: Option<ShaderProgram>,
    pub shadow: Option<ShaderProgram>,
}

impl Shaders {
    fn compile(renderer: &mut GlesRenderer) -> Self {
        let clipped_surface = renderer
            .compile_custom_texture_shader(
                concat!(
                    include_str!("shaders/clipped_surface.frag"),
                    include_str!("shaders/rounding_alpha.frag"),
                ),
                &[
                    UniformName::new("geo_size", UniformType::_2f),
                    UniformName::new("corner_radius", UniformType::_4f),
                    UniformName::new("input_to_geo", UniformType::Matrix3x3),
                ],
            )
            .map_err(|err| {
                warn!("error compiling clipped surface shader: {err:?}");
            })
            .ok();

        let border = ShaderProgram::compile(
            renderer,
            concat!(
                include_str!("shaders/border.frag"),
                include_str!("shaders/rounding_alpha.frag")
            ),
            &[
                UniformName::new("color", UniformType::_4f),
                UniformName::new("geo_size", UniformType::_2f),
                UniformName::new("outer_radius", UniformType::_4f),
                UniformName::new("border_width", UniformType::_1f),
            ],
            &[],
        )
        .map_err(|err| warn!("error compiling border shader: {err:?}"))
        .ok();

        let shadow = ShaderProgram::compile(
            renderer,
            include_str!("shaders/shadow.frag"),
            &[
                UniformName::new("color", UniformType::_4f),
                UniformName::new("geo_size", UniformType::_2f),
                UniformName::new("shadow_range", UniformType::_1f),
                UniformName::new("corner_radius", UniformType::_1f),
                UniformName::new("shadow_power", UniformType::_1f),
            ],
            &[],
        )
        .map_err(|err| warn!("error compiling shadow shader: {err:?}"))
        .ok();

        Self {
            clipped_surface,
            border,
            shadow,
        }
    }

    pub fn get_from_frame<'a>(
        frame: &'a mut smithay::backend::renderer::gles::GlesFrame<'_, '_>,
    ) -> Option<&'a Self> {
        frame.egl_context().user_data().get()
    }

    pub fn program(&self, program: ProgramType) -> Option<ShaderProgram> {
        match program {
            ProgramType::Border => self.border.clone(),
            ProgramType::Shadow => self.shadow.clone(),
        }
    }

    /// The shaders for `renderer`'s Gles context, or `None` when [`init`]
    /// never ran for it (effects then silently stay off).
    pub fn get(renderer: &mut impl AsGlesRenderer) -> Option<&Self> {
        let renderer = renderer.as_gles_renderer();
        renderer.egl_context().user_data().get()
    }
}

/// Compile the shader set for `renderer`'s Gles context. Call once when the
/// context comes up (winit init / TTY primary-GPU bring-up); repeat calls
/// are no-ops.
pub fn init(renderer: &mut GlesRenderer) {
    let shaders = Shaders::compile(renderer);
    let data = renderer.egl_context().user_data();
    data.insert_if_missing(|| shaders);
    super::resources::init(renderer);
}

/// A `mat3` uniform from a glam matrix.
pub fn mat3_uniform(name: &str, mat: Mat3) -> Uniform<'_> {
    Uniform::new(
        name,
        UniformValue::Matrix3x3 {
            matrices: vec![mat.to_cols_array()],
            transpose: false,
        },
    )
}
