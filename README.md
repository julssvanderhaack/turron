# Turrón

<img src="data/turron.svg" width="96" align="right" alt="Turrón">

Reproductor de vídeo ligero para Wayland, escrito en C. Pensado para portátiles
modestos: usa la GPU siempre que puede y no hace nada que no haga falta.

- **Formatos**: todo lo que decodifica FFmpeg (los mismos códecs que usa VLC):
  MKV, MP4, AVI, WebM, MPEG-TS, WMV, FLV, MOV, OGG…; H.264, HEVC, AV1, VP8/9,
  MPEG-2/4, VC-1, Theora…; también URLs (HTTP, HLS, RTSP).
- **Decodificación por GPU** con VA-API en copia cero (los fotogramas pasan del
  decodificador a OpenGL como dmabuf, sin tocar la CPU). Lo que la GPU no
  soporta se decodifica por software con todos los núcleos.
- **Sin toolkit**: Wayland nativo + OpenGL ES 3. La conversión de color, el
  escalado y el mapeo de tonos HDR→SDR se hacen en un único shader.
- **Audio** por PipeWire; el reloj maestro es el audio.
- **Subtítulos** con libass: ASS/SSA con sus fuentes incrustadas, SRT, WebVTT,
  PGS/DVD (bitmap) y archivos `.srt`/`.ass` junto al vídeo.
- Recuerda por dónde ibas, MPRIS (barra y teclas multimedia), español/inglés.

## Teclas

| Tecla | Acción |
|---|---|
| Espacio / `p` / clic | Pausa |
| ← / → | ∓5 s (Mayús ∓1 s, Ctrl ∓60 s) |
| Re Pág / Av Pág | ±60 s |
| 0–9 | Saltar al 0 %–90 % |
| ↑ / ↓ / rueda | Volumen |
| `m` | Silencio |
| `f` / doble clic | Pantalla completa |
| `s` | Cambiar subtítulos |
| `a` | Cambiar pista de audio |
| `n` / `b` | Archivo siguiente / anterior |
| `i` | Estadísticas (códec, GPU o CPU, fotogramas descartados) |
| `q` / Esc | Salir |

## Compilar

Dependencias (Arch): `ffmpeg libass pipewire wayland wayland-protocols libxkbcommon mesa systemd-libs`.

```sh
make            # NATIVE=0 para un binario portable
make install    # en ~/.local (PREFIX=/usr para todo el sistema)
```

## Opciones

`turron --help`. Las más útiles: `--fs`, `--start=SEG`, `--no-hw`, `--fast`
(decodificación más rápida y de menor calidad para vídeos que el equipo no
alcanza a decodificar en tiempo real), `--keep-open`, `--no-resume`.

## Limitaciones conocidas

- El audio se mezcla siempre a estéreo.
- Sin menús de DVD/Blu-ray ni desentrelazado.
- Sin control de velocidad de reproducción.

Licencia: GPL-3.0-or-later.
