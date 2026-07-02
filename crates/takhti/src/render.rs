//! Render element construction, all in integer physical coordinates.
//!
//! Every element is positioned at `Point<i32, Physical>` so client buffers
//! are sampled 1:1 on the pixel grid — the whole point of the physical-first
//! coordinate space. smithay's `space_render_elements` is not used: it
//! positions from integer *logical* locations and rounds each element
//! independently, which drifts decorations a pixel away from windows at
//! fractional scales.

use smithay::backend::renderer::element::memory::MemoryRenderBufferRenderElement;
use smithay::backend::renderer::element::solid::SolidColorRenderElement;
use smithay::backend::renderer::element::surface::WaylandSurfaceRenderElement;
use smithay::backend::renderer::element::{AsRenderElements, Kind};
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::desktop::layer_map_for_output;
use smithay::output::Output;
use smithay::render_elements;
use smithay::utils::{Physical, Point, Scale};
use smithay::wayland::shell::wlr_layer::Layer as WlrLayer;

use crate::coords;
use crate::space::PhysicalSpace;
use crate::state::Takhti;

render_elements! {
    pub OutputRenderElements<=GlesRenderer>;
    Solid=SolidColorRenderElement,
    Memory=MemoryRenderBufferRenderElement<GlesRenderer>,
    Surface=WaylandSurfaceRenderElement<GlesRenderer>,
}

/// Border frame slabs drawn around windows (buffers are persisted in `Takhti`
/// so the damage tracker sees stable element ids). Four slabs per window so
/// nothing is drawn behind the window itself — transparent surfaces would
/// otherwise show the border color across their whole area.
///
/// Widths and positions are physical pixels; the solid buffers are sized in
/// physical too, so they render with scale 1.0 (buffer units == pixels) and
/// can never land off-grid.
pub fn border_elements(takhti: &Takhti, output_loc: Point<i32, Physical>) -> Vec<OutputRenderElements> {
    let width = takhti.lua.settings().border_width;
    if width <= 0 {
        return Vec::new();
    }
    let mut elements = Vec::new();
    for window in takhti.space.elements() {
        let Some(buffers) = takhti.border_buffers.get(window) else {
            continue;
        };
        let Some(geo) = takhti.space.element_geometry(window) else {
            continue;
        };
        let loc = geo.loc - output_loc;
        let offsets: [Point<i32, Physical>; 4] = [
            Point::from((-width, -width)),          // top
            Point::from((-width, geo.size.h)),      // bottom
            Point::from((-width, 0)),               // left
            Point::from((geo.size.w, 0)),           // right
        ];
        for (buffer, offset) in buffers.iter().zip(offsets) {
            elements.push(OutputRenderElements::Solid(SolidColorRenderElement::from_buffer(
                buffer,
                loc + offset,
                1.0,
                1.0,
                Kind::Unspecified,
            )));
        }
    }
    elements
}

/// Build the full scene for one output (everything except the cursor), in
/// render order: earlier elements draw on top. `ui` and `borders` are built
/// by the caller because they need parts of `Takhti` this borrow can't reach.
pub fn scene_elements(
    renderer: &mut GlesRenderer,
    space: &PhysicalSpace,
    output: &Output,
    ui: Vec<OutputRenderElements>,
    borders: Vec<OutputRenderElements>,
) -> Vec<OutputRenderElements> {
    let scale = space.scale();
    let render_scale = Scale::from(scale);
    let Some(output_geo) = space.output_geometry(output) else {
        return ui;
    };

    let layer_elements = |renderer: &mut GlesRenderer, kinds: [WlrLayer; 2]| {
        let layers = layer_map_for_output(output);
        let mut elements = Vec::new();
        for kind in kinds {
            // Top → bottom within each layer kind.
            for layer in layers.layers_on(kind).rev() {
                let Some(geo) = layers.layer_geometry(layer) else {
                    continue;
                };
                let loc = coords::logical_point_to_physical(geo.loc.to_f64(), scale);
                elements.extend(layer.render_elements(renderer, loc, render_scale, 1.0));
            }
        }
        elements
    };

    let mut elements = ui;
    elements.extend(layer_elements(renderer, [WlrLayer::Overlay, WlrLayer::Top]));

    // Windows top → bottom. The stored location is the geometry origin; the
    // buffer origin shifts by the client's (logical) geometry offset, rounded
    // once onto the grid.
    for window in space.elements().rev() {
        let Some(geo) = space.element_geometry(window) else {
            continue;
        };
        if output_geo.intersection(geo).is_none() {
            continue;
        }
        let buffer_offset =
            coords::logical_point_to_physical(window.geometry().loc.to_f64(), scale);
        let loc = geo.loc - buffer_offset - output_geo.loc;
        elements.extend(window.render_elements::<OutputRenderElements>(
            renderer,
            loc,
            render_scale,
            1.0,
        ));
    }

    // Borders render below windows (visible as a ring around each).
    elements.extend(borders);
    elements.extend(layer_elements(renderer, [WlrLayer::Bottom, WlrLayer::Background]));
    elements
}
