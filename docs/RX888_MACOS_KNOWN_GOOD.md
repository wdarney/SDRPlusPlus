# RX888 macOS known-good checkpoint

Validated on 2026-09-01 using `/Applications/SDR++-RX888-Recovery.app` built
from SDR++ commit `77c567cc46be765c301c32541d5a1b6046744a19`.

The attached RX888 MkII was confirmed to:

- start normally and display the waterfall;
- retain the ADC sample-rate control;
- switch from HF to VHF;
- switch from VHF back to HF; and
- repeat the HF/VHF transitions without requiring an application restart.

The bundle used the separately maintained SDDC driver checkpoint
`2c89b681bb896ac43f4357f53db5ef92ca195121`. The SDDC driver history is not
part of this SDR++ merge and must remain separately versioned.

This is a hardware-tested recovery checkpoint, not evidence that every SDR++
source, platform, or RX888 configuration has been regression-tested.
