use std::collections::HashMap;
use std::rc::Rc;

use smithay::backend::renderer::element::{Element, Id, Kind, RenderElement, UnderlyingStorage};
use smithay::backend::renderer::gles::{GlesError, GlesFrame, GlesRenderer, Uniform};
use smithay::backend::renderer::utils::{CommitCounter, DamageSet, OpaqueRegions};
use smithay::utils::user_data::UserDataMap;
use smithay::utils::{Buffer, Physical, Point, Rectangle, Scale, Size, Transform};

use super::shader_element::ShaderRenderElement;
use super::shaders::ProgramType;
use crate::backend::tty::{TtyFrame, TtyRenderer, TtyRendererError};

#[derive(Debug, Clone, Copy, PartialEq)]
struct Parameters {
    window_size: Size<i32, Physical>,
    color: [f32; 4],
    range: i32,
    radius: i32,
    power: f32,
    alpha: f32,
}

/// One persistent textureless rounded drop shadow, in physical pixels.
#[derive(Debug, Clone)]
pub struct ShadowRenderElement {
    inner: ShaderRenderElement,
    params: Parameters,
}

impl Default for ShadowRenderElement {
    fn default() -> Self {
        Self::new((0, 0).into(), [0.; 4], 0, 0, 3.)
    }
}

impl ShadowRenderElement {
    pub fn new(
        window_size: Size<i32, Physical>,
        color: [f32; 4],
        range: i32,
        radius: i32,
        power: f32,
    ) -> Self {
        let params = Parameters {
            window_size,
            color,
            range,
            radius,
            power,
            alpha: 1.,
        };
        let inner = ShaderRenderElement::empty(ProgramType::Shadow, Kind::Unspecified);
        let mut element = Self { inner, params };
        element.update_inner();
        element
    }

    pub fn update(
        &mut self,
        window_size: Size<i32, Physical>,
        color: [f32; 4],
        range: i32,
        radius: i32,
        power: f32,
    ) {
        let params = Parameters {
            window_size,
            color,
            range,
            radius,
            power,
            alpha: self.params.alpha,
        };
        if params != self.params {
            self.params = params;
            self.update_inner();
        }
    }

    pub fn range(&self) -> i32 {
        self.params.range
    }

    pub fn set_alpha(&mut self, alpha: f32) {
        if self.params.alpha != alpha {
            self.params.alpha = alpha;
            self.update_inner();
        }
    }

    pub fn with_location(mut self, location: Point<i32, Physical>) -> Self {
        self.inner = self
            .inner
            .with_location(Point::from((location.x as f64, location.y as f64)));
        self
    }

    fn update_inner(&mut self) {
        let range = self.params.range.max(0);
        let full_size: Size<i32, Physical> = (
            self.params.window_size.w + range * 2,
            self.params.window_size.h + range * 2,
        )
            .into();
        let size: Size<f64, smithay::utils::Logical> =
            (full_size.w.max(0) as f64, full_size.h.max(0) as f64).into();
        self.inner.update(
            size,
            None,
            1.,
            self.params.alpha,
            Rc::new([
                Uniform::new("color", self.params.color),
                Uniform::new("geo_size", [size.w as f32, size.h as f32]),
                Uniform::new("shadow_range", range as f32),
                Uniform::new("corner_radius", self.params.radius.max(0) as f32),
                Uniform::new("shadow_power", self.params.power.clamp(1., 4.)),
            ]),
            HashMap::new(),
        );
    }
}

impl Element for ShadowRenderElement {
    fn id(&self) -> &Id {
        self.inner.id()
    }
    fn current_commit(&self) -> CommitCounter {
        self.inner.current_commit()
    }
    fn geometry(&self, _: Scale<f64>) -> Rectangle<i32, Physical> {
        self.inner.geometry(Scale::from(1.))
    }
    fn transform(&self) -> Transform {
        self.inner.transform()
    }
    fn src(&self) -> Rectangle<f64, Buffer> {
        self.inner.src()
    }
    fn damage_since(
        &self,
        _: Scale<f64>,
        commit: Option<CommitCounter>,
    ) -> DamageSet<i32, Physical> {
        self.inner.damage_since(Scale::from(1.), commit)
    }
    fn opaque_regions(&self, _: Scale<f64>) -> OpaqueRegions<i32, Physical> {
        OpaqueRegions::default()
    }
    fn alpha(&self) -> f32 {
        self.inner.alpha()
    }
    fn kind(&self) -> Kind {
        self.inner.kind()
    }
}

impl RenderElement<GlesRenderer> for ShadowRenderElement {
    fn draw(
        &self,
        frame: &mut GlesFrame<'_, '_>,
        src: Rectangle<f64, Buffer>,
        dst: Rectangle<i32, Physical>,
        damage: &[Rectangle<i32, Physical>],
        opaque: &[Rectangle<i32, Physical>],
        cache: Option<&UserDataMap>,
    ) -> Result<(), GlesError> {
        RenderElement::<GlesRenderer>::draw(&self.inner, frame, src, dst, damage, opaque, cache)
    }
    fn underlying_storage(&self, renderer: &mut GlesRenderer) -> Option<UnderlyingStorage<'_>> {
        self.inner.underlying_storage(renderer)
    }
}

impl<'render> RenderElement<TtyRenderer<'render>> for ShadowRenderElement {
    fn draw(
        &self,
        frame: &mut TtyFrame<'render, '_, '_>,
        src: Rectangle<f64, Buffer>,
        dst: Rectangle<i32, Physical>,
        damage: &[Rectangle<i32, Physical>],
        opaque: &[Rectangle<i32, Physical>],
        cache: Option<&UserDataMap>,
    ) -> Result<(), TtyRendererError<'render>> {
        RenderElement::<TtyRenderer<'render>>::draw(
            &self.inner,
            frame,
            src,
            dst,
            damage,
            opaque,
            cache,
        )
    }
    fn underlying_storage(
        &self,
        renderer: &mut TtyRenderer<'render>,
    ) -> Option<UnderlyingStorage<'_>> {
        self.inner.underlying_storage(renderer)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn geometry_includes_falloff_on_every_side() {
        let shadow = ShadowRenderElement::new((100, 50).into(), [0.; 4], 12, 8, 3.);
        assert_eq!(shadow.geometry(Scale::from(1.)).size, (124, 74).into());
    }

    #[test]
    fn unchanged_update_preserves_commit() {
        let mut shadow = ShadowRenderElement::new((100, 50).into(), [0., 0., 0., 0.8], 12, 8, 3.);
        let id = shadow.id().clone();
        let commit = shadow.current_commit();
        shadow.update((100, 50).into(), [0., 0., 0., 0.8], 12, 8, 3.);
        assert_eq!(shadow.id(), &id);
        assert_eq!(shadow.current_commit(), commit);
    }
}
