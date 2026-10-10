# v1.4.0 - Zero-GPU idle, one-click optimizer & desktop layering fix

### Changes in v1.4.0

- **True Zero-Cost Idle (Deep Sleep)**: Pausing no longer leaves Media Foundation decoding in the background. After a few seconds of occlusion the engine fully unloads the decoder and swapchain, dropping to ~5 MB RAM and 0% GPU, then rebuilds in ~130 ms on resume. Honors your Performance > Deep sleep setting (auto after 8s when unset).

- **One-Click Optimize For This PC**: New `Optimize for this PC…` action in the wallpaper menu plus `motioncli --optimize` CLI. Re-encodes oversized clips (e.g. 1440p) to screen-friendly 720p with the built-in Windows encoders, keeping AAC audio. Example: 39.4 MB 1440p → 7.4 MB 720p, playing GPU 36% → ~10% on weak iGPUs.

- **Wallpaper Now Attaches On Every Shell Layout**: Host discovery now also finds Progman-child `WorkerW` windows (used by newer Windows builds), waits longer on slow PCs, hides stray visible `WorkerW`s, sizes the host fullscreen, and falls back to Progman placement instead of ever opening a popup window.

- **Fresh-Install Playback Fixes**: Local files are passed to Media Foundation as proper `file://` URLs (spaces/Unicode paths work), D3D11 keeps video-decode support on every fallback including WARP, missing files and unplugged monitors no longer produce black panes, and absent H264 decoders are reported clearly.

- **Faster Failure Diagnosis**: One-shot logging for swapchain, transfer, and present failures, pause/resume transition lines, pane counts on reload, and HD-vs-full rendition picks in the downloader.

- **Lighter Idle Polling**: The occlusion scanner now re-scans only on focus change or every 4th tick, cutting background CPU to a fraction with identical pause behavior.
