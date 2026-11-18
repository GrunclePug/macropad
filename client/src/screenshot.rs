//! `--screenshot <dir>`: run the editor against the pretend pad and render
//! each tab offscreen (wgpu, so lavapipe works without a GPU) into PNGs.

use std::path::Path;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use eframe::egui;
use egui_wgpu::wgpu;

use crate::app::{App, TABS};
use crate::fake::FakePorts;
use crate::worker::Worker;

const W: u32 = 860;
const H: u32 = 660;
const FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8UnormSrgb;

struct Gpu {
    device: wgpu::Device,
    queue: wgpu::Queue,
    renderer: egui_wgpu::Renderer,
}

pub fn run(dir: &Path) -> Result<()> {
    std::fs::create_dir_all(dir).with_context(|| format!("create {}", dir.display()))?;
    let mut gpu = gpu()?;
    let ctx = egui::Context::default();
    let mut app = App::new(
        Worker::spawn(FakePorts(crate::fake_pad()?), || {}),
        crate::fake_ports,
    );
    let input = || egui::RawInput {
        screen_rect: Some(egui::Rect::from_min_size(
            egui::Pos2::ZERO,
            egui::vec2(W as f32, H as f32),
        )),
        ..Default::default()
    };

    let shot = |gpu: &mut Gpu, app: &mut App, name: &str| -> Result<()> {
        // A few frames, so collapsing headers and combo widths settle.
        let mut out = None;
        for _ in 0..4 {
            out = Some(frame(gpu, &ctx, input(), app));
        }
        let out = out.context("a frame")?;
        let path = dir.join(format!("{name}.png"));
        render(gpu, &ctx, out, &path)?;
        println!("{}", path.display());
        Ok(())
    };

    shot(&mut gpu, &mut app, "0-start")?;
    app.connect_first();
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        frame(&mut gpu, &ctx, input(), &mut app);
        if !app.poll() && app.connected() {
            break;
        }
        if Instant::now() > deadline {
            bail!("the pretend pad didn't connect");
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    for (i, (tab, name)) in TABS.iter().enumerate() {
        app.tab = *tab;
        shot(
            &mut gpu,
            &mut app,
            &format!("{}-{}", i + 1, name.to_lowercase().replace(' ', "-")),
        )?;
    }
    Ok(())
}

fn gpu() -> Result<Gpu> {
    let instance =
        wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    let adapter =
        pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions::default()))
            .context("no GPU adapter (install a Vulkan driver, or lavapipe from vulkan-swrast)")?;
    log::info!("adapter: {:?}", adapter.get_info());
    let (device, queue) =
        pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default()))?;
    let renderer = egui_wgpu::Renderer::new(&device, FORMAT, egui_wgpu::RendererOptions::default());
    Ok(Gpu {
        device,
        queue,
        renderer,
    })
}

/// One frame, with its texture changes applied (egui insists on it).
fn frame(
    gpu: &mut Gpu,
    ctx: &egui::Context,
    input: egui::RawInput,
    app: &mut App,
) -> egui::FullOutput {
    let mut out = ctx.run_ui(input, |ui| app.show(ui));
    for (id, delta) in &out.textures_delta.set {
        for d in delta {
            gpu.renderer.update_texture(&gpu.device, &gpu.queue, *id, d);
        }
    }
    for id in &out.textures_delta.free {
        gpu.renderer.free_texture(id);
    }
    out.textures_delta.clear();
    out
}

fn render(gpu: &mut Gpu, ctx: &egui::Context, out: egui::FullOutput, path: &Path) -> Result<()> {
    let Gpu {
        device,
        queue,
        renderer,
    } = gpu;
    let jobs = ctx.tessellate(out.shapes, out.pixels_per_point);
    let screen = egui_wgpu::ScreenDescriptor {
        size_in_pixels: [W, H],
        pixels_per_point: out.pixels_per_point,
    };
    let size = wgpu::Extent3d {
        width: W,
        height: H,
        depth_or_array_layers: 1,
    };
    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("screenshot"),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: FORMAT,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
    let row =
        (W * 4).div_ceil(wgpu::COPY_BYTES_PER_ROW_ALIGNMENT) * wgpu::COPY_BYTES_PER_ROW_ALIGNMENT;
    let buffer = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("readback"),
        size: u64::from(row * H),
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
    let mut cmds = renderer.update_buffers(device, queue, &mut encoder, &jobs, &screen);
    {
        let bg = ctx
            .global_style()
            .visuals
            .panel_fill
            .to_normalized_gamma_f32();
        let mut pass = encoder
            .begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("egui"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color {
                            r: f64::from(bg[0]),
                            g: f64::from(bg[1]),
                            b: f64::from(bg[2]),
                            a: 1.0,
                        }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                ..Default::default()
            })
            .forget_lifetime();
        renderer.render(&mut pass, &jobs, &screen);
    }
    encoder.copy_texture_to_buffer(
        wgpu::TexelCopyTextureInfo {
            texture: &texture,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        wgpu::TexelCopyBufferInfo {
            buffer: &buffer,
            layout: wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(row),
                rows_per_image: Some(H),
            },
        },
        size,
    );
    cmds.push(encoder.finish());
    queue.submit(cmds);
    buffer.map_async(wgpu::MapMode::Read, .., |r| {
        if let Err(e) = r {
            log::error!("map: {e}");
        }
    });
    device.poll(wgpu::PollType::wait_indefinitely())?;
    let mapped = buffer.get_mapped_range(..)?;
    let mut pixels = Vec::with_capacity(usize::try_from(W * H * 4)?);
    for y in 0..usize::try_from(H)? {
        let start = y * usize::try_from(row)?;
        pixels.extend_from_slice(&mapped[start..start + usize::try_from(W * 4)?]);
    }
    drop(mapped);
    buffer.unmap();

    let file = std::fs::File::create(path).with_context(|| format!("create {}", path.display()))?;
    let mut enc = png::Encoder::new(std::io::BufWriter::new(file), W, H);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    enc.write_header()?.write_image_data(&pixels)?;
    Ok(())
}
