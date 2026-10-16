# Hardware captures

Sanitized frames from a physical BedJet, written by `scripts/captures.py` from a
Flipper CLI `log` capture. See [the bring-up runbook](../../docs/bringup.md#5-turning-a-log-into-fixtures).
`scripts/check.py --sanitizers` replays every `*.cap` file here through the
parser. Only real device frames belong here; synthetic vectors stay in
`tests/protocol_test.c`. Never edit a frame line to make a check pass.
