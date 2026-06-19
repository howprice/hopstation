# Spoilers

**Do not read this document if you intend to write a PlayStation emulator and wish to be pleasantly (or not so pleasantly) surprised along the way.**

---
## CPU

The RISC pipeline does not need to be emulated unless your aim is 100% accuracy and compatibility. 

However, the *load delay slots* and *branch delay slots* that occur because of the pipeline certainly do need to be emulated.

The R3000 is a RISC processor, so by definition has far fewer instructions than many of its predecessors (I'm looking at you Z80 and 68000). This makes it far quicker to get up and running than you might expect.

The instruction cache does not need to be emulated to support the vast majority of games.

The data cache is repurposed as a small fast scratch pad. The performance of this does not need to be emulated to support most games.

RISC assembly is designed to be written by compilers rather than humans, so most (all?) PlayStation games were written in C. This makes a traditional emulator disassembler and debugger far less valuable.

Exceptions and interrupts in delay slots must be handled carefully.

The CPU does not have the floating point coprocessor.

Some aspects of the CPU are not covered by the test suites.

## Sideloading

It is possible to run CPU and GPU test executables early in development without CDROM support by adding special case code to inject the executable into the system at the correct point in the BIOS.

## GPU

The PlayStation GPU is 2D!!! It renders 2D triangle and rectangles. GPU commands do not accept Z or W coords. 3D is achieved by using the GTE for vector maths operations and sending the 2D vertex coords and associated attributes to the GPU as commands.

The GPU does not have a depth buffer. Software is responsible for depth ordering.

VRAM is a nice big rectangle and the display buffers are just rectangles in it. Very handy for debugging!

The triangle rasterizer does not use the modern edge-function based approach; it uses DDA. This gives slightly different results, but it is almost unnoticeable in practice.

Accurate dithering is essential for games such as Silent Hill, else they look terrible!

## CDROM

The CDROM has its own CPU. Nobody bothers emulating this; they use high level emulation (HLE) instead. A scheduler is invaluable here to approximate the delays between command submission, acknowledgement and completion, as well as laser seek times. 

The CDROM has many, many commands and can be a lot of work to get working. It is the primary source of incompatibility in PlayStation emulators.

The commonly used .cue format for CD images looks deceptively simple, but is underspecified and very awkward to parse and process accurately. This can lead to many games not working due to incorrect implicit gaps.

It is possible to avoid cue files for quite some time by only using disc images which consist of a single .bin file; just ignore the .cue file prepend the bin file payload with a 2 second pregap to construct the addressable disc image.

Overall, CDROM was an unexpected headache and timesink.

An emulator with CPU, GPU, a little DMA and interrupts and a very basic CDROM should be able to run *Mortal Kombat 2* and *Earthworm Jim 2*. These games are both 2D, with no MDEC requirements; SPU can be stubbed out.

## MDEC

More like JPEG (single frame) decoding than MPEG (multiple frame) decoding.

Must be implemented exactly per the spec, or expect garbage.

MDEC decoding makes heavy use of DMA. It's a nice system to implement, because it pulls together many system components into a pipeline: CDROM -> DMA -> MDEC -> DMA -> GPU

*Megaman X4* and The *Bad Apple* demo/video disc are great test cases for MDEC.

## GTE

The GTE is a coprocessor, mainly used for transform and lighting of vertices which are sent to the GPU. The GTE has to be almost 100% exact for most 3D games to work without terrible visual artifacts. Amidog's GTE tests are absolutely essential here.

Floating point arithmetic is *not* available; everything is fixed point.

Once GTE is implemented, *Crash Bandicoot*, *Ridge Racer*, *Battle Arena Toshinden* and others should work.

## Timing

To get some/most games to work, timing is hardly important at all in a PlayStation emulator. Most devices are synchronized against each other. This was a massive shock after emulating 8 and 16 bit machines which usually require good per-cycle bus accuracy.

Most hobby emulators use 2 CPI (cycles per instruction) as approximate CPU timing.  DMA transfers can be instantaneous and most games will still work fine. The system does however have hardware timers, which must be implemented accurately.

GPU, DMA and MDEC operations can complete instantaneously.

PAL/NTSC timing does not matter. You can run a PAL game on an NTSC emulator.

## SPU

The SPU has many features and is a significant amount of work, but is very rewarding.

CDROM audio is quite separate from most of the other SPU features and can be implemented quickly and early. *Earthworm Jim 2* is a very good test case (but may well require cue parsing...).

The SPU plays samples; it does not generate waves.

The iconic PlayStation start-up sound is not a simple sample. It requires a lot of SPU features including the full reverb chain (you're going to do some DSP).


## Architecture

The PlayStation is a very modular system. Many devices act like little subsystems and are controlled by sending commands e.g. GPU, CDROM, controllers, memory cards. This has the advantage that the emulator can be built piece by piece (with a little stubbing out). For example, if a game is 2D and doesn't have video intros, then it is likely that it can be run without any GTE or MDEC implementation.

It is very possible to emulate the system on a single CPU host thread on a modern GPU and run at full speed, at least in an optimised build.

Logging is essential for debugging, especially when DMA and CDROM are involved.

## Testing

It is almost impossible to develop an accurate emulator without taking advantage of the test suites (see list in README.md). This makes much of the process TDD if you want games to run whether you like it or not.

Amidog's CPU and GTE tests are very comprehensive.

Using PeterLemon's tests really helps get an accurate GPU up an running.

## Summary

Completely different to 8 and 16 bit machine emulation. Compatibility very dependent on passing tests. Lots of time implementing features exactly as specified in the psx-spx spec pseudocode to avoid random failures. Lots of time looking through logs. More time spent debugging at a high, conceptual level rather than at a low, cycle-level. A very interesting machine and very much of its time.
