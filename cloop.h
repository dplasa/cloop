#ifndef _COMPRESSED_LOOP_H
#define _COMPRESSED_LOOP_H

/* The cloop v2 file usually looks like this:         */
/* HEADER                                             */
/* #!/bin/sh                                          */
/* #V2.0 Format                                       */
/* ...padding up to CLOOP2_HEADROOM...                */
/* block_size (32bit number, network order)           */
/* num_blocks (32bit number, network order)           */
/* CLOOP BLOCK OFFSETS                                */
/* num_blocks * offsets (64bit, network order)        */
/* COMPRESSED BLOCK DATA FROM HERE ...                */
/* num_blocks * ?? (gzip block compressed format)     */

// DP: This preamble is misleading in every thinkable way (iso9660 - really?, /dev/cloop - really ?)
// Kept for compatibility reasons to produce v2 cloop files.
#define CLOOP_PREAMBLE "#!/bin/sh\n" \
                       "#V2.0 Format\n" \
                       "modprobe cloop file=$0 && mount -r -t iso9660 /dev/cloop $1\n" \
                       "exit $?\n"
// More details
#define CLOOP2_HEADROOM 128
#define CLOOP2_HEADERSIZE (CLOOP2_HEADROOM + 4 + 4)   /* 136 bytes      */
#define CLOOP2_SIGNATURE "V2.0"                       /* @ offset 0x0b  */
#define CLOOP2_SIG_OFFSET 0x0b

/*
 *  A cloop v3 file looks like this:
 *  [ ? Bytes Block 0 Compressed Data    ] 2)
 *  [ ? Bytes Block 1 Compressed Data    ]
 *  [   ...                              ]
 *  [ ? Bytes Block (n-1) Compressed Data]
 *  [ ? Bytes padding ...                ] 3)
 *  [ 4 Bytes Number of Blocks (n)       ] 4)
 *  [ 4 Bytes Block Uncompressed Size    ] 5)
 *  [ 8 Bytes Original File Size         ]
 *  [ 8 Bytes Block 0 Offset             ] 6)
 *  [ 8 Bytes Block 1 Offset             ]
 *  [   ...                              ]
 *  [ 8 Bytes Block (n-1) Offset         ]
 *  [ 8 Bytes Total Compressed Length    ] 7)
 *  [ 4 Bytes Block 0 Flags              ] 8)
 *  [ 4 Bytes Block 1 Flags              ]
 *  [   ...                              ]
 *  [ 4 Bytes Block (n-1) Flags          ]
 *  [ 8 Bytes Offset of 4) in cloop file ] 9)
 *  [ 4 Bytes File Format Version        ] 10)
 *  [ 8 Bytes v3 Signature               ] 1) see notes below.
 *
 *  Please note, that all compressed block offsets information
 *  are stored at the end of the file rather than at the beginning
 *  (as in v2), which simplifies handling a lot when creating
 *  compressed images on streams. The header becomes a footer :)
 *
 *  1) Format of 8 Bytes Signature
 *     The cloop v3 signature is the string "\nCLOOP3\n"
 *
 *  2) Block Compressed Data
 *     Contains the compressed data of a block. The size of a compressed
 *     block will always be less or equal to the size of an uncompressed
 *     block. If a block is incompressible (i.e. its compressed size
 *     is larger than its uncompressed size), that block will be stored
 *     uncompressed.
 *  3) Padding
 *     Padding on 4096-byte boundary. Content is unspecified.
 *  4) Number of Compressed Blocks
 *     Total number of compressed blocks in network order.
 *  5) Uncompressed Block Size
 *     Uncompressed size of every compressed block in network
 *     order. Should not be less than 4k (or overall compression
 *     will suffer), should not exceed 256k (or performance of
 *     file operations on compressed image will suffer).
 *  6) Block Offset
 *     Offset at which a compressed block starts relative to 
 *     beginning of file in network order.
 *     The size of a compressed block can be determined by subtracting
 *     two consecutive offsets, e.g.
 *     compressed block 3 size = block offset 4 - block offset 3
 *  7) Total Compressed Size
 *     Total size of all compressed blocks in network order. Used
 *     to determine the size of the last compressed block (n-1).
 *  8) Block Flags
 *     Attributes for a compressed flag in network order. Bit field
 *     containing 
 *     - CLOOP3_COMPRESSED - if block is compressed
 *     - if CLOOP3_COMPRESSED is set the compression algorithm used, 
 *       which can be one of: CLOOP3_COMPRESSOR_ZLIB, 
 *       CLOOP3_COMPRESSOR_LZO1X, CLOOP3_COMPRESSOR_XZ, CLOOP3_COMPRESSOR_LZ4
 *  9) The last 8 bytes contain the offset at which 4) starts in the
 *     cloop image file in network order.
 * 10) File Format Version
 *     Holds the file format version in network order. Current value is 3.
 */

#define CLOOP3_SIGNATURE "\nCLOOP3\n"    /* @ offset 0x00 from EOF */
#define CLOOP3_SIGNATURE_LEN  8

/*  Info struct with all relevant cloop image information  */
struct cloop_info
{
  u_int32_t file_version;  /* cloop file version            */
  u_int32_t block_size;    /* size of an uncompressed block */
  u_int32_t num_blocks;    /* number of compressed blocks   */
  loff_t original_size;    /* original size of all un-
                              compressed data               */
  loff_t * offsets;        /* all n block offsets + total 
                              compressed size (=n+1 offsets)*/
  u_int32_t* flags;        /* all n block flags             */
};

/*
 *  CLOOP3 flags for each compressed block
 *  Bit  Meaning if bit is set
 *    0  reserved
 *    1  zlib compression (or 7z compression)
 *    2  lz4 compression
 *    3  lzo compression
 *    4  xz compression
 */

#define CLOOP3_COMPRESSOR_ZLIB  (1<<1)
#define CLOOP3_COMPRESSOR_LZO1X (1<<2)
#define CLOOP3_COMPRESSOR_XZ    (1<<3)
#define CLOOP3_COMPRESSOR_LZ4   (1<<4)
#define CLOOP3_COMPRESSOR_ANY (CLOOP3_COMPRESSOR_ZLIB | CLOOP3_COMPRESSOR_LZO1X | CLOOP3_COMPRESSOR_XZ | CLOOP3_COMPRESSOR_LZ4)

/* Cloop suspend IOCTL */
#define CLOOP_SUSPEND 0x4C07

#endif /*_COMPRESSED_LOOP_H*/
