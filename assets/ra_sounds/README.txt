These notification sounds are generated originals, not third-party assets.

They are synthesised by tools/make_sfx.py (sine / triangle / pulse partials,
exponential envelopes, one-pole low-pass) and dedicated to the public domain
(CC0 1.0). The build regenerates them before packaging, so no binary audio needs
to be tracked:

    python tools/make_sfx.py --style clean --out third_party/Adrenaline/user/ra_sounds

Format: 48 kHz, 16-bit stereo PCM, which is what the notification renderer plays.
Styles: clean (shipped: sine, shorter, modern), warm (triangle, softer), retro
(pulse-wave, PSP-era chiptune) - see docs/48 for what each one sounds like.

