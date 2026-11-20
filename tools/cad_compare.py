"""Target placement and synchronized inspection of calculated receiver images."""
import numpy as np
from PIL import Image, ImageOps
from PySide6 import QtCore as C, QtGui as G, QtWidgets as W
from PySide6.QtCore import Qt

from cad_targets import decode_srgb, encode_srgb, screen_pixels, screen_target


def pixmap(array):
    pixels = np.ascontiguousarray(array)
    fmt = G.QImage.Format_Grayscale8 if pixels.ndim == 2 else G.QImage.Format_RGB888
    image = G.QImage(pixels.data, pixels.shape[1], pixels.shape[0], pixels.strides[0], fmt)
    return G.QPixmap.fromImage(image.copy())


class TargetDialog(W.QDialog):
    def __init__(self, parent, path, screen):
        super().__init__(parent)
        self.setWindowTitle("Изображение экрана")
        self.mode = W.QComboBox()
        for label, key in (("Вписать целиком · тёмные поля", "fit"),
                           ("Заполнить экран · обрезать края", "crop"),
                           ("Изменить высоту экрана под изображение", "resize")):
            self.mode.addItem(label, key)
        self.mode.setCurrentIndex(self.mode.findData(screen.get("image_fit", "fit")))
        with Image.open(path) as source:
            original = ImageOps.exif_transpose(source)
            self.source_size = original.size
            original.thumbnail((640, 640))
            self.thumbnail = original.convert("RGBA")
        self.dims = screen["dims"].copy()
        self.preview = W.QLabel()
        self.preview.setFixedSize(330, 220)
        self.preview.setAlignment(Qt.AlignCenter)
        self.preview.setStyleSheet("background:#dae3e8")
        self.caption = W.QLabel()
        layout = W.QVBoxLayout(self)
        layout.addWidget(W.QLabel(screen["name"] + f"  ·  {self.source_size[0]} × {self.source_size[1]} пикс."))
        layout.addWidget(self.mode)
        layout.addWidget(self.preview, 0, Qt.AlignHCenter)
        layout.addWidget(self.caption)
        buttons = W.QDialogButtonBox(W.QDialogButtonBox.Ok | W.QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.mode.currentIndexChanged.connect(self.update_preview)
        self.update_preview()

    def update_preview(self):
        mode = self.mode.currentData()
        w, h = self.dims["w"], self.dims["d"]
        if mode == "resize":
            h = w * self.source_size[1] / self.source_size[0]
        self.height_mm = h
        cols, rows = screen_pixels(w, h, 320)
        frame = screen_target(self.thumbnail, (cols, rows), "crop" if mode == "crop" else "fit")
        self.preview.setPixmap(pixmap(encode_srgb(frame)).scaled(320, 210, Qt.KeepAspectRatio, Qt.SmoothTransformation))
        self.caption.setText(f"Размер экрана после применения   {w:g} × {h:g} мм")


def comparisons(image, target, gain=None):
    with Image.open(image) as im:
        actual = decode_srgb(np.asarray(im.convert("L"), dtype=float) / 255)
    if target is None:
        return [None, pixmap(encode_srgb(actual)), None]
    with Image.open(target) as im:
        if target.suffix==".pgm":
            # The 3D renderer writes 8-bit PGM. Photos use 16-bit PGM.
            goal = np.asarray(im, dtype=float) / (255 if im.mode == "L" else 65535)
        else:
            goal = decode_srgb(np.asarray(im.convert("L"), dtype=float) / 255)
    if gain is None:
        energy = float(np.sum(actual * actual))
        gain = float(np.sum(actual * goal)) / energy if energy else 0
    actual = gain * actual
    delta = actual - goal
    magnitude = encode_srgb(np.abs(delta)).astype(float) / 255
    # Diverging map: neutral paper at equality, blue at deficit, orange at excess.
    colors = np.where((delta < 0)[..., None], [33, 99, 166], [184, 68, 18])
    error = np.uint8(247 + magnitude[..., None] * (colors - 247))
    return [pixmap(encode_srgb(goal)), pixmap(encode_srgb(actual)), pixmap(error)]


class Comparison(W.QWidget):
    def __init__(self):
        super().__init__()
        self.setMinimumSize(210, 155)
        self.images = []
        self.reset()
        self.setToolTip("Колесо — общий масштаб, перетаскивание — сдвиг, двойной щелчок — вписать")

    def reset(self):
        self.zoom = 1.
        self.offset = C.QPointF()
        self.drag_position = None
        self.update()

    def set_images(self, images):
        self.images = images
        self.reset()

    def cells(self):
        width = self.width() / 3
        return [C.QRectF(i * width + 3, 30, max(1, width - 6), max(1, self.height() - 38)) for i in range(3)]

    def paintEvent(self, event):
        p = G.QPainter(self)
        p.fillRect(self.rect(), G.QColor("#eff3f5"))
        p.setRenderHint(G.QPainter.SmoothPixmapTransform, self.zoom < 2)
        for i, (name, area) in enumerate(zip(("Цель", "Проекция", "Ошибка яркости"), self.cells())):
            p.setPen(G.QColor("#264354"))
            p.drawText(C.QRectF(area.x(), 0, area.width(), 28), Qt.AlignCenter, name)
            p.fillRect(area, G.QColor("#dde5e9"))
            image = self.images[i] if self.images else None
            if image is None:
                p.drawText(area, Qt.AlignCenter | Qt.TextWordWrap, "Цель отсутствует" if self.images else "Нет расчёта")
                continue
            scale = min(area.width() / image.width(), area.height() / image.height()) * self.zoom
            rect = C.QRectF(0, 0, image.width() * scale, image.height() * scale)
            rect.moveCenter(area.center() + self.offset)
            p.save()
            p.setClipRect(area)
            p.drawPixmap(rect, image, C.QRectF(image.rect()))
            p.restore()

    def wheelEvent(self, event):
        if not self.images:
            return
        area = next((r for r in self.cells() if r.contains(event.position())), None)
        if area is None:
            return
        before = self.zoom
        self.zoom = float(np.clip(before * 1.25 ** (event.angleDelta().y() / 120), .5, 24))
        anchor = event.position() - area.center()
        self.offset = anchor + (self.offset - anchor) * (self.zoom / before)
        self.update()

    def mousePressEvent(self, event):
        if event.button() == Qt.LeftButton:
            self.drag_position = event.position()

    def mouseMoveEvent(self, event):
        if self.drag_position is not None:
            self.offset += event.position() - self.drag_position
            self.drag_position = event.position()
            self.update()

    def mouseReleaseEvent(self, event):
        self.drag_position = None

    def mouseDoubleClickEvent(self, event):
        self.reset()
