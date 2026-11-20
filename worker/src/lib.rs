//! Inversion of a regular bilinear transport map. Distances are in millimetres.
use std::thread;
mod numerics;

#[derive(Clone, Copy, Default)]
struct Point {
    x: f64,
    y: f64,
    u: f64,
    v: f64,
    residual: (f64, f64),
    jacobian: [f64; 4],
}

struct Map<'a> {
    x: &'a [f64],
    y: &'a [f64],
    n: usize,
}

impl Map<'_> {
    fn sample(&self, u: f64, v: f64) -> [(f64, f64, f64); 2] {
        let i = (u as usize).min(self.n - 1);
        let j = (v as usize).min(self.n - 1);
        let u = u - i as f64;
        let v = v - j as f64;
        let stride = self.n + 1;
        let cell = j * stride + i;
        [self.x, self.y].map(|a| {
            let lower = a[cell + 1] - a[cell];
            let upper = a[cell + stride + 1] - a[cell + stride];
            let side = a[cell + stride] - a[cell];
            (
                a[cell] + u * lower + v * (side + u * (upper - lower)),
                lower + v * (upper - lower),
                side + u * (upper - lower),
            )
        })
    }

    fn measure(&self, point: &mut Point) {
        let [(px, xu, xv), (py, yu, yv)] = self.sample(point.u, point.v);
        point.residual = (px - point.x, py - point.y);
        point.jacobian = [xu, xv, yu, yv];
    }

    fn advance(&self, point: &mut Point) -> Result<(), &'static str> {
        let [xu, xv, yu, yv] = point.jacobian;
        let (ex, ey) = point.residual;
        let det = xu * yv - xv * yu;
        if det <= 0. {
            return Err("Transport folded. Reduce contrast or increase the transport resolution.");
        }
        let (du, dv) = ((yv * ex - xv * ey) / det, (xu * ey - yu * ex) / det);
        let n = self.n as f64;
        let old_error = ex * ex + ey * ey + 1e-24;
        let mut scale = 1. / 1.0_f64.max(du.abs().max(dv.abs()) / (n / 8.));
        let (mut a, mut b) = (point.u, point.v);
        for _ in 0..14 {
            a = (point.u - du * scale).clamp(0., n);
            b = (point.v - dv * scale).clamp(0., n);
            let [(x, _, _), (y, _, _)] = self.sample(a, b);
            let dx = x - point.x;
            let dy = y - point.y;
            if dx * dx + dy * dy <= old_error {
                break;
            }
            scale *= 0.5;
        }
        (point.u, point.v) = (a, b);
        self.measure(point);
        Ok(())
    }
}

pub fn forward_transport(
    x: &[f64],
    y: &[f64],
    n: usize,
    grid: usize,
    aperture: f64,
    dimensions: [f64; 2],
    threads: usize,
) -> Result<(Vec<f64>, Vec<f64>), &'static str> {
    if !(1..=1024).contains(&n)
        || !(2..=1024).contains(&grid)
        || threads == 0
        || x.len() != (n + 1).pow(2)
        || y.len() != x.len()
        || x.iter().chain(y).any(|v| !v.is_finite())
        || [aperture, dimensions[0], dimensions[1]]
            .iter()
            .any(|v| !v.is_finite() || *v <= 0.)
    {
        return Err("Invalid transport dimensions or map");
    }
    let map = Map { x, y, n };
    let step = aperture / (grid - 1) as f64;
    let axis = |i: usize| {
        if i == grid - 1 {
            aperture * 0.5
        } else {
            i as f64 * step - aperture * 0.5
        }
    };
    let mut points = Vec::with_capacity(grid * grid);
    for j in 0..grid {
        for i in 0..grid {
            let (x, y) = (axis(i), axis(j));
            let mut point = Point {
                x,
                y,
                u: (x / aperture + 0.5) * n as f64,
                v: (y / aperture + 0.5) * n as f64,
                ..Point::default()
            };
            map.measure(&mut point);
            points.push(point);
        }
    }
    let rows = grid.div_ceil(threads.min(grid));
    for _ in 0..80 {
        let residual = points
            .iter()
            .map(|point| point.residual.0.abs().max(point.residual.1.abs()))
            .fold(0., f64::max);
        if residual < 1e-7 * aperture {
            return Ok(points
                .iter()
                .map(|point| {
                    (
                        (point.u / n as f64 - 0.5) * dimensions[0],
                        (point.v / n as f64 - 0.5) * dimensions[1],
                    )
                })
                .unzip());
        }
        if threads == 1 {
            for point in &mut points {
                map.advance(point)?;
            }
        } else {
            thread::scope(|scope| {
                let jobs: Vec<_> = points
                    .chunks_mut(rows * grid)
                    .map(|chunk| {
                        let map = &map;
                        scope.spawn(move || {
                            for point in chunk {
                                map.advance(point)?;
                            }
                            Ok::<_, &'static str>(())
                        })
                    })
                    .collect();
                for job in jobs {
                    job.join().map_err(|_| "Transport thread failed")??;
                }
                Ok(())
            })?;
        }
    }
    Err("Transport inversion did not converge within its iteration budget.")
}

/// Invert a map through the version 1 C ABI. The caller owns all arrays.
///
/// # Safety
/// Non-null pointers must be aligned and valid for the declared number of
/// elements. Only read access to the input arrays and exclusive write access
/// to the output arrays may occur during this synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raysculpt_forward_transport_v1(
    mx: *const f64,
    my: *const f64,
    map_len: usize,
    cells: usize,
    grid: usize,
    aperture: f64,
    width: f64,
    height: f64,
    threads: usize,
    tx: *mut f64,
    ty: *mut f64,
    out_len: usize,
) -> i32 {
    if [mx, my, tx.cast_const(), ty.cast_const()]
        .iter()
        .any(|p| p.is_null())
        || !(1..=1024).contains(&cells)
        || !(2..=1024).contains(&grid)
        || !(1..=12).contains(&threads)
        || tx == ty
        || map_len != (cells + 1).pow(2)
        || out_len != grid.pow(2)
    {
        return 1;
    }
    std::panic::catch_unwind(|| {
        let x = unsafe { std::slice::from_raw_parts(mx, map_len) };
        let y = unsafe { std::slice::from_raw_parts(my, map_len) };
        match forward_transport(x, y, cells, grid, aperture, [width, height], threads) {
            Ok((x, y)) => {
                unsafe {
                    std::ptr::copy_nonoverlapping(x.as_ptr(), tx, out_len);
                    std::ptr::copy_nonoverlapping(y.as_ptr(), ty, out_len);
                }
                0
            }
            Err(message) if message.starts_with("Transport folded") => 2,
            Err(message) if message.starts_with("Transport inversion") => 3,
            Err(_) => 1,
        }
    })
    .unwrap_or(4)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identity_and_folded_maps() {
        let x = [-1., 1., -1., 1.];
        let y = [-1., -1., 1., 1.];
        let (tx, ty) = forward_transport(&x, &y, 1, 3, 2., [2., 4.], 1).unwrap();
        assert_eq!(tx, [-1., 0., 1.].repeat(3));
        assert_eq!(ty, [-2., -2., -2., 0., 0., 0., 2., 2., 2.]);
        let folded = x.map(|v| -v);
        assert!(forward_transport(&folded, &y, 1, 3, 2., [2., 4.], 1).is_err());
    }
}
