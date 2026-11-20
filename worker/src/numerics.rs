//! Native numerical stages for the photographic solver.
use std::f64::consts::PI;

type Filter = unsafe extern "C" fn(*const f64, *const f64, *mut f64, usize, usize, f64) -> i32;
type NumericResult<T> = Result<T, i32>;

fn filter(
    apply: Filter,
    src: &[f64],
    symbol: Option<&[f64]>,
    dst: &mut [f64],
    rows: usize,
    cols: usize,
    sigma: f64,
) -> NumericResult<()> {
    let symbol = symbol.map_or(std::ptr::null(), |values| values.as_ptr());
    let status = unsafe { apply(src.as_ptr(), symbol, dst.as_mut_ptr(), rows, cols, sigma) };
    if status == 0 { Ok(()) } else { Err(3) }
}

fn poisson_symbol(n: usize, dx: f64, dy: f64) -> Vec<f64> {
    let eigenvalues = |d: f64| -> Vec<_> {
        (0..n)
            .map(|i| {
                let sine = (PI * i as f64 / (2. * n as f64)).sin() / d;
                -4. * sine * sine
            })
            .collect()
    };
    let (x, y) = (eigenvalues(dx), eigenvalues(dy));
    let mut inverse = vec![0.; n * n];
    for j in 0..n {
        for i in 0..n {
            if i != 0 || j != 0 {
                inverse[j * n + i] = 1. / (x[i] + y[j]);
            }
        }
    }
    inverse
}

fn map_determinants(mx: &[f64], my: &[f64], n: usize, limit: f64, bad: &mut [bool]) -> bool {
    let stride = n + 1;
    let mut found = false;
    for j in 0..n {
        for i in 0..n {
            let base = j * stride + i;
            let mut folded = false;
            for horizontal in [base, base + stride] {
                for vertical in [base, base + 1] {
                    let hx = mx[horizontal + 1] - mx[horizontal];
                    let hy = my[horizontal + 1] - my[horizontal];
                    let kx = mx[vertical + stride] - mx[vertical];
                    let ky = my[vertical + stride] - my[vertical];
                    folded |= hx * ky - hy * kx < limit;
                }
            }
            bad[j * n + i] = folded;
            found |= folded;
        }
    }
    found
}

fn inverse(
    field: &[f64],
    n: usize,
    width: f64,
    height: f64,
    aperture: f64,
    steps: usize,
    apply: Filter,
) -> NumericResult<(Vec<f64>, Vec<f64>)> {
    let dx = width / n as f64;
    let dy = height / n as f64;
    let (ax, ay) = (aperture / width, aperture / height);
    let mean = field.iter().sum::<f64>() / (n * n) as f64;
    if !mean.is_finite() || mean <= 0. {
        return Err(1);
    }
    let mut density: Vec<_> = field.iter().map(|value| value / mean + 0.025).collect();
    let factor = ax * ay / (density.iter().sum::<f64>() / (n * n) as f64);
    for value in &mut density {
        *value *= factor;
    }
    let mut potential = vec![0.; n * n];
    let mut residual = vec![0.; n * n];
    let mut correction = vec![0.; n * n];
    let symbol = poisson_symbol(n, dx, dy);
    for _ in 0..steps {
        let mut squared = 0.;
        for j in 0..n {
            let top = j.saturating_sub(1) * n;
            let bottom = (j + 1).min(n - 1) * n;
            let row = j * n;
            for i in 0..n {
                let left = i.saturating_sub(1);
                let right = (i + 1).min(n - 1);
                let p = potential[row + i];
                let xx = ax + (potential[row + right] + potential[row + left] - 2. * p) / (dx * dx);
                let yy = ay + (potential[bottom + i] + potential[top + i] - 2. * p) / (dy * dy);
                let xy = (potential[bottom + right] + potential[top + left]
                    - potential[top + right]
                    - potential[bottom + left])
                    / (4. * dx * dy);
                let delta = xx - yy;
                let value = (delta * delta + 4. * (density[row + i] + xy * xy)).sqrt() - xx - yy;
                residual[row + i] = value;
                squared += value * value;
            }
        }
        filter(apply, &residual, Some(&symbol), &mut correction, n, n, 0.)?;
        for (value, adjustment) in potential.iter_mut().zip(&correction) {
            *value += 0.82 * adjustment;
        }
        if (squared / (n * n) as f64).sqrt() < 2e-5 * (ax + ay) {
            break;
        }
    }
    let stride = n + 1;
    let mut mx = vec![0.; stride * stride];
    let mut my = mx.clone();
    let axis = |i: usize| {
        if i == n {
            aperture / 2.
        } else {
            i as f64 * (aperture / n as f64) - aperture / 2.
        }
    };
    for j in 0..stride {
        let top = j.saturating_sub(1) * n;
        let bottom = j.min(n - 1) * n;
        for i in 0..stride {
            let left = i.saturating_sub(1);
            let right = i.min(n - 1);
            let vx = ((potential[bottom + right] - potential[bottom + left])
                + (potential[top + right] - potential[top + left]))
                / (2. * dx);
            let vy = ((potential[bottom + right] - potential[top + right])
                + (potential[bottom + left] - potential[top + left]))
                / (2. * dy);
            mx[j * stride + i] = vx + axis(i);
            my[j * stride + i] = vy + axis(j);
        }
    }
    let mut bad = vec![false; n * n];
    let mut blurx = vec![0.; stride * stride];
    let mut blury = blurx.clone();
    let limit = 1e-6 * (aperture / n as f64).powi(2);
    for _ in 0..16 {
        if !map_determinants(&mx, &my, n, limit, &mut bad) {
            return Ok((mx, my));
        }
        filter(apply, &mx, None, &mut blurx, stride, stride, 1.1)?;
        filter(apply, &my, None, &mut blury, stride, stride, 1.1)?;
        for j in 0..stride {
            for i in 0..stride {
                let repair = (j.saturating_sub(2)..=(j + 2).min(n - 1)).any(|row| {
                    (i.saturating_sub(2)..=(i + 2).min(n - 1)).any(|col| bad[row * n + col])
                });
                if repair {
                    mx[j * stride + i] = blurx[j * stride + i];
                    my[j * stride + i] = blury[j * stride + i];
                }
            }
        }
        for j in 0..stride {
            mx[j * stride] = axis(0);
            mx[j * stride + n] = axis(n);
            my[j] = axis(0);
            my[n * stride + j] = axis(n);
        }
    }
    Err(2)
}

fn spline_axis(values: &mut [f64], grid: usize, transpose: bool) {
    let index = |row: usize, col: usize| {
        if transpose {
            col * grid + row
        } else {
            row * grid + col
        }
    };
    let mut upper = vec![0.; grid];
    for j in 1..grid - 1 {
        let pivot = 4. - upper[j - 1];
        upper[j] = 1. / pivot;
        for i in 0..grid {
            let next = index(j, i);
            values[next] = (6. * values[next] - values[index(j - 1, i)]) / pivot;
        }
    }
    for j in (1..grid - 1).rev() {
        for i in 0..grid {
            let current = index(j, i);
            values[current] -= upper[j] * values[index(j + 1, i)];
        }
    }
}

fn profile(
    tx: &[f64],
    ty: &[f64],
    n: usize,
    aperture: f64,
    thickness: f64,
    index: f64,
    source: f64,
    screen: f64,
    apply: Filter,
) -> NumericResult<Vec<f64>> {
    let spacing = aperture / (n - 1) as f64;
    let axis = |i: usize| {
        if i == n - 1 {
            aperture / 2.
        } else {
            i as f64 * spacing - aperture / 2.
        }
    };
    let symbol = poisson_symbol(n, spacing, spacing);
    let mut relief = vec![0.; n * n];
    let mut rhs = relief.clone();
    let mut gx = relief.clone();
    let mut gy = relief.clone();
    for _ in 0..7 {
        for j in 0..n {
            for i in 0..n {
                let p = j * n + i;
                let (x, y) = (axis(i), axis(j));
                let z = thickness + relief[p];
                let ns = index * source;
                let mut factor = 1. + z / ns;
                for _ in 0..3 {
                    let r2 = (x * x + y * y) / (factor * factor);
                    factor = 1. + z / (ns * ns + (index * index - 1.) * r2).sqrt();
                }
                let (ex, ey) = (x / factor, y / factor);
                let distance = (source * source + ex * ex + ey * ey).sqrt();
                let (nx, ny) = (ex / distance, ey / distance);
                let nz = (index * index - nx * nx - ny * ny).sqrt();
                let (ox, oy, oz) = (tx[p] - x, ty[p] - y, screen - z);
                let length = (ox * ox + oy * oy + oz * oz).sqrt();
                let denominator = nz - oz / length;
                if !denominator.is_finite() || denominator <= 0. {
                    return Err(2);
                }
                gx[p] = (ox / length - nx) / denominator;
                gy[p] = (oy / length - ny) / denominator;
            }
        }
        for j in 0..n {
            for i in 0..n {
                let p = j * n + i;
                let mut value = 0.;
                if i + 1 < n {
                    value += (gx[p] + gx[p + 1]) / (2. * spacing);
                }
                if i > 0 {
                    value -= (gx[p - 1] + gx[p]) / (2. * spacing);
                }
                if j + 1 < n {
                    value += (gy[p] + gy[p + n]) / (2. * spacing);
                }
                if j > 0 {
                    value -= (gy[p - n] + gy[p]) / (2. * spacing);
                }
                rhs[p] = value;
            }
        }
        filter(apply, &rhs, Some(&symbol), &mut relief, n, n, 0.)?;
        let low = relief.iter().copied().fold(f64::INFINITY, f64::min);
        let high = relief.iter().copied().fold(f64::NEG_INFINITY, f64::max);
        for value in &mut relief {
            *value -= 0.5 * (low + high);
        }
    }
    spline_axis(&mut relief, n, false);
    spline_axis(&mut relief, n, true);
    Ok(relief)
}

fn display(linear: f64) -> f64 {
    let linear = linear.max(0.);
    if linear <= 0.0031308 {
        12.92 * linear
    } else {
        1.055 * linear.powf(1. / 2.4) - 0.055
    }
}

fn calibrate(
    actual: &[f64],
    target: &[f64],
    goal: &[f64],
    drive: &[f64],
    rows: usize,
    cols: usize,
    error: f64,
    floor: f64,
    apply: Filter,
) -> NumericResult<(f64, Vec<f64>)> {
    let delta: Vec<_> = actual
        .iter()
        .zip(target)
        .map(|(a, t)| display(*a) - display(*t))
        .collect();
    let mut blurred = vec![0.; rows * cols];
    filter(apply, &delta, None, &mut blurred, rows, cols, 1.5)?;
    let area = (rows * cols) as f64;
    let tone = (delta.iter().map(|d| d * d).sum::<f64>() / area).sqrt();
    let texture = (delta
        .iter()
        .zip(&blurred)
        .map(|(a, b)| (a - b).powi(2))
        .sum::<f64>()
        / area)
        .sqrt();
    let residual: Vec<_> = goal.iter().zip(actual).map(|(g, a)| g - a).collect();
    filter(apply, &residual, None, &mut blurred, rows, cols, 0.65)?;
    let corrected = drive
        .iter()
        .zip(&blurred)
        .map(|(d, r)| floor.max(d + 0.8 * r))
        .collect();
    Ok((tone + texture + 0.15 * error, corrected))
}

fn status(run: impl FnOnce() -> NumericResult<()> + std::panic::UnwindSafe) -> i32 {
    match std::panic::catch_unwind(run) {
        Ok(Ok(())) => 0,
        Ok(Err(code)) => code,
        Err(_) => 4,
    }
}

// All arrays below are caller-owned contiguous f64 buffers. The Python
// wrapper supplies dimensions and keeps arrays and the adapter alive.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raysculpt_inverse_transport_v1(
    field: *const f64,
    n: usize,
    width: f64,
    height: f64,
    aperture: f64,
    steps: usize,
    mx: *mut f64,
    my: *mut f64,
    apply: Option<Filter>,
) -> i32 {
    if field.is_null()
        || mx.is_null()
        || my.is_null()
        || mx == my
        || n < 2
        || n > 1024
        || !n.is_multiple_of(2)
        || !(1..=500).contains(&steps)
        || [width, height, aperture]
            .iter()
            .any(|v| !v.is_finite() || *v <= 0.)
    {
        return 1;
    }
    let Some(apply) = apply else {
        return 1;
    };
    status(|| {
        let field = unsafe { std::slice::from_raw_parts(field, n * n) };
        let (x, y) = inverse(field, n, width, height, aperture, steps, apply)?;
        unsafe {
            std::ptr::copy_nonoverlapping(x.as_ptr(), mx, x.len());
            std::ptr::copy_nonoverlapping(y.as_ptr(), my, y.len());
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn raysculpt_optical_profile_v1(
    tx: *const f64,
    ty: *const f64,
    n: usize,
    aperture: f64,
    thickness: f64,
    index: f64,
    source: f64,
    screen: f64,
    out: *mut f64,
    apply: Option<Filter>,
) -> i32 {
    if tx.is_null()
        || ty.is_null()
        || out.is_null()
        || !(2..=1024).contains(&n)
        || !n.is_multiple_of(2)
        || [aperture, thickness, index, source, screen]
            .iter()
            .any(|v| !v.is_finite() || *v <= 0.)
    {
        return 1;
    }
    let Some(apply) = apply else {
        return 1;
    };
    status(|| {
        let (x, y) = unsafe {
            (
                std::slice::from_raw_parts(tx, n * n),
                std::slice::from_raw_parts(ty, n * n),
            )
        };
        let values = profile(x, y, n, aperture, thickness, index, source, screen, apply)?;
        unsafe {
            std::ptr::copy_nonoverlapping(values.as_ptr(), out, values.len());
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn raysculpt_calibrate_v1(
    actual: *const f64,
    target: *const f64,
    goal: *const f64,
    drive: *const f64,
    rows: usize,
    cols: usize,
    error: f64,
    floor: f64,
    score: *mut f64,
    out: *mut f64,
    apply: Option<Filter>,
) -> i32 {
    if [
        actual,
        target,
        goal,
        drive,
        score.cast_const(),
        out.cast_const(),
    ]
    .iter()
    .any(|v| v.is_null())
        || !(1..=1024).contains(&rows)
        || !(1..=1024).contains(&cols)
    {
        return 1;
    }
    let Some(apply) = apply else {
        return 1;
    };
    status(|| {
        let arrays = [actual, target, goal, drive]
            .map(|p| unsafe { std::slice::from_raw_parts(p, rows * cols) });
        let (value, drive) = calibrate(
            arrays[0], arrays[1], arrays[2], arrays[3], rows, cols, error, floor, apply,
        )?;
        unsafe {
            score.write(value);
            std::ptr::copy_nonoverlapping(drive.as_ptr(), out, drive.len());
        }
        Ok(())
    })
}
