Helix MP3 + AAC fixed-point decoders (RealNetworks; RPSL/RCSL, see the
license files in each directory), vendored from
github.com/pschatzmann/arduino-libhelix @ f6e55a307c7577c5e3dfe1bf424a61075235fa08
(src/libhelix-mp3, src/libhelix-aac, src/utils/helix_{pgm,memory}.h).
Only ConfigHelix.h and helix_memory.c are ours, plus one line in each
assembly.h (`defined(__unix__)` for the Chumby and `defined(__arm__)` for the
Playdate's Cortex-M7 added to the portable-C branch) (the wrapper's C++ config and
allocator replaced with plain C). Free-format MP3 is rejected in
UnpackFrameHeader (bitstream.c): seeking into a file lands on false syncs, and one
that claims free format corrupts the heap or copies (size_t)-1 bytes. Built with the portable __unix__ path of
assembly.h: clang emits smull/smlal for the 64-bit multiplies on ARMv5TE.

Two additions for the Playdate, which plays mono and is short of CPU:
- `MP3SetMonoOutput` (mp3dec.c/.h, mp3common.h, subband.c, polyphase.c,
  imdct.c, coder.h): mixes stereo before the synthesis filterbank so it runs
  once instead of twice. With `halfRate`, it also drops subbands 16-31 and
  outputs every other sample (`PolyphaseMonoHalf`), at half the sample rate.
- `AACSkipSBR` (aacdec.c/.h, aaccommon.h, decelmnt.c): ignores HE-AAC's SBR
  data, so only the core band is decoded, at the core sample rate.
