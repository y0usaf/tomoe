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
    size: Size<i32, Physical>,
    color: [f32; 4],
    width: i32,
    radius: i32,
    alpha: f32,
}

/// One persistent textureless rounded ring, in physical pixels.
#[derive(Debug, Clone)]
pub struct BorderRenderElement {
    inner: ShaderRenderElement,
    params: Parameters,
}

impl Default for BorderRenderElement {
    fn default() -> Self {
        Self::new((0, 0).into(), [0.; 4], 0, 0)
    }
}

impl BorderRenderElement {
    pub fn new(size: Size<i32, Physical>, color: [f32; 4], width: i32, radius: i32) -> Self {
        let params = Parameters {
            size,
            color,
            width,
            radius,
            alpha: 1.,
        };
        let inner = ShaderRenderElement::empty(ProgramType::Border, Kind::Unspecified);
        let mut element = Self { inner, params };
        element.update_inner();
        element
    }

    pub fn update(&mut self, size: Size<i32, Physical>, color: [f32; 4], width: i32, radius: i32) {
        let params = Parameters {
            size,
            color,
            width,
            radius,
            alpha: self.params.alpha,
        };
        if params != self.params {
            self.params = params;
            self.update_inner();
        }
    }
    pub fn width(&self) -> i32 {
        self.params.width
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
        let size: Size<f64, smithay::utils::Logical> =
            (self.params.size.w as f64, self.params.size.h as f64).into();
        let radius = self.params.radius.max(0) as f32;
        self.inner.update(
            size,
            None,
            1.,
            self.params.alpha,
            Rc::new([
                Uniform::new("color", self.params.color),
                Uniform::new("geo_size", [size.w as f32, size.h as f32]),
                Uniform::new("outer_radius", [radius; 4]),
                Uniform::new("border_width", self.params.width.max(0) as f32),
            ]),
            HashMap::new(),
        );
    }
}

impl Element for BorderRenderElement {
    fn id(&self) -> &Id {
        self.inner.id()
    }
    fn current_commit(&self) -> CommitCounter {
        self.inner.current_commit()
    }
    fn geometry(&self, _: Scale<f64>) -> Rectangle<i32, Physical> {
        Rectangle::new(self.inner.geometry(Scale::from(1.)).loc, self.params.size)
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

impl RenderElement<GlesRenderer> for BorderRenderElement {
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

impl<'render> RenderElement<TtyRenderer<'render>> for BorderRenderElement {
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
    fn outer_geometry_adds_width_on_every_side() {
        let window: Size<i32, Physical> = (100, 50).into();
        let width = 3;
        let outer: Size<i32, Physical> = (window.w + width * 2, window.h + width * 2).into();
        assert_eq!(outer, (106, 56).into());
    }
    #[test]
    fn unchanged_update_preserves_commit() {
        let mut border = BorderRenderElement::new((100, 50).into(), [1., 0., 0., 1.], 2, 8);
        let id = border.id().clone();
        let commit = border.current_commit();
        border.update((100, 50).into(), [1., 0., 0., 1.], 2, 8);
        assert_eq!(border.id(), &id);
        assert_eq!(border.current_commit(), commit);
    }

    #[test]
    fn alpha_change_damages_persistent_element() {
        let mut border = BorderRenderElement::new((100, 50).into(), [1.; 4], 2, 8);
        let commit = border.current_commit();
        border.set_alpha(0.5);
        assert_ne!(border.current_commit(), commit);
    }
}
