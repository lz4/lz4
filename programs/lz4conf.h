/*
  LZ4conf.h - compile-time parameters
  Copyright (C) Yann Collet 2011-2024
  GPL v2 License

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

  You can contact the author at :
  - LZ4 source repository : https://github.com/lz4/lz4
  - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/

#ifndef LZ4CONF_H_32432
#define LZ4CONF_H_32432


/* Determines if multithreading is enabled or not
 * Default: disabled */
#ifndef LZ4IO_MULTITHREAD
# define LZ4IO_MULTITHREAD 0
#endif

/* Determines default nb of threads for compression
 * Default value is 0, which means "auto" :
 * nb of threads is determined from detected local cpu.
 * Can also be selected at runtime using -T# command */
#ifndef LZ4_NBWORKERS_DEFAULT
# define LZ4_NBWORKERS_DEFAULT 0
#endif

/* Maximum nb of compression threads that can selected at runtime */
#ifndef LZ4_NBWORKERS_MAX
# define LZ4_NBWORKERS_MAX 125
#endif

/* Determines default lz4 block size when none provided.
 * Default value is 7, which represents 4 MB.
 * Can also be selected at runtime using -B# command */
#ifndef LZ4_BLOCKSIZEID_DEFAULT
# define LZ4_BLOCKSIZEID_DEFAULT 7
#endif


#endif  /* LZ4CONF_H_32432 */
