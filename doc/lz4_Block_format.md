LZ4 Block Format Description
============================
Last revised: 2022-02-02.
Author : Yann Collet


This specification is intended for developers
willing to produce LZ4-compatible compressed data blocks
using any programming language.

LZ4 is an LZ77-type compressor with a fixed, byte-oriented encoding.
There is no entropy encoder back-end nor framing layer.
The latter is assumed to be handled by other parts of the system
(see [LZ4 Frame format]).
This design is assumed to favor simplicity and speed.
It helps later on for optimizations, compactness, and features.

This document describes only the block format,
not how the compressor nor decompressor actually work.
The correctness of the decompressor should not depend
on implementation details of the compressor, and vice versa.

[LZ4 Frame format]: lz4_Frame_format.md



Compressed block format
-----------------------
An LZ4 compressed block is composed of sequences.
A sequence is a suite of literals (not-compressed bytes),
followed by a match copy.

Each sequence starts with a `token`.
The `token` is a one byte value, separated into two 4-bits fields.
Therefore each field ranges from 0 to 15.


The first field uses the 4 high-bits of the token.
It provides the length of literals to follow.

If the field value is 0, then there is no literal.
If it is 15, then we need to add some more bytes to indicate the full length.
Each additional byte then represent a value from 0 to 255,
which is added to the previous value to produce a total length.
When the byte value is 255, another byte must read and added, and so on.
There can be any number of bytes of value "255" following `token`.
There is no "size limit".
(Side note : this is why a not-compressible input block is expanded by 0.4%).

Example 1 : A literal length of 48 will be represented as :

  - 15 : value for the 4-bits High field
  - 33 : (=48-15) remaining length to reach 48

Example 2 : A literal length of 280 will be represented as :

  - 15  : value for the 4-bits High field
  - 255 : following byte is maxed, since 280-15 >= 255
  - 10  : (=280 - 15 - 255) ) remaining length to reach 280

Example 3 : A literal length of 15 will be represented as :

  - 15 : value for the 4-bits High field
  - 0  : (=15-15) yes, the zero must be output

Following `token` and optional length bytes, are the literals themselves.
They are exactly as numerous as previously decoded (length of literals).
It's possible that there are zero literals.


Following the literals is the match copy operation.

It starts by the `offset`.
This is a 2 bytes value, in little endian format
(the 1st byte is the "low" byte, the 2nd one is the "high" byte).

The `offset` represents the position of the match to be copied from.
For example, 1 means "current position - 1 byte".
The maximum `offset` value is 65535, 65536 and beyond cannot be coded.
Note that 0 is an invalid offset value.
The presence of such a value denotes an invalid (corrupted) block.

Then the `matchlength` can be extracted.
For this, we use the second token field, the low 4-bits.
Such a value, obviously, ranges from 0 to 15.
However here, 0 means that the copy operation will be minimal.
The minimum length of a match, called `minmatch`, is 4.
As a consequence, a 0 value means 4 bytes, and a value of 15 means 19+ bytes.
Similar to literal length, on reaching the highest possible value (15),
one must read additional bytes, one at a time, with values ranging from 0 to 255.
They are added to total to provide the final match length.
A 255 value means there is another byte to read and add.
There is no limit to the number of optional "255" bytes that can be present.
(Note: this points towards a maximum achievable compression ratio of about 250).

Decoding the `matchlength` reaches the end of current sequence.
Next byte will be the start of another sequence.
But before moving to next sequence,
it's time to use the decoded match position and length.
The decoder copies `matchlength` bytes from match position to current position.

In some cases, `matchlength` can be larger than `offset`.
Therefore, since `match_pos + matchlength > current_pos`,
later bytes to copy are not decoded yet.
This is called an "overlap match", and must be handled with special care.
A common case is an offset of 1,
meaning the last byte is repeated `matchlength` times.


End of block restrictions
-----------------------
There are specific restrictions required to terminate an LZ4 block.

1. The last sequence contains only literals.
   The block ends right after them.
2. The last 5 bytes of input are always literals.
   Therefore, the last sequence contains at least 5 bytes.
   - Special : if input is smaller than 5 bytes,
     there is only one sequence, it contains the whole input as literals.
     Empty input can be represented with a zero byte,
     interpreted as a final token without literal and without a match.
3. The last match must start at least 12 bytes before the end of block.
   The last match is part of the penultimate sequence.
   It is followed by the last sequence, which contains only literals.
   - Note that, as a consequence,
     an independent block < 13 bytes cannot be compressed,
     because the match must copy "something",
     so it needs at least one prior byte.
   - However, when a block can reference data from another block,
     it can start immediately with a match and no literal,
     therefore a block of exactly 12 bytes can be compressed.

When a block does not respect these end conditions,
a conformant decoder is allowed to reject the block as incorrect.

These rules are in place to ensure compatibility with
a wide range of historical decoders
which rely on these conditions in their speed-oriented design.

Additional notes
-----------------------
If the decoder will decompress data from any external source,
it is recommended to ensure that the decoder is resilient to corrupted data,
and typically not vulnerable to buffer overflow manipulations.
Always ensure that read and write operations
remain within the limits of provided buffers.
Test the decoder with fuzzers
to ensure it's resilient to improbable sequences of conditions.
Combine them with sanitizers, in order to catch overflows (asan)
or initialization issues (msan).
Pay some attention to offset 0 scenario, which is invalid,
and therefore must not be blindly decoded
(a naive implementation could preserve destination buffer content,
which could then result in information disclosure
if such buffer was uninitialized and still containing private data).
For reference, in such a scenario, the reference LZ4 decoder
clears the match segment with `0` bytes,
though other solutions are certainly possible.

The format makes no assumption nor limits to the way a compressor
searches and selects matches within the source data block.
Multiple techniques can be considered,
featuring distinct time / performance trade offs.
For example, an upper compression limit can be reached,
using a technique called "full optimal parsing", at very high cpu cost.
As long as the specified format is respected,
the result will be compatible and decodable by any compliant decoder.
