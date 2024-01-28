/*
    lorem.c - lorem ipsum generator
    Copyright (C) Yann Collet 2024

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
   - Public forum : https://groups.google.com/forum/#!forum/lz4c
*/

/* Implementation notes:
 *
 * This is a very simple lorem ipsum generator
 * which features a static list of words
 * and print them one after another randomly
 * with a fake sentence / paragraph structure.
 *
 * It would be possible to create some more complex scheme,
 * notably by enlarging the dictionary of words with a word generator,
 * inventing grammatical rules (composition) and syntax rules.
 * But that's probably overkill for the intended goal.
 *
 * The goal is to generate a text which can be printed,
 * and can be used to fake a text compression scenario.
 * The resulting compression / ratio curve of the lorem ipsum generator
 * is more satisfying than the existing statistical generator,
 * which was initially designed for entropy compression,
 * but lacks a regularity more representative of text,
 * which more properly tests the LZ compression part.
 */

#include "platform.h"  /* Compiler options, SET_BINARY_MODE */
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

#define WORD_MAX_SIZE 20

/* Define the word pool */
const char *words[] = {
    "lorem",       "ipsum",      "dolor",      "sit",          "amet",
    "consectetur", "adipiscing", "elit",       "sed",          "do",
    "eiusmod",     "tempor",     "incididunt", "ut",           "labore",
    "et",          "dolore",     "magna",      "aliqua",       "dis",
    "lectus",      "vestibulum", "mattis",     "ullamcorper",  "velit",
    "commodo",     "a",          "lacus",      "arcu",         "magnis",
    "parturient",  "montes",     "nascetur",   "ridiculus",    "mus",
    "mauris",      "nulla",      "malesuada",  "pellentesque", "eget",
    "gravida",     "in",         "dictum",     "non",          "erat",
    "nam",         "voluptat",   "maecenas",   "blandit",      "aliquam",
    "etiam",       "enim",       "lobortis",   "scelerisque",  "fermentum",
    "dui",         "faucibus",   "ornare",     "at",           "elementum",
    "eu",          "facilisis",  "odio",       "morbi",        "quis",
    "eros",        "donec",      "ac",         "orci",         "purus",
    "turpis",      "cursus",     "leo",        "vel",          "porta"};
const int wordCount = sizeof(words) / sizeof(words[0]);

static char *g_ptr = NULL;
static size_t g_nbChars = 0;
static size_t g_maxChars = 10000000;

static unsigned g_randRoot = 0;

#define RDG_rotl32(x, r) ((x << r) | (x >> (32 - r)))
static unsigned int LOREM_rand(void) {
  static const unsigned prime1 = 2654435761U;
  static const unsigned prime2 = 2246822519U;
  unsigned rand32 = g_randRoot;
  rand32 *= prime1;
  rand32 ^= prime2;
  rand32 = RDG_rotl32(rand32, 13);
  g_randRoot = rand32;
  return rand32;
}

static void writeLastCharacters(void) {
  size_t lastChars = g_maxChars - g_nbChars;
  assert(g_maxChars >= g_nbChars);
  if (lastChars == 0)
    return;
  g_ptr[g_nbChars++] = '.';
  if (lastChars > 1) {
    memset(g_ptr + g_nbChars, ' ', lastChars - 1);
  }
  g_nbChars = g_maxChars;
}

static void generateWord(const char *word, const char *separator) {
  size_t const len = strlen(word) + strlen(separator);
  if (g_nbChars + len > g_maxChars) {
    writeLastCharacters();
    return;
  }
  memcpy(g_ptr + g_nbChars, word, strlen(word));
  g_nbChars += strlen(word);
  memcpy(g_ptr + g_nbChars, separator, strlen(separator));
  g_nbChars += strlen(separator);
  // fprintf(stderr, "print %s (%i) \n", word, g_nbChars);
}

static const char* upWord(char *dst, const char *src) {
  size_t len = strlen(src);
  int toUp = 'A' - 'a';
  assert(len < WORD_MAX_SIZE);
  memcpy(dst, src, len);
  dst[0] += toUp;
  dst[len] = 0;
  return dst;
}

static int about(unsigned target) {
  return (int)((LOREM_rand() % target) + (LOREM_rand() % target) + 1);
}

/* Function to generate a random sentence */
static void generateSentence(int nbWords) {
  char upWordBuff[WORD_MAX_SIZE];
  int commaPos = about(9);
  int comma2 = commaPos + about(7);
  int i;
  for (i = 0; i < nbWords; i++) {
    const char *word = words[LOREM_rand() % wordCount];
    const char* sep = " ";
    if (i == 0) {
      word = upWord(upWordBuff, word);
    }
    if (i == commaPos)
      sep = ", ";
    if (i == comma2)
      sep = ", ";
    if (i == nbWords - 1)
      sep = ". ";
    generateWord(word, sep);
  }
}

static void generateParagraph(int nbSentences) {
  int i;
  for (i = 0; i < nbSentences; i++) {
    int wordsPerSentence = about(8);
    generateSentence(wordsPerSentence);
  }
  if (g_nbChars < g_maxChars) {
    g_ptr[g_nbChars++] = '\n';
  }
  if (g_nbChars < g_maxChars) {
    g_ptr[g_nbChars++] = '\n';
  }
}

/* It's "common" for lorem ipsum generators to start with the same first
 * pre-defined sentence */
static void generateFirstSentence(void) {
  char buffer[WORD_MAX_SIZE];
  int i;
  for (i = 0; i < 18; i++) {
    const char *word = words[i];
    const char *separator = " ";
    if (i == 0)
      word = upWord(buffer, word);
    if (i == 4)
      separator = ", ";
    if (i == 7)
      separator = ", ";
    generateWord(word, separator);
  }
  generateWord(words[18], ". ");
}

static size_t
LOREM_genBlock(void *buffer, size_t size,
                unsigned seed,
                int first, int fill)
{
  g_ptr = (char*)buffer;
  assert(size < INT_MAX);
  g_maxChars = size;
  g_nbChars = 0;
  g_randRoot = seed;
  if (first) {
    generateFirstSentence();
  }
  while (g_nbChars < g_maxChars) {
    int sentencePerParagraph = about(6);
    generateParagraph(sentencePerParagraph);
    if (!fill)
      break; /* only generate one paragraph in not-fill mode */
  }
  return g_nbChars;
}

void LOREM_genBuffer(void* buffer, size_t size, unsigned seed)
{
  LOREM_genBlock(buffer, size, seed, 1, 1);
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define LOREM_BLOCKSIZE (1 << 10)
void LOREM_genOut(unsigned long long size, unsigned seed)
{
  char buff[LOREM_BLOCKSIZE + 1] = {0};
  unsigned long long total = 0;
  size_t genBlockSize = MIN(size, LOREM_BLOCKSIZE);

  /* init */
  SET_BINARY_MODE(stdout);

  /* Generate data, per blocks */
  while (total < size) {
    size_t generated = LOREM_genBlock(buff, genBlockSize, seed++, total == 0, 0);
    assert(generated <= genBlockSize);
    total += generated;
    assert(total <= size);
    fwrite(buff, 1, generated,
           stdout); /* note: should check potential write error */
    if (size - total < genBlockSize)
      genBlockSize = (size_t)(size - total);
  }
  assert(total == size);
}
