# Engine 2.0 measured comparison

Three repetitions with real loopback peers and disk cache; VLC rows decode real video.
Values are medians; maximum rebuild time is the largest across all runs.
See [method, constraints and interpretation](../adaptive-engine-2.md).

## sequential

| Metric | Baseline | Adaptive |
|---|---:|---:|
| First decoded frame (ms; VLC only) | N/A | N/A |
| Verified prebuffer readiness (ms) | 20.862 | 39.782 |
| Post-start read stalls >=50 ms | 0.000 | 0.000 |
| Post-start read stall time (ms) | 0.000 | 0.000 |
| Post-start verified-data waiting (ms) | 0.000 | 0.000 |
| Average verified forward buffer (s) | 0.787 | 21.748 |
| Peak managed allocation (MiB) | 0.781 | 20.625 |
| Sampled slot utilization (%) | 86.000 | 58.300 |
| Torrent rebuilds | 14.000 | 0.000 |
| Average rebuild duration (ms) | 0.959 | 0.000 |
| Maximum rebuild duration (ms) | 1.587 | 0.000 |
| Useful verified delivery (MiB/s) | 0.593 | 2.936 |

## seeking

| Metric | Baseline | Adaptive |
|---|---:|---:|
| First decoded frame (ms; VLC only) | N/A | N/A |
| Verified prebuffer readiness (ms) | 21.733 | 34.727 |
| Post-start read stalls >=50 ms | 0.000 | 0.000 |
| Post-start read stall time (ms) | 0.000 | 0.000 |
| Post-start verified-data waiting (ms) | 25.298 | 18.125 |
| Average verified forward buffer (s) | 0.833 | 20.003 |
| Peak managed allocation (MiB) | 0.781 | 20.625 |
| Sampled slot utilization (%) | 86.000 | 77.400 |
| Torrent rebuilds | 7.000 | 3.000 |
| Average rebuild duration (ms) | 1.031 | 6.152 |
| Maximum rebuild duration (ms) | 1.952 | 9.333 |
| Seek-request read recovery (ms) | 0.240 | 0.216 |
| Useful verified delivery (MiB/s) | 7.137 | 52.682 |

## unstable

| Metric | Baseline | Adaptive |
|---|---:|---:|
| First decoded frame (ms; VLC only) | N/A | N/A |
| Verified prebuffer readiness (ms) | 2,001.106 | 5,503.203 |
| Post-start read stalls >=50 ms | 1.000 | 0.000 |
| Post-start read stall time (ms) | 187.833 | 0.000 |
| Post-start verified-data waiting (ms) | 188.346 | 0.000 |
| Average verified forward buffer (s) | 0.622 | 7.246 |
| Peak managed allocation (MiB) | 0.781 | 20.625 |
| Sampled slot utilization (%) | 86.000 | 64.500 |
| Torrent rebuilds | 18.000 | 0.000 |
| Average rebuild duration (ms) | 1.062 | 0.000 |
| Maximum rebuild duration (ms) | 2.232 | 0.000 |
| Useful verified delivery (MiB/s) | 0.394 | 0.748 |

## vlc

| Metric | Baseline | Adaptive |
|---|---:|---:|
| First decoded frame (ms; VLC only) | 58.827 | 81.687 |
| Verified prebuffer readiness (ms) | 15.110 | 52.420 |
| Post-start read stalls >=50 ms | 0.000 | 0.000 |
| Post-start read stall time (ms) | 0.000 | 0.000 |
| Post-start verified-data waiting (ms) | 211.746 | 2.207 |
| Player buffering episodes | 0.000 | 0.000 |
| Player buffering time (ms) | 0.000 | 0.000 |
| Average verified forward buffer (s) | 0.045 | 1.424 |
| Peak managed allocation (MiB) | 0.312 | 15.219 |
| Sampled slot utilization (%) | 99.200 | 84.700 |
| Torrent rebuilds | 102.000 | 0.000 |
| Average rebuild duration (ms) | 0.701 | 0.000 |
| Maximum rebuild duration (ms) | 1.709 | 0.000 |
| Useful verified delivery (MiB/s) | 3.239 | 3.206 |

