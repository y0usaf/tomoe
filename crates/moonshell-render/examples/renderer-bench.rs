use std::hint::black_box;
use std::time::Instant;

use moonshell_render::element::{Flex, Style, Text};
use moonshell_render::{Element, Renderer, Rgba, Scene, SceneDamage};

const WIDTH: u32 = 800;
const HEIGHT: u32 = 32;
const ITERATIONS: usize = 200;

fn benchmark_tree() -> Element {
    let label = |content: &str, width: f32| {
        Element::Text(Text {
            style: Style {
                width: Some(width),
                ..Style::default()
            },
            content: content.to_owned(),
            size: 14.0,
            line_height: Some(18.0),
            color: Rgba::new(0xff, 0xff, 0xff, 0xff),
        })
    };

    Element::HBox(Flex {
        style: Style {
            bg: Some(Rgba::new(0x1e, 0x1e, 0x2e, 0xff)),
            ..Style::default()
        },
        gap: 12.0,
        padding: moonshell_render::element::Edges::all(7.0),
        children: vec![
            label("tomoe", 70.0),
            label("workspace: 1", 120.0),
            Element::Spacer(Default::default()),
            label("cpu 12%", 80.0),
            label("memory 384 MiB", 150.0),
        ],
        ..Flex::default()
    })
}

fn elapsed_ns(start: Instant) -> u128 {
    start.elapsed().as_nanos()
}

fn main() {
    let mut renderer = Renderer::new();
    let mut scene = Scene::new();
    let mut canvas = vec![0; (WIDTH * HEIGHT * 4) as usize];
    let tree = benchmark_tree();

    let cold_start = Instant::now();
    let cold_damage = scene.render(&mut renderer, &mut canvas, WIDTH, HEIGHT, 1.0, &tree);
    let cold_ns = elapsed_ns(cold_start);

    let mut warm_ns = 0u128;
    let mut unchanged = 0usize;
    let mut checksum = 0u64;
    for _ in 0..ITERATIONS {
        let start = Instant::now();
        let damage = scene.render(
            &mut renderer,
            &mut canvas,
            WIDTH,
            HEIGHT,
            1.0,
            black_box(&tree),
        );
        warm_ns += elapsed_ns(start);
        if matches!(damage, SceneDamage::None) {
            unchanged += 1;
        }
        checksum = checksum.wrapping_add(canvas[0] as u64);
    }

    let warm_avg_ns = warm_ns / ITERATIONS as u128;
    println!(
        "{{\"benchmark\":\"renderer_scene\",\"width\":{WIDTH},\"height\":{HEIGHT},\"iterations\":{ITERATIONS},\"cold_ns\":{cold_ns},\"warm_avg_ns\":{warm_avg_ns},\"unchanged\":{unchanged},\"cold_damage\":\"{cold_damage:?}\",\"checksum\":{checksum}}}"
    );

    // Keep the result live even with an aggressive optimizer. The checksum is
    assert!(canvas.iter().any(|&byte| byte != 0));
}
