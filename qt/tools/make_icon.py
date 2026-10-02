"""Render the app icon (same design as theme::appIcon() in src/Theme.cpp).

Writes resources/app.ico (Windows exe icon, multi-size) and
resources/rf-powermeter-assistant.png (256 px, Linux desktop entry).

Needs PyQt5 or PySide6, and Pillow:  python tools/make_icon.py
"""

import io
import os

try:
    from PyQt5.QtCore import QBuffer, QIODevice, QPointF, QRectF, Qt
    from PyQt5.QtGui import QColor, QGuiApplication, QImage, QLinearGradient, QPainter, QPainterPath, QPen
except ImportError:  # pragma: no cover
    from PySide6.QtCore import QBuffer, QIODevice, QPointF, QRectF, Qt
    from PySide6.QtGui import QColor, QGuiApplication, QImage, QLinearGradient, QPainter, QPainterPath, QPen

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "resources")


def paint_logo(p, r):
    wave = QPainterPath()
    w, h = r.width(), r.height()
    y0 = r.center().y()
    amp = h * 0.36
    wave.moveTo(r.left(), y0)
    for i in range(4):
        x0 = r.left() + w * i / 4.0
        sign = -1.0 if i % 2 == 0 else 1.0
        wave.quadTo(QPointF(x0 + w / 8.0, y0 + sign * amp * 1.6), QPointF(x0 + w / 4.0, y0))
    for i in range(3, 0, -1):
        glow = QColor(0xA8, 0x55, 0xF7)
        glow.setAlphaF(0.12)
        p.setPen(QPen(glow, h * 0.09 + i * h * 0.07, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
        p.drawPath(wave)
    p.setPen(QPen(QColor(0xC0, 0x84, 0xFC), max(1.6, h * 0.11), Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
    p.drawPath(wave)


def render(size):
    img = QImage(size, size, QImage.Format_ARGB32)
    img.fill(Qt.transparent)
    p = QPainter(img)
    p.setRenderHint(QPainter.Antialiasing)
    r = QRectF(0.5, 0.5, size - 1.0, size - 1.0)
    bg = QLinearGradient(r.topLeft(), r.bottomRight())
    bg.setColorAt(0, QColor(0x2A, 0x1B, 0x52))
    bg.setColorAt(1, QColor(0x0C, 0x08, 0x13))
    p.setBrush(bg)
    p.setPen(QPen(QColor(0x4C, 0x3A, 0x82), max(1.0, size / 48.0)))
    p.drawRoundedRect(r, size * 0.22, size * 0.22)
    paint_logo(p, r.adjusted(size * 0.16, size * 0.3, -size * 0.16, -size * 0.3))
    p.end()
    buf = QBuffer()
    buf.open(QIODevice.ReadWrite)
    img.save(buf, "PNG")
    return Image.open(io.BytesIO(bytes(buf.data())))


def main():
    _app = QGuiApplication([])
    os.makedirs(OUT, exist_ok=True)
    big = render(256)
    big.save(os.path.join(OUT, "rf-powermeter-assistant.png"))
    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [render(s) for s in sizes]
    frames[-1].save(os.path.join(OUT, "app.ico"), format="ICO", sizes=[(s, s) for s in sizes],
                    append_images=frames[:-1])
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
