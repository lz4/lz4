# LZ4 Streaming API Example: Line by Line Text Compression
by *Takayuki Matsuoka*

[`blockStreaming_lineByLine.c`](blockStreaming_lineByLine.c) is an example of
the LZ4 Streaming API where we implement line by line incremental
(de)compression.

Please note the following restrictions:

- Firstly, read ["LZ4 Streaming API Basics"](streaming_api_basics.md).
- This is a relatively advanced application example.
- The output file is not compatible with lz4frame and is platform dependent.


## What's the point of this example?

The LZ4 Streaming API can be used to handle compressing a huge file in a small
amount of memory. This example shows how to use a "Ring Buffer" to compress
blocks (i.e. chunks) of a non-uniform size in a stream. The Streaming API used
in this way generally yields a better compression ratio than the regular Block
API.

For an example with uniform blocks, see ["LZ4 Streaming API Example: Double
Buffer"](blockStreaming_doubleBuffer.md).

## How compression works

Firstly, allocate "Ring Buffer" for input and "compressed data buffer" for output.

```
(1)
    Ring Buffer

    +--------+
    | Line#1 |
    +---+----+
        |
        v
     {Out#1}
```

Next (see (1)), read the first line to the ring buffer and compress it with
`LZ4_compress_continue()`. On the first compression, LZ4 doesn't have any
previous dependencies, so it just compresses the line without dependencies and
writes the compressed line `{Out#1}` to the compressed data buffer. After that,
write `{Out#1}` to the file and shift the ring buffer offset forward.

```
(2)
    Prefix Mode Dependency
          +----+
          |    |
          v    |
    +--------+-+------+
    | Line#1 | Line#2 |
    +--------+---+----+
                 |
                 v
              {Out#2}

(3)
          Prefix   Prefix
          +----+   +----+
          |    |   |    |
          v    |   v    |
    +--------+-+------+-+------+
    | Line#1 | Line#2 | Line#3 |
    +--------+--------+---+----+
                          |
                          v
                       {Out#3}
```

Repeat the above for the second line (see (2)). Repeat again for the third line
(see (3)). However,this time LZ4 can use the dependency on `Line#1` (and then on
the third invocation, the dependency on both `Line#2` and `Line#1`) to improve
the compression ratio. This dependency is called "Prefix mode".

```
(4)
                        External Dictionary Mode
                +----+   +----+
                |    |   |    |
                v    |   v    |
    ------+--------+-+------+-+--------+
          |  ....  | Line#X | Line#X+1 |
    ------+--------+--------+-----+----+
                            ^     |
                            |     v
                            |  {Out#X+1}
                            |
                          Reset
```

Eventually, we'll reach the end of the ring buffer at `Line#X` (see (4)). This
time, we reset the ring buffer offset. After resetting, the pointer to
`Line#X+1` is no longer adjacent to `Line#X`, but LZ4 still maintains its memory
of it. This is called "External Dictionary Mode".

```
(5)
                                    Prefix
                                    +-----+
                                    |     |
                                    v     |
    ------+--------+--------+----------+--+-------+
          |  ....  | Line#X | Line#X+1 | Line#X+2 |
    ------+--------+--------+----------+-----+----+
                            ^                |
                            |                v
                            |            {Out#X+2}
                            |
                          Reset
```

In `Line#X+2` (see (5)), LZ4 finally clears all lines except `Line#X+1` from the
ring buffer. This is the same situation as `Line#2`.

Continue this procedure till the end of the text file.


## How decompression works

Decompression follows the reverse order.

- Read compressed line from the input file to buffer.
- Decompress it to the ring buffer.
- Write the decompressed plain text line to the output file.
- Shift the ring buffer offset forward. If the offset exceeds end of the ring
  buffer, reset it.

Continue this procedure till the end of the compressed file.
