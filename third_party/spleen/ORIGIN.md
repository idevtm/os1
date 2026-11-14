# Spleen

Spleen is a monospaced bitmap font by Frederic Cambus, under the BSD 2-Clause license.
The files in this directory are from <https://github.com/fcambus/spleen>, commit `cfde41205b7589ac29f09ffd9b230d8a9f8702c7` of the `master` branch, without changes.
That commit is later than release 2.2.0. Its `spleen-5x8.bdf` and `spleen-6x12.bdf` are the same as in release 2.2.0. The other four files are different.
See [LICENSE](LICENSE).
The build converts the BDF files to the os1 font file format with `cmake/scripts/make_font.py`.

SHA-256 of each file:

```text
40488184d075d0c752cdd239b441c5ece51e50b353156f2496c756c384ab01cb  spleen-5x8.bdf
fc0743d164690f99b7e2e1b9d503180e4c719a9831ae03fd8f6da18c857dee27  spleen-6x12.bdf
4f741d93f2a01c3898b73ab438d3fab3ff7574afe1cbd96e9894b8b46eeaba86  spleen-8x16.bdf
e4cb24e53489893fa1e7a8fde51b053995b37b8aac157c604018c01c2937552b  spleen-12x24.bdf
31f40cca4450e5ffbe297f10fd59f3a92dcf5b0260a8d2a235fb4f785bf82bc4  spleen-16x32.bdf
cdcbddcabb745ac2daeac4e4ae59cdb3291e55a71d61ad5b88a4c5ed09ddf8ce  spleen-32x64.bdf
f33fe8679d5b2abecc4f1313ce6c6bfa58262964de5f7bca146596a7318047af  LICENSE
```
