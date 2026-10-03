"""Turns the same frames shown live in the terminal into an actual video
file: each frame is rasterized to an RGB image (via Pillow, honoring the
colors/bold of the rich renderable) and streamed straight into ffmpeg's
stdin as raw video, so nothing is ever written to disk except the final
.mp4. Frame cadence matches wall-clock time, so the video's timer is the
real, accurate elapsed time of each task -- recording never speeds anything
up or down.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess

from PIL import Image, ImageDraw, ImageFont
from rich.console import Console, RenderableType

BG = (18, 18, 18)
FG = (220, 220, 220)

_FONT_CANDIDATES = [
    ("C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/consolab.ttf"),
    (
        "C:/Windows/Fonts/JetBrainsMonoNerdFontMono-Regular.ttf",
        "C:/Windows/Fonts/JetBrainsMonoNerdFontMono-Bold.ttf",
    ),
    ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"),
]


def _find_fonts() -> tuple[str, str]:
    for regular, bold in _FONT_CANDIDATES:
        if pathlib.Path(regular).exists():
            return regular, bold if pathlib.Path(bold).exists() else regular
    raise FileNotFoundError(
        "no monospace TTF font found for video rendering; looked for "
        + ", ".join(r for r, _ in _FONT_CANDIDATES)
    )


def _require_ffmpeg() -> str:
    exe = shutil.which("ffmpeg")
    if not exe:
        raise FileNotFoundError("ffmpeg not found on PATH; install it to render videos")
    return exe


class VideoRecorder:
    def __init__(
        self,
        out_path: pathlib.Path,
        width_chars: int = 150,
        height_chars: int = 42,
        fps: int = 20,
        font_size: int = 16,
    ):
        regular, bold = _find_fonts()
        self.font = ImageFont.truetype(regular, font_size)
        self.bold_font = ImageFont.truetype(bold, font_size)
        self.console = Console(width=width_chars, height=height_chars, color_system="truecolor")

        l, t, r, b = self.font.getbbox("M")
        self.cell_w = max(r - l, 1)
        self.cell_h = int(font_size * 1.35)
        self.img_w = width_chars * self.cell_w
        self.img_h = height_chars * self.cell_h
        self.img_w += self.img_w % 2   # libx264 + yuv420p require even dimensions
        self.img_h += self.img_h % 2
        self.fps = fps
        self.out_path = pathlib.Path(out_path)
        self.out_path.parent.mkdir(parents=True, exist_ok=True)

        ffmpeg = _require_ffmpeg()
        self._proc = subprocess.Popen(
            [
                ffmpeg, "-y", "-loglevel", "error",
                "-f", "rawvideo", "-pix_fmt", "rgb24",
                "-s", f"{self.img_w}x{self.img_h}", "-r", str(fps),
                "-i", "-",
                "-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
                str(self.out_path),
            ],
            stdin=subprocess.PIPE,
        )
        self._frame_count = 0

    def _rasterize(self, renderable: RenderableType) -> bytes:
        lines = self.console.render_lines(renderable, pad=True)
        img = Image.new("RGB", (self.img_w, self.img_h), BG)
        draw = ImageDraw.Draw(img)
        for y, line in enumerate(lines[: self.console.height]):
            x = 0
            for seg in line:
                text = seg.text
                if not text:
                    continue
                w = len(text) * self.cell_w
                fg, bg, bold = FG, None, False
                style = seg.style
                if style:
                    bold = bool(style.bold)
                    if style.color is not None:
                        c = style.color.get_truecolor()
                        fg = (c.red, c.green, c.blue)
                    if style.bgcolor is not None:
                        c = style.bgcolor.get_truecolor()
                        bg = (c.red, c.green, c.blue)
                    if style.dim:
                        fg = tuple(v // 2 for v in fg)
                if bg is not None:
                    draw.rectangle([x, y * self.cell_h, x + w, (y + 1) * self.cell_h], fill=bg)
                draw.text((x, y * self.cell_h), text, font=self.bold_font if bold else self.font, fill=fg)
                x += w
        return img.tobytes()

    def capture(self, renderable: RenderableType, hold_frames: int = 1) -> None:
        """Render one frame and write it `hold_frames` times (>1 to pause on
        a frame, e.g. the closing summary, without re-rendering it)."""
        data = self._rasterize(renderable)
        stdin = self._proc.stdin
        assert stdin is not None
        for _ in range(hold_frames):
            stdin.write(data)
        self._frame_count += hold_frames

    def close(self) -> None:
        assert self._proc.stdin is not None
        self._proc.stdin.close()
        self._proc.wait()
        if self._proc.returncode != 0:
            raise RuntimeError(f"ffmpeg exited {self._proc.returncode} while writing {self.out_path}")
