"""Map a photo into physical screen coordinates; returned samples are linear luminance."""
import numpy as np
from PIL import Image, ImageOps


def screen_pixels(width, height, resolution):
    longest = max(width, height)
    return max(1, round(resolution * width / longest)), max(1, round(resolution * height / longest))


def decode_srgb(encoded):
    return np.where(encoded <= .04045, encoded / 12.92, ((encoded + .055) / 1.055) ** 2.4)


def encode_srgb(linear):
    linear = np.clip(linear, 0, 1)
    return np.uint8(np.rint(255 * np.where(linear <= .0031308, 12.92 * linear, 1.055 * linear ** (1 / 2.4) - .055)))


def screen_target(path, size, mode="fit"):
    cols, rows = size
    if isinstance(path, Image.Image):
        photo = path.convert("RGBA")
    else:
        with Image.open(path) as original:
            photo = ImageOps.exif_transpose(original).convert("RGBA")
    w, h = photo.size
    if mode == "crop":
        if w * rows > h * cols:
            extent = max(1, round(h * cols / rows))
            left = (w - extent) // 2
            photo = photo.crop((left, 0, left + extent, h))
        else:
            extent = max(1, round(w * rows / cols))
            top = (h - extent) // 2
            photo = photo.crop((0, top, w, top + extent))
        dest = size
    elif mode == "fit":
        scale = min(cols / w, rows / h)
        dest = max(1, round(w * scale)), max(1, round(h * scale))
    else:
        raise ValueError("Неизвестный режим размещения изображения")
    rgba = np.asarray(photo)
    table = decode_srgb(np.arange(256, dtype=np.float32) / 255)
    gray = np.zeros(rgba.shape[:2], dtype=np.float32)
    for channel, weight in enumerate((.2126, .7152, .0722)):
        gray += weight * table[rgba[..., channel]]
    gray *= rgba[..., 3] / 255
    resampling = Image.Resampling.BOX if dest[0] <= photo.width and dest[1] <= photo.height else Image.Resampling.BILINEAR
    tile = Image.fromarray(gray).resize(dest, resampling)
    canvas = Image.new("F", size)
    canvas.paste(tile, ((cols - dest[0]) // 2, (rows - dest[1]) // 2))
    return np.asarray(canvas)
