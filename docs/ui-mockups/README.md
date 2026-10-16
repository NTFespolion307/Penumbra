# Penumbra UI mockups (approved reference for milestone M10)

Six controller screens, designed and approved in planning. Live canvas: https://claude.ai/artifact/LS9QoQHVvAH1wV3UW4S3kL

| File | Screen |
|---|---|
| 01-dashboard.dc.html | Dashboard (start page): render card, hosts online + join code, GPUs, cluster CPU, cluster RAM, throughput, controller disk, events |
| 02-hosts.dc.html | Hosts: one row per host (status, name, OS, address, CPU, RAM, GPUs, avg GPU use, tiles/s, ping); click opens detail |
| 03-host-detail.dc.html | Host detail: header, CPU/RAM graphs, one card per GPU (backend, tier, VRAM, usage, temp, tiles, enable toggle), cache, log, actions |
| 04-render.dc.html | Render: scene selector + info, camera + framing preview, image and output settings, estimates, Start render |
| 05-viewport.dc.html | Viewport: live image (tiles coloured by host), progress, per-host share, Pause/Save/Stop, live exposure/tonemap |
| 06-settings.dc.html | Settings: hosting, join code, addresses, Join a render, this machine's devices, defaults and storage |

These are design files, not app code. They need the design tool's runtime to render, so read them as source:
the markup gives layout, labels and structure; inline styles give exact colours and sizes.

Visual language: background #121417, panels #1A1D21, borders #2A2E34, text #E8EAED, secondary text #A3AAB3,
accent amber #F2A33A, info blue #5AA9F6, OK green #4CC38A, danger #FF8A80. Fonts: IBM Plex Sans (UI), IBM Plex Mono (numbers, codes, addresses).
Host colours in the viewport: up to 8 distinct colours, then repeat with a hatch pattern.
All numbers in the mockups are sample data.
