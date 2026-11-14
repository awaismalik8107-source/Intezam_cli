# Cache performance comparison

Real loopback peers, verified disk cache and headless LibVLC. Values are
medians of three runs; maximum rebuild duration is the largest observed
rebuild across the three runs. Read stalls are a consumer-wait measure,
separate from LibVLC buffering states. This is not a WAN benchmark.

Raw data: [before](performance/before.csv), [after](performance/after.csv).
See [the design and reproduction instructions](cache-optimization.md).

## Sequential

| Metric | Before | After | Change |
|---|---:|---:|---:|
| Torrent rebuilds | 111 | 14 | 87.4% lower |
| Average rebuild duration | 0.665 ms | 0.872 ms | 31.1% higher |
| Maximum rebuild duration | 1.429 ms | 1.822 ms | 27.5% higher |
| Pieces reclaimed per rebuild | 1.000 | 8.000 | +7.000 pieces |
| Pieces admitted per rebuild | 1.000 | 8.000 | +7.000 pieces |
| Cache-window changes | 112 | 29 | 74.1% lower |
| Consumer stalls (wait >=50 ms) | 0 | 0 | unchanged |
| Total verified-data waiting | 4.181 ms | 1.002 ms | 76.0% lower |
| Consumer stall duration | 0.000 ms | 0.000 ms | unchanged |
| Cache hit rate | 99.632% | 99.632% | +0.000 percentage points |
| Backend work, including hashing | 7.190 ms | 8.310 ms | 15.6% higher |
| Verified lease disk reads | 0.929 ms | 1.610 ms | 73.3% higher |
| Minimum free slots sampled | 0 | 7 | +7.000 slots |
| Maximum free slots sampled | 0 | 7 | +7.000 slots |
| Post-start VLC buffering states | 0 | 0 | unchanged |
| Post-start VLC buffering duration | 0.000 ms | 0.000 ms | unchanged |

Managed disk allocation: 819,200 -> 819,200 bytes; budget 838,860 bytes.

## Seeking

| Metric | Before | After | Change |
|---|---:|---:|---:|
| Torrent rebuilds | 25 | 7 | 72.0% lower |
| Average rebuild duration | 0.899 ms | 1.282 ms | 42.6% higher |
| Maximum rebuild duration | 1.970 ms | 2.286 ms | 16.0% higher |
| Pieces reclaimed per rebuild | 14.720 | 43.000 | +28.280 pieces |
| Pieces admitted per rebuild | 14.720 | 41.000 | +26.280 pieces |
| Cache-window changes | 26 | 26 | unchanged |
| Consumer stalls (wait >=50 ms) | 0 | 0 | unchanged |
| Total verified-data waiting | 98.256 ms | 31.393 ms | 68.0% lower |
| Consumer stall duration | 0.000 ms | 0.000 ms | unchanged |
| Cache hit rate | 95.455% | 95.455% | +0.000 percentage points |
| Backend work, including hashing | 15.648 ms | 17.778 ms | 13.6% higher |
| Verified lease disk reads | 0.684 ms | 1.352 ms | 97.7% higher |
| Minimum free slots sampled | 0 | 7 | +7.000 slots |
| Maximum free slots sampled | 0 | 7 | +7.000 slots |
| Post-start VLC buffering states | 0 | 0 | unchanged |
| Post-start VLC buffering duration | 0.000 ms | 0.000 ms | unchanged |

Managed disk allocation: 819,200 -> 819,200 bytes; budget 838,860 bytes.

## VLC

| Metric | Before | After | Change |
|---|---:|---:|---:|
| Torrent rebuilds | 97 | 91 | 6.2% lower |
| Average rebuild duration | 0.601 ms | 0.668 ms | 11.1% higher |
| Maximum rebuild duration | 1.506 ms | 1.332 ms | 11.6% lower |
| Pieces reclaimed per rebuild | 8.557 | 9.132 | +0.575 pieces |
| Pieces admitted per rebuild | 8.557 | 9.121 | +0.564 pieces |
| Cache-window changes | 98 | 92 | 6.1% lower |
| Consumer stalls (wait >=50 ms) | 0 | 0 | unchanged |
| Total verified-data waiting | 501.417 ms | 209.668 ms | 58.2% lower |
| Consumer stall duration | 0.000 ms | 0.000 ms | unchanged |
| Cache hit rate | 96.448% | 93.716% | -2.732 percentage points |
| Backend work, including hashing | 39.257 ms | 51.415 ms | 31.0% higher |
| Verified lease disk reads | 6.277 ms | 6.413 ms | 2.2% higher |
| Minimum free slots sampled | 0 | 0 | +0.000 slots |
| Maximum free slots sampled | 0 | 1 | +1.000 slots |
| Post-start VLC buffering states | 0 | 0 | unchanged |
| Post-start VLC buffering duration | 0.000 ms | 0.000 ms | unchanged |

Managed disk allocation: 327,680 -> 327,680 bytes; budget 347,812 bytes.

