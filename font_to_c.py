#!/usr/bin/python2

# Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Converts a font in bdf format into C source code."""

from __future__ import print_function
import re
import sys

class GlyphSet(object):
  """Collects glyph bitmap data and outputs it into C source code"""
  def __init__(self, width, height):
    self.glyph_map = {}
    self.width = width
    self.height = height
    self.bits_per_pixel = 1
    self.bytes_per_row = (self.bits_per_pixel * self.width + 7) // 8
    self.glyph_size = self.bytes_per_row * self.height

  def AddGlyph(self, code_point, data):
    """Adds a bitmap associated with the glyph identified by the code point.

    A glyph can be added at most once.

    Args:
      code_point: a 32-bit unsigned integer identifying the code point of the
          glyph bitmap
      data: an array of unsigned bytes with a length of exactly self.glyph_size

    Raises:
      Exception: the bitmap data is the wrong size or the code point was added
          once before
    """
    if code_point in self.glyph_map:
      raise Exception('code point {} already added'.format(code_point))

    if len(data) != self.glyph_size:
      raise Exception('given glyph is the wrong size, expected {}, got {}'
                      .format(self.glyph_size, len(data)))

    self.glyph_map[code_point] = data

  def ToCSource(self, out_file):
    """Writes this GlyphSet's data into a C source file.

    The data written includes:
      - the global dimensions of the glyphs
      - the glyph bitmaps, stored in an array
      - a function to convert code points to the index of the glyph in the
          bitmap array

    The C source file outputs static data and methods and is intended to be
    #include'd by a compilation unit.

    Args:
      out_file: the file to write the GlyphSet to
    """
    print('#define GLYPH_WIDTH {}'.format(self.width), file=out_file)
    print('#define GLYPH_HEIGHT {}'.format(self.height), file=out_file)
    print('#define GLYPH_BYTES_PER_ROW {}'
          .format(self.bytes_per_row), file=out_file)
    print('', file=out_file)

    sorted_glyphs = sorted(self.glyph_map.items())

    breaks = []
    last_code_point = None
    for glyph_index in range(len(sorted_glyphs)):
      code_point = sorted_glyphs[glyph_index][0]
      if last_code_point is None or (last_code_point + 1) != code_point:
        breaks.append((code_point, glyph_index))

      last_code_point = code_point

    breaks.append((None, len(sorted_glyphs)))

    print('static int32_t code_point_to_glpyh_index(uint32_t cp)\n{',
          file=out_file)
    for break_idx in range(len(breaks) - 1):
      this_break_code_point, this_break_glyph_index = breaks[break_idx]
      next_break_glyph_index = breaks[break_idx + 1][1]
      this_break_range = next_break_glyph_index - this_break_glyph_index
      if this_break_range == 1:
        print('  if (cp == {}) '.format(this_break_code_point), file=out_file)
        print('    return {};'.format(this_break_glyph_index), file=out_file)
      else:
        print('  if (cp >= {} && cp < {}) '
              .format(this_break_code_point,
                      this_break_code_point + this_break_range),
              file=out_file)
        print('    return cp - {};'
              .format(this_break_code_point - this_break_glyph_index),
              file=out_file)
      print('', file=out_file)
    print('  return -1;', file=out_file)
    print('}', file=out_file)
    print('', file=out_file)

    print('static const uint8_t glyphs[{}][{}] = {{'
          .format(len(self.glyph_map), self.glyph_size), file=out_file)
    for code_point, data in sorted_glyphs:
      print('  { ', end='', file=out_file)
      for data_idx in range(self.glyph_size):
        print('0x{:02x}, '.format(data[data_idx]), end='', file=out_file)
      print('},', file=out_file)
    print('};', file=out_file)


# This BDF parser is very simple. It basically does the minimum work to extract
# all the glyphs in the input file. The only validation done is to check that
# each glyph has exactly the right amount of data. This has only ever been
# tested on the normal Terminus font, sizes 16 and 32.
class BdfState(object):
  """Holds the state and output of the bdf parser.

  This parses the input file on init.
  """
  def __init__(self, in_file):
    # Simple algorithm, try to match each line of input against each regex.
    # The first match that works gets passed to the handler in the tuples
    # below. If there was no match, the line is dropped.
    self.patterns = [
        (re.compile(r'FONTBOUNDINGBOX +(\d+) +(\d+) +([+-]?\d+) +([+-]?\d+)$'),
         self.HandleFONTBOUNDINGBOX),
        (re.compile(r'ENCODING +(\d+)$'), self.HandleENCODING),
        (re.compile(r'BITMAP$'), self.HandleBITMAP),
        (re.compile(r'ENDCHAR$'), self.HandleENDCHAR),
        (re.compile(r'([0-9a-fA-F]{2})+$'), self.HandleDataBITMAP),
    ]
    self.out_glyph_set = None
    self.current_code_point = None
    self.current_glyph_data = None
    self.current_glyph_data_index = None
    for line in in_file:
      self.HandleLine(line)

  def HandleLine(self, line):
    line = line.strip()
    for pattern, handler in self.patterns:
      match = pattern.match(line)
      if match is not None:
        handler(match)
        break

  def HandleFONTBOUNDINGBOX(self, match):
    self.out_glyph_set = GlyphSet(int(match.group(1)), int(match.group(2)))

  def HandleENCODING(self, match):
    self.current_code_point = int(match.group(1))

  def HandleBITMAP(self, _match):
    self.current_glyph_data = [0] * self.out_glyph_set.glyph_size
    self.current_glyph_data_index = 0

  def HandleDataBITMAP(self, match):
    row = match.group(0)
    for c_idx in range(len(row) / 2):
      c = row[c_idx * 2:(c_idx + 1) * 2]
      if self.current_glyph_data_index >= len(self.current_glyph_data):
        raise Exception('too much glyph data, expected {}'
                        .format(len(self.current_glyph_data)))
      self.current_glyph_data[self.current_glyph_data_index] = \
        int(c, base=16)
      self.current_glyph_data_index += 1

  def HandleENDCHAR(self, _match):
    if self.current_glyph_data_index != len(self.current_glyph_data):
      raise Exception('too little glyph data, expected {}, got {}'
                      .format(len(self.current_glyph_data),
                              self.current_glyph_data_index))
    self.out_glyph_set.AddGlyph(self.current_code_point,
                                self.current_glyph_data)
    self.current_code_point = None
    self.current_glyph_data = None
    self.current_glyph_data_index = None

def main(args):
  if len(args) != 3:
    print('Usage: {} [INPUT BDF PATH] [OUTPUT C PATH]'.format(args[0]))
    sys.exit(1)
  gs = BdfState(open(args[1], 'r')).out_glyph_set
  gs.ToCSource(open(args[2], 'w'))

if __name__ == '__main__':
  main(sys.argv)
