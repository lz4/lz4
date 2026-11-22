# LZ4 users : sources

Proof of LZ4 use for each entry of the former "LZ4 is used by" list (89 entries), and for candidates outside it.
Research of 2026-11-21 : every URL fetched and checked for the quoted evidence.
Links still citing LZ4 can be rechecked with `audit-ecosystem.py`.

Verdict : S = strong (official doc/code, active), W = weak, D = discontinued/dormant, X = no source / wrong entry

| Entry | Cat. | V | Source | Evidence | Notes |
|---|---|---|---|---|---|
| Linux | OS | S | https://github.com/torvalds/linux/blob/v6.12/lib/lz4/lz4_compress.c | kernel lib/lz4 | linux.org is a forum, not kernel.org ; also init/Kconfig KERNEL_LZ4 |
| FreeBSD | OS | S | https://docs.freebsd.org/en/books/handbook/zfs/ | "compression=on, which selects LZ4" | via OpenZFS |
| Illumos | OS | S | https://illumos.org/man/7/zpool-features | lz4_compress feature org.illumos | ZFS LZ4 came through illumos |
| coreboot | OS | S | https://github.com/coreboot/coreboot/blob/26.09/src/Kconfig | "Compress romstage and verstage with LZ4" | |
| SmartOS | OS | W | https://smartos.org/man/8/vmadm | zfs_compression "lz4" | via illumos ZFS ; smartos.org -> tritoncloud.io |
| Zephyr | OS | S | https://docs.zephyrproject.org/latest/develop/manifest/external/lz4.html | lz4 Zephyr module | name is "Zephyr" ; ramdisk.c includes lz4frame.h |
| OpenZFS | FS | S | https://openzfs.github.io/openzfs-docs/man/master/7/zfsprops.7.html | "default compression algorithm is ... lz4" | |
| SquashFS | FS | S | https://docs.kernel.org/filesystems/squashfs.html | "zlib, lz4, lzo, xz or zstd" | old sourceforge link 403 |
| EROFS | FS | S | https://source.android.com/docs/core/architecture/kernel/erofs | "By default, the compression scheme is lz4hc" | current link OK |
| Bcachefs | FS | S | https://bcachefs.org/Compression/ | "gzip, lz4, and zstd" | removed from mainline Linux 6.18 |
| HAMMER2 | FS | S | https://man.dragonflybsd.org/?command=hammer2&section=8 | "default compression to lz4" | |
| LessFS | FS | X | - | 1.7.0 source has no LZ4 (qlz, lzo, bzip, deflate, snappy) | wrong entry, dead 2013 |
| LeoFS | FS | D | https://github.com/leo-project/website/blob/7478ca110d838006b54e2974556e8e97550a3e04/leofs/blog-entry-3.html | "compressed with LZ4 in order to reduce network traffic" | site down, dormant since 2020 |
| GNU GRUB | FS | S | https://git.savannah.gnu.org/cgit/grub.git/tree/grub-core/fs/zfs/zfs.c?h=grub-2.14 | {"lz4", lz4_decompress} | checked in grub-2.14 tarball ; reads LZ4 ZFS pools |
| TrueNAS | Storage | S | https://www.truenas.com/docs/scale/datasets/datasetsscreens/ | "LZ4 - Generally recommended" | via OpenZFS |
| DataStax | Storage | S | https://docs.datastax.com/en/hyper-converged-database/1.1/manage/operations/configure-compression.html | "LZ4Compressor ... default" | via Cassandra ; IBM 2025 |
| Nimble Storage | Storage | X | - | none found | HPE Alletra now |
| Nexenta | Storage | S | https://info.nexenta.com/docs/products/nexentastor/NexentaStor-5-Product-Guide.pdf | "The default algorithm is LZ4" | DDN 2019 |
| Syneto OS | Storage | W | https://syneto.eu/docs/whitepaper-syfs-filesystem.html?lang=en | "LZ4 compression x1.6" (UI figure) | ZFS lineage |
| QuadStor | Storage | S | https://www.quadstor.com/vr5GUDTGer/ReleaseNotes.txt | "Support and use LZ4 compression" | |
| VDO | Storage | S | https://github.com/torvalds/linux/blob/v6.12/drivers/md/dm-vdo/data-vio.c#L1781 | LZ4_compress_default | current link (userspace tools) has no LZ4 |
| pgBackRest | Storage | S | https://pgbackrest.org/configuration.html#section-general/option-compress-type | "lz4 - lz4 compression format" | |
| Zram | Caching | S | https://docs.kernel.org/admin-guide/blockdev/zram.html | comp_algorithm "lzo [lz4]" | |
| HHVM | Caching | S | https://github.com/meta/hhvm/blob/2bb8db3c1678754eb8f426949f32ccfcf4ba1360/hphp/hack/src/heap/hh_shared.c#L1253 | LZ4_compress_default | Hack typechecker shared heap |
| PHP Zend Optimizer | Caching | X | - | opcache has no LZ4 | wrong entry ; product gone |
| BuildCache | Caching | S | https://gitlab.com/bits-n-bites/buildcache/-/blob/v0.33.2/README.md | "Compression with LZ4 or optionally ZSTD" | |
| Intel QAT | HW | S | https://github.com/intel/QATzip/blob/v2.0.0/README.md | QZ_LZ4, QAT Gen4+ | |
| Marvell Structera | HW | S | https://investor.marvell.com/news-events/press-releases/detail/1037/adding-and-replacing-marvell-unveils-second-generation-structera-x-industry-first-six-channel-cxl-3-2-memory-expanders-for-ai-inference | "inline LZ4 compression" | |
| Xilinx FPGA | HW | D | https://github.com/Xilinx/Vitis_Libraries/blob/v2025.1_update2/data_compression/L2/tests/lz4_compress/README.rst | "AMD LZ4 Compression" | no longer maintained from 2025.2 |
| Zeropoint | HW | S | https://www.zeropoint-tech.com/products/zerostorage | "standards-based LZ4 compression ... in hardware" | old Flash MX link 500 |
| CAST | HW | S | https://www.cast-inc.com/compression/lossless-data-compression | LZ4SNP-C | |
| IP Cores LZR4D | HW | S | https://ipcores.com/LZ4_ip_core_LZR4D.htm | LZ4 decompression core | |
| Hadoop | BigData | S | https://github.com/apache/hadoop/blob/rel/release-3.5.0/hadoop-common-project/hadoop-common/src/main/java/org/apache/hadoop/io/compress/Lz4Codec.java | "creates lz4 compressors/decompressors" | lz4-java, own framing |
| Cassandra | BigData | S | https://cassandra.apache.org/doc/latest/cassandra/managing/operating/compression.html | "default choice in Cassandra" | |
| HBase | BigData | S | https://hbase.apache.org/book.html#lz4.compression | "universally recommended option" | |
| Spark | BigData | S | https://spark.apache.org/docs/latest/configuration.html#compression-and-serialization | spark.io.compression.codec default lz4 | |
| Hustle | BigData | D | https://github.com/chango/hustle/blob/890793b996d5ba0660f4f16dd71c88abc86ae5b5/README.md | "bitmap indexes, lz4" | dead 2014 |
| Kafka | BigData | S | https://kafka.apache.org/44/configuration/topic-configs/#topicconfigs_compression.type | compression.type lz4 | |
| Lucene | Search | S | https://lucene.apache.org/core/10_5_0/core/org/apache/lucene/codecs/lucene90/Lucene90StoredFieldsFormat.html | "uses the LZ4 compression algorithm by default" | own Java rewrite |
| Solr | Search | W | https://github.com/apache/solr/blob/releases/solr/10.1.0/NOTICE.txt | Lucene's LZ4 rewrite | only through Lucene |
| Groonga | Search | S | https://groonga.org/docs/reference/commands/column_create.html#column-create-flags | COMPRESS_LZ4 | current anchor broken |
| MySQL | DB | S | https://dev.mysql.com/doc/refman/8.4/en/innodb-page-compression.html | "Zlib and LZ4" | 403 to scripts, checked on Wayback 2026-10-02 |
| RocksDB | DB | S | https://github.com/meta/rocksdb/wiki/Compression | "LZ4 is almost always better than Snappy" | repo moved to meta/ |
| Redis | DB | X | - | rdb.c uses lzf_compress, no lz4 | false claim (homepage hit was base64 image) |
| Delphix | DB | W | https://www.perforce.com/blog/pdx/zfs-raidz | "databases with LZ4 compression" | via ZFS, 2014 blog |
| infiniSQL | DB | D | https://github.com/infinisql/infinisql/blob/6e858e142196e20b6779e1ee84c4a501e246c1f8/docs/pubs/overview.xml#L374-L377 | "compresses these messages using LZ4" | dead 2014 |
| Sophia | DB | D | https://github.com/pmwkaa/sophia/blob/version_2.2/documentation/admin/compression.md | "lz4, zstd, none" | "no longer developed" |
| OlegDB | DB | D | https://olegdb.org/docs/0.1.6/en/documentation.html#lz4_compression | "uses the super-fast LZ4" | hobby project, inactive |
| GemStone | DB | S | https://downloads.gemtalksystems.com/docs/GemStone64/3.4.x/GS64-ReleaseNotes-3.4/2-AdministrationChanges.htm#pgfId-1803797 | "now supports lz4" | |
| Percona | DB | S | https://docs.percona.com/percona-xtrabackup/8.4/create-compressed-backup.html#lz4 | --compress=lz4 | XtraBackup (MongoRocks deprecated) |
| PostgreSQL | DB | S | https://www.postgresql.org/docs/current/runtime-config-client.html#GUC-DEFAULT-TOAST-COMPRESSION | "The default is lz4 (if available)" (v19) | current link OK |
| OpenVPN | Net | W | https://openvpn.net/community-docs/community-articles/openvpn-2-6-manual/ | "LZ4 generally offering the best performance" | compression off by default (VORACLE) |
| SimpleLink | Net | W | https://www.ti.com/en-us/design-development/educational-resources/video/5399267783001 | video title only (2017) | MSP432 maybe discontinued |
| Netty | Net | S | https://netty.io/4.2/api/io/netty/handler/codec/compression/Lz4FrameEncoder.html | "Compresses a ByteBuf using the LZ4 format" | lz4-java block stream, not LZ4 frame |
| dovecot | Net | S | https://doc.dovecot.org/latest/core/plugins/mail_compress.html | lz4 - liblz4 | |
| virtualHere | Net | W | https://www.virtualhere.com/embedded_server_license | license notice only | |
| linBit | Net | D | https://linbit.com/drbd-user-guide/drbd-guide-9_0-en/#s-drbd-proxy-3-plugins | lz4 plugin (Proxy v3) | removed in Proxy v4 (zstd only) |
| Embulk | Net | W | https://plugins.embulk.org/ | third-party embulk-decoder-lz4 | plugin stale 2017 |
| NeoMutt | Net | S | https://neomutt.org/guide/reference.html#header-cache-compress-method | header cache lz4 | |
| Nintendo Switch | Games | S | https://support.nintendo.com/jp/switch/intellectual-property/switch-family/switch.html | "LZ4 Copyright (c) 2011-2017, Yann Collet" | system software notices |
| Unity | Games | S | https://docs.unity3d.com/Manual/assetbundles-compression-format.html | "caches AssetBundles with LZ4" | |
| Hitman 3 | Games | S | https://www.pcgamer.com/how-hitman-3s-devs-shrank-the-entire-trilogys-install-size-by-over-80gb/ | IOI CTO quote | current link OK |
| Death Stranding | Games | X | - | none found | |
| Battlefield 4 | Games | X | - | none found | |
| Guild Wars 2 | Games | X | - | none found | |
| CRYENGINE (Crytek) | Games | S | https://www.cryengine.com/docs/static/engines/cryengine-5/categories/47316993/pages/87884191 | lz4 copyright notice | name it CRYENGINE |
| Black Ops 3 | Games | X | - | none found | |
| Black Ops Cold War | Games | W | https://youtu.be/DkspHgt27Io?t=712 | credits video (fan upload), unverified | someone must watch 11:52 |
| FIFA | Games | X | - | none found | |
| Sine Mora | Games | X | - | none found | |
| 1000 Tiny Claws | Games | X | - | none found | |
| NVIDIA | Graphics | S | https://docs.nvidia.com/cuda/nvcomp/ | nvCOMP LZ4 | own GPU implementation ; library notice : docs.nvidia.com/clara/parabricks/.../lz-4 |
| Enlightenment | Graphics | S | https://www.enlightenment.org/_legacy_embed/group__Eet__Compression.html | EET_COMPRESSION_SUPERFAST (LZ4) | legacy docs page |
| Xpra | Graphics | S | https://github.com/Xpra-org/xpra/blob/v7.0/docs/Network/Protocol.md?plain=1#L172 | "MUST implement rencodeplus and LZ4" | |
| OpenVDB | Graphics | S | https://github.com/AcademySoftwareFoundation/openvdb/blob/v13.0.0/openvdb/openvdb/io/Compression.cc#L187 | BLOSC_LZ4_COMPNAME | via Blosc |
| PixInsight | Graphics | S | https://pixinsight.com/doc/docs/XISF-1.0-spec/XISF-1.0-spec.html | "zlib, LZ4, LZ4_HC and Zstandard" | |
| Firefox | Other | S | https://github.com/mozilla-firefox/firefox/blob/893a7ba9f78f4680d50b1485c3b703bbfddda940/toolkit/mozapps/extensions/AddonManagerStartup.cpp#L204 | "LZ4-compressed file" | mozglue/static/lz4 |
| SVN | Other | S | https://subversion.apache.org/docs/release-notes/1.10.html | "LZ4 compression is now used by default" | |
| Bareos | Other | S | https://docs.bareos.org/Configuration/Director.html#config-Dir_Fileset_Include_Options_Compression | "LZ4 is the best choice" | |
| GTKWave | Other | S | https://gtkwave.github.io/gtkwave/man/vcd2fst.1.html | "LZ4 should be used ... (default)" | moved from sourceforge |
| Blosc | Other | S | https://www.blosc.org/python-blosc2/getting_started/overview.html | "codecs (LZ4, BloscLZ, ...)" | |
| Nippy | Other | S | https://github.com/taoensso/nippy | "compression with LZ4 or Zstandard" | airlift pure-Java LZ4 |
| systemd | Other | S | https://systemd.io/JOURNAL_FILE_FORMAT/ | HEADER_INCOMPATIBLE_COMPRESSED_LZ4 | |
| gdsfmt | Other | S | https://bioconductor.org/packages/release/bioc/vignettes/gdsfmt/inst/doc/gdsfmt.html | "ZIP, ... LZ4, LZ4_ra" | |
| Logscape | Other | D | https://github.com/logscape/Logscape/blob/fde9133797b8f1d03f824c65e26e352ae5677063/common/src/com/liquidlabs/common/file/raf/RafFactory.java#L52 | .lz4 -> Lz4Raf | dead 2021 |
| vim | Other | S | https://github.com/vim/vim/blob/v9.2.1373/runtime/plugin/gzip.vim#L31 | *.lz4 -> "lz4 -d -q --rm" | current link = 3-star third-party plugin ; vim calls the lz4 CLI since 2025 |
| FST (R) | Other | S | http://www.fstpackage.org/ | "LZ4 and ZSTD compression" | last CRAN release 2022 |
| ASDF | Other | S | https://www.asdf-format.org/projects/asdf/en/latest/asdf/arrays.html | "lz4 compression algorithm is also supported" | optional |
| Emscripten | Other | S | https://emscripten.org/docs/tools_reference/settings_reference.html#lz4 | "lz4-compressed file packages" | mini-lz4.js |

## Candidates outside the old list (checked 2026-11-21)

| Entry | Cat. | V | Source | Evidence | Notes |
|---|---|---|---|---|---|
| macOS, iOS | OS | S | https://developer.apple.com/documentation/compression/compression_lz4 | "compatible with the open-source version, apart from ... a very simple frame" | Compression framework ; iOS, iPadOS, macOS, tvOS, visionOS, watchOS (JSON data endpoint checked) |
| zswap | OS (Linux) | S | https://github.com/torvalds/linux/blob/v6.12/mm/Kconfig | ZSWAP_COMPRESSOR_DEFAULT_LZ4 "Use the LZ4 algorithm as the default" | build-time choice ; docs page has no lz4 |
| Parquet | Formats | S | https://github.com/apache/parquet-format/blob/master/Compression.md | LZ4_RAW : "the implementation provided by the LZ4 compression library is authoritative" | old LZ4 codec deprecated (Hadoop framing) |
| Arrow | Formats | S | https://arrow.apache.org/docs/format/Columnar.html | IPC buffers "compressed with the lz4 compression codec" = LZ4 frame format | |
| ORC | Formats | S | https://orc.apache.org/specification/ORCv1/ | CompressionKind LZ4 = 4 | enum only |
| ClickHouse | DB | S | https://clickhouse.com/docs/sql-reference/statements/create/table | "By default, ClickHouse applies lz4 compression in the self-managed version" | zstd in ClickHouse Cloud |
| BorgBackup | Backup | S | https://borgbackup.readthedocs.io/en/stable/usage/help.html | "Compression is lz4 by default." | |
| Hugging Face Xet | AI | S | https://huggingface.co/docs/xet/xorb | chunks as LZ4 frame, or ByteGrouping4LZ4 for floating-point data | model / dataset storage |

## Packet capture (checked 2026-11-22)

| Entry | Cat. | V | Source | Evidence | Notes |
|---|---|---|---|---|---|
| Suricata | Network | S | https://docs.suricata.io/en/latest/configuration/suricata-yaml.html#packet-log-pcap-log | pcap-log : "The pcap files can be compressed before being written to disk by setting the compression option to lz4" | lz4-level 0-16, lz4-checksum |
| Wireshark | Network | S | https://www.wireshark.org/docs/relnotes/wireshark-4.4.0.html#_new_and_updated_features | "Capture files can be saved with LZ4 compression" ; fast random access with independent blocks (default) | tshark, editcap, mergecap, text2pcap --compress lz4 |
| Arkime | Network | X | https://arkime.com/settings | simpleCompression : none, gzip, zstd | no lz4 |
