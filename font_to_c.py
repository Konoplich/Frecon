#!/usr/bin/python

# Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from __future__ import print_function
import re
import sys

class GlyphSet:
    def __init__(self, width, height):
        self.glyph_map = {}
        self.width = width
        self.height = height
        self.bits_per_pixel = 1
        self.bytes_per_row = (self.bits_per_pixel * self.width + 7) // 8
        self.glyph_size = self.bytes_per_row * self.height

    def add_glyph(self, code_point, data):
        if code_point in self.glyph_map:
            raise Exception('code point {} already added'.format(code_point))

        if len(data) != self.glyph_size:
            raise Exception('given glyph is the wrong size, expected {}, got {}'.format(self.glyph_size, len(data)))

        self.glyph_map[code_point] = data

    def to_c_src(self, out_file):
        print('#define GLYPH_WIDTH {}'.format(self.width), file=out_file)
        print('#define GLYPH_HEIGHT {}'.format(self.height), file=out_file)
        print('#define GLYPH_BYTES_PER_ROW {}'.format(self.bytes_per_row), file=out_file)
        print('', file=out_file)

        sorted_glyphs = sorted(self.glyph_map.items())

        breaks = []
        last_code_point = None
        base_code_point = None
        for glyph_index in range(len(sorted_glyphs)):
            code_point = sorted_glyphs[glyph_index][0]
            if last_code_point is None or (last_code_point + 1) != code_point:
                base_code_point = code_point
                breaks.append((code_point, glyph_index))

            last_code_point = code_point

        breaks.append((None, len(sorted_glyphs)))

        print('static int32_t code_point_to_glpyh_index(uint32_t cp)\n{', file=out_file)
        for break_idx in range(len(breaks) - 1):
            this_break_code_point, this_break_glyph_index = breaks[break_idx]
            next_break_glyph_index = breaks[break_idx + 1][1]
            this_break_range = next_break_glyph_index - this_break_glyph_index
            if this_break_range == 1:
                print('  if (cp == {}) '.format(this_break_code_point), file=out_file)
                print('    return {};'.format(this_break_glyph_index), file=out_file)
            else:
                print('  if (cp >= {} && cp < {}) '.format(this_break_code_point,
                this_break_code_point + this_break_range), file=out_file)
                print('    return cp - {};'.format(
                    this_break_code_point - this_break_glyph_index), file=out_file)
            print('', file=out_file)
        print('  return -1;', file=out_file)
        print('}', file=out_file)
        print('', file=out_file)

        print('static const uint8_t glyphs[{}][{}] = {{'.format(
            len(self.glyph_map), self.glyph_size), file=out_file)
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
class BdfState:
    def __init__(self, in_file):
        # Simple algorithm, try to match each line of input against each regex.
        # The first match that works gets passed to the handler in the tuples
        # below. If there was no match, the line is dropped.
        self.patterns = [
            (re.compile(r'FONTBOUNDINGBOX +(\d+) +(\d+) +([+-]?\d+) +([+-]?\d+)$'), self.handle_FONTBOUNDINGBOX),
            (re.compile(r'ENCODING +(\d+)$'), self.handle_ENCODING),
            (re.compile(r'BITMAP$'), self.handle_BITMAP),
            (re.compile(r'ENDCHAR$'), self.handle_ENDCHAR),
            (re.compile(r'([0-9a-fA-F]{2})+$'), self.handle_BITMAP_data),
        ]
        self.in_file = in_file
        self.out_glyph_set = None
        self.current_code_point = None
        self.current_glyph_data = None
        self.current_glyph_data_index = None


    def handle_line(self, line):
        line = line.strip()
        for pattern, handler in self.patterns:
            match = pattern.match(line)
            if match is not None:
                handler(match)
                break

    def handle_FONTBOUNDINGBOX(self, match):
        self.out_glyph_set = GlyphSet(int(match.group(1)), int(match.group(2)))

    def handle_ENCODING(self, match):
        self.current_code_point = int(match.group(1))

    def handle_BITMAP(self, match):
        self.current_glyph_data = [0] * self.out_glyph_set.glyph_size
        self.current_glyph_data_index = 0

    def handle_BITMAP_data(self, match):
        row = match.group(0)
        for c_idx in range(len(row) / 2):
            c = row[c_idx * 2:(c_idx + 1) * 2]
            if self.current_glyph_data_index >= len(self.current_glyph_data):
                raise Exception('too much glyph data, expected {}'.format(len(self.current_glyph_data)))
            self.current_glyph_data[self.current_glyph_data_index] = \
                int(c, base=16)
            self.current_glyph_data_index += 1

    def handle_ENDCHAR(self, match):
        if self.current_glyph_data_index != len(self.current_glyph_data):
            raise Exception('too little glyph data, expected {}, got {}'.format(len(self.current_glyph_data), self.current_glyph_data_index))
        self.out_glyph_set.add_glyph(self.current_code_point,
                                     self.current_glyph_data)
        self.current_code_point = None
        self.current_glyph_data = None
        self.current_glyph_data_index = None

def bdf_to_glyph_set(in_file):
    state = BdfState(in_file)
    for line in in_file:
        state.handle_line(line)
    return state.out_glyph_set

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print('Usage:\n{} [INPUT BDF PATH] [OUTPUT C PATH]'.format(sys.argv[0]))
        sys.exit(1)
    gs = bdf_to_glyph_set(open(sys.argv[1], 'r'))
    gs.to_c_src(open(sys.argv[2], 'w'))

