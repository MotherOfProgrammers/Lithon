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
    (
        "C:/Windows/Fonts/JetBrainsMonoNerdFontMono-Regular.ttf",
        "C:/Windows/Fonts/JetBrainsMonoNerdFontMono-Bold.ttf",
    ),  # Nerd Font first: the UI uses its icon glyphs (checked against its cmap, see ui.py)
    ("C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/consolab.ttf"),
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

# --------------------------------------------------------------------------
# Background music: procedurally synthesized by ffmpeg itself at mux time
# (lavfi sources -- sine waves and expression-driven oscillators), not a
# downloaded/licensed audio file. 100% generated on the fly, so there is no
# copyright to clear: a steady pulsing bass note under a fast four-note
# arpeggio, in the same spirit as the terminal UI -- synth/"hacker" adjacent,
# unobtrusive under a benchmark recording.
# --------------------------------------------------------------------------

AUDIO_SAMPLE_RATE = 44100
MUSIC_BPM = 128.0
MUSIC_ARP_NOTES = (220.00, 261.63, 329.63, 392.00)  # A3 minor-7th arpeggio (A, C, E, G)
MUSIC_BASS_HZ = 55.00  # A1, two octaves under the arpeggio root


def _arp_freq_expr(notes: tuple[float, ...], step: float) -> str:
    """An ffmpeg eval expression that steps through `notes` every `step`
    seconds, built as nested if(eq(mod(...))) terms (ffmpeg's `aevalsrc` has
    no array/lookup primitive). Commas are backslash-escaped because
    ffmpeg's filtergraph syntax otherwise reads them as argument separators."""
    n = len(notes)

    def nest(i: int) -> str:
        if i == n - 1:
            return f"{notes[i]:.2f}"
        return f"if(eq(mod(floor(t/{step:.4f})\\,{n})\\,{i})\\,{notes[i]:.2f}\\,{nest(i + 1)})"

    return nest(0)


def _music_ffmpeg_args() -> tuple[list[str], str]:
    """Two extra `-f lavfi` input args (arpeggio, pulsing bass) plus a
    `-filter_complex` string that mixes them down to a single `[a]` output
    stream, ready to `-map "[a]"`."""
    step = 60.0 / MUSIC_BPM / 4.0  # 16th-note step
    pulse_hz = MUSIC_BPM / 60.0 / 2.0  # half-note tremolo on the bass
    freq_expr = f"({_arp_freq_expr(MUSIC_ARP_NOTES, step)})"
    arp = f"aevalsrc=exprs=0.15*sin(2*PI*{freq_expr}*t):s={AUDIO_SAMPLE_RATE}"
    bass = f"sine=frequency={MUSIC_BASS_HZ:.2f}:sample_rate={AUDIO_SAMPLE_RATE},tremolo=f={pulse_hz:.4f}:d=0.6"
    filter_complex = (
        "[1:a]volume=1.0[arp];"
        "[2:a]volume=0.9[bass];"
        "[arp][bass]amix=inputs=2:duration=longest:weights=1 1[mixed];"
        "[mixed]afade=t=in:st=0:d=1.5,alimiter=limit=0.9,volume=0.6[a]"
    )
    return ["-f", "lavfi", "-i", arp, "-f", "lavfi", "-i", bass], filter_complex


class VideoRecorder:
    def __init__(
        self,
        out_path: pathlib.Path,
        width_chars: int = 150,
        height_chars: int = 42,
        fps: int = 20,
        font_size: int = 16,
        music: bool = True,
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
        self.music = music
        self.out_path = pathlib.Path(out_path)
        self.out_path.parent.mkdir(parents=True, exist_ok=True)

        ffmpeg = _require_ffmpeg()
        cmd = [
            ffmpeg, "-y", "-loglevel", "error",
            "-f", "rawvideo", "-pix_fmt", "rgb24",
            "-s", f"{self.img_w}x{self.img_h}", "-r", str(fps),
            "-i", "-",
        ]
        if music:
            music_inputs, filter_complex = _music_ffmpeg_args()
            cmd += music_inputs
            cmd += [
                "-filter_complex", filter_complex,
                "-map", "0:v", "-map", "[a]",
                "-c:v", "libx264", "-pix_fmt", "yuv420p",
                "-c:a", "aac", "-b:a", "160k",
                "-shortest",  # the music generators are infinite; stop when the video (stdin) ends
                "-movflags", "+faststart",
                str(self.out_path),
            ]
        else:
            cmd += [
                "-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
                str(self.out_path),
            ]
        self._proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
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
