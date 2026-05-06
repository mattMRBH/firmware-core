# sensirion-gas-index-algorithm

Vendored Sensirion gas-index algorithm (float variant) used by the SGP41
driver to convert raw VOC and NOx ticks into 1..500 index values.

## Status

`Vendored` — third-party source kept in sync with the upstream release.
Do not modify `sensirion_gas_index_algorithm.c` or
`sensirion_gas_index_algorithm.h` in-tree; if a behavioural change is
required, wrap it in the SGP41 driver instead.

## Provenance

| Field | Value |
|---|---|
| Upstream | <https://github.com/Sensirion/gas-index-algorithm> |
| Tag | `v3.2.0` |
| Variant | `sensirion_gas_index_algorithm/` (float) |
| License | BSD-3-Clause (Sensirion AG, 2022) |

The fixed-point variant
(`sensirion_gas_index_algorithm_fixpoint/`) is intentionally omitted —
ESP32 has an FPU and the float variant is sufficient.

## Files

| File | Source | Purpose |
|---|---|---|
| `sensirion_gas_index_algorithm.c` | upstream | Algorithm implementation |
| `sensirion_gas_index_algorithm.h` | upstream | Public C API |
| `LICENSE` | upstream | BSD-3-Clause (preserved verbatim) |
| `CMakeLists.txt` | local | `idf_component_register()` glue |

## Public API (summary)

```c
#include "sensirion_gas_index_algorithm.h"

GasIndexAlgorithmParams voc{}, nox{};
GasIndexAlgorithm_init_with_sampling_interval(
    &voc, GasIndexAlgorithm_ALGORITHM_TYPE_VOC, 10.0f);
GasIndexAlgorithm_init_with_sampling_interval(
    &nox, GasIndexAlgorithm_ALGORITHM_TYPE_NOX, 10.0f);

int32_t voc_index = 0, nox_index = 0;
GasIndexAlgorithm_process(&voc, voc_raw_ticks, &voc_index);
GasIndexAlgorithm_process(&nox, nox_raw_ticks, &nox_index);
```

The algorithm returns `0` during the first ~45 s (initial blackout) and
`1..500` afterwards. The host caller (the SGP41 driver) is responsible for
mapping `0` to the project's invalid sentinel.

## Consumers

- `components/airgradient-sensors/drivers/sgp41/` — sole in-tree consumer.

## Notes

- Sampling interval **must** match the cadence at which
  `GasIndexAlgorithm_process()` is called. Sensirion validates `1 s` and
  `10 s`; other intervals are unsupported.
- The algorithm keeps state in RAM only. Deep sleep (full RAM loss) resets
  the algorithm and re-triggers the 45 s blackout on the next
  initialisation.
- Float variant assumes a working FPU; do not enable on FPU-less targets
  without re-evaluating timing.

## License

BSD-3-Clause. See [`LICENSE`](LICENSE).
