# Compatibility

Games tested are listed here.

🟢 Perfect
🟡 Minor issues
🟠 Major issues
🔴 Unplayable

| Game | Status | Notes |
|------|--------|-------|
| Ape Escape | 🔴 | Requires CDROM mute command |
| Battle Arena Toshinden | 🟢 | Easy 3D game to emulate | 
| Bubble Bobble II | 🟢 | Drumroll audio in intro sounds corrupt but is actually just a bad repeating sample! | 
| Castlevania: Symphony of the Night | 🟡 |  | 
| Crash Bandicoot  | 🟢 |  Easiest 3D game to emulate | 
| Crash Bandicoot 2 - Cortex Strikes Back | 🟢 | Sets GPUSTAT.11 Texture page Y Base 2 (N*512) (only for 2 MB VRAM i.e. arcade, not PSX)" | 
| CTR: Crash Team Racing | 🟢 | Requires DMA4 - SPU to RAM direction transfers. | 
| Doom | 🔴 | Required MDEC/DMA fixes. Hangs on loading screen. Reportedly requires accurate CDROM timing. |
| Driver | 🟡 | | 
| Earthworm Jim 2 | 🟢 | CDDA. No GTE, MDEC | 
| Einhander | 🟡 | Missing transparancy on explosions | 
| Final Fantasy VII | 🟡 |  Cloud's sword sound effect uses noise channel to modulate subsequent channel pitch.. Reverb. SPU voice noise, pitch modulation and dynamic voice address change |
| Gradius Gaiden | 🟢 | | 
| Gran Turismo | 🟡 | Intro music sounds like a bad loop, but this is as designed!, Intro CD music skips (Seek status pulses). |
| Gran Turismo 2 | 🟡 | 16-bit read from 1f801130, which is past end of Timers. Uses 3x24-bit framebuffers for the intro (GP1(05h) start X addresses VRAM in halfwords, not pixels ) |
| Heart of Darkness | 🟠 | May use CPU to VRAM CLUT DMAs with odd width (like MGS) CDROM Pause assert. No sound in game. |
| Intelligent Qube | 🟢 | Possible MDEC buffer overflow. |
| Megaman X4 | 🟢 | Good MDEC. CD XA. test case | 
| Metal Gear Solid | 🟡 | IRQ9 required at boot. CPU to VRAM CLUT DMAs with odd width (37) | 
| Mortal Kombat | 🟢 | Easiest game to emulate. No GTE, CDDA, CDXA | 
| Oddworld: Abe's Oddysee | 🟡 |  MDEC buffer overflow. |
| Pac-Man World | 🟡 | Audio sounds very muffled in game |
| Parodius | 🟡 |  | 
| Puzzle Bobble 2 (Japan) | 🟠 | CDROM asserts and corrupt sounding audio |
| Rayman 2: The Great Escape | 🟢 | | 
| Resident Evil | 🟢 | Requires mono XA-ADPCM decoding as soon as get in game. | 
| Resident Evil 2 | 🟢 | 2nd best horror. Only given it a quick go. | 
| Salamander Deluxe Pack Plus | 🟢 |  | 
| Sexy Parodius | 🟢 | Attempts to read past end of RAM - requires RAM address mask | 
| Silent Hill | 🟢 | Invalid GPU commands | 
| Skullmonkeys | 🟡 |  |
| Spyro | 🟡 | Good analogue controller test case | 
| Street Fighter Alpha 3  | 🟡 |  | 
| Super Puzzle Fighter II Turbo | 🟡 | Intro movie seems to be skipped, but can be heard a little. CDROM ReadS1 assert when setting Stat,Seek. |
| Syphon Filter | 🔴 | CDROM stat read/seek/play assert. Loader fails. CDROM stuck in loop. |
| Tekken | 🟢 |  | 
| Tekken 2 | 🟡 | Infinite GPU2 linked list DMA | 
| Tekken 3 | 🟢 |  | 
| The Raiden Project | 🟡 | Sets GP0(E1h) TEXPAGE bit 11, which is usually only for arcade boards using 2 MB RAM. Displays garbage to right of narrow (tate?) display. |
| Tomb Raider | 🟠 | Requires CDROM GetLocP |
| Tomba! | 🟢 | Poor performance in debug build |
| Tony Hawk's Pro Skater | 🟡 | Uses 18900 Hz (half rate) CDROM XA-ADPCM audio for Activision intro. Requires CDROM Mute command. |
| Tony Hawk's Pro Skater 2 | 🔴 | Requires CDROM Mute command. |
| Wipeout | 🟠 | MDEC required fixes. GetLocP assert. CDROM Report required. CDROM stat read/seek/play assert. Apparantly requires accurate CDROM timing |
| Wipeout 2/XL | 🔴 | Requires SPU volume sweep |
| X2: No Relief | 🟠 | Team 17 / Ocean Project-X sequel | Constant CDROM status read/seek/play bit asserts |