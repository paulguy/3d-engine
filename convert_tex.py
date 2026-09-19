#!/usr/bin/env python3

import sys
import pathlib
import array
import itertools
from PIL import Image

# image parameters
# edge dimension (32, 64) (1b)
# storage direction (horizontal/vertical) (1b)
# palette size (2, 4, 8, 16, 32, 64) (3b)
# RLE repeat word size (uncompressed, 2, 3, 4, 5, 6, 7, 8) (3b)
# 64 color palette = direct color

def makepalette(data : bytes) -> array.array:
    pal = array.array('B')
    for pixel in data:
        if not pixel in pal:
            pal.append(pixel)

    return pal

def smallest_power_of_2(val : int) -> int:
    power = 1
    bits = 0
    while power < val:
        power *= 2
        bits += 1
    return power, bits

def bitpack(buf : array.array, bit : int,
            val : int, size : int) -> int:
    #print(bit, val, size)
    cur : int = 0
    if bit == 0:
        buf.append(0)

    if bit + size < 8:
        # shift val in place and return remaining bits
        buf[-1] = buf[-1] | (val << (8 - bit - size))
        return bit + size
    else:
        # shift the front of val in to the rest of the last byte
        buf[-1] = buf[-1] | (val >> (size - (8 - bit)))
        cur += 8 - bit

    while size - cur > 8:
        # shift whole bytes out of val
        buf.append((val >> (size - cur - 8)) & 0xFF)
        cur += 8

    if size - cur > 0:
        # shift last bits in to the first bits of the next byte
        buf.append(((val & (0xFF >> (8 - (size - cur)))) << (8 - (size - cur))))
        return size - cur

    return 0

def encode(vertical : bool,
           repeat_bits : int,
           img : Image) -> array.array:
    if repeat_bits > 8:
        raise ValueError("Repeat bits can only be 0 for uncompressed or 2-8")
    
    if (img.width == 32 and img.height == 32):
        dim_bit = 0
    elif (img.width == 64 and img.height == 64):
        dim_bit = 1
    else:
        raise ValueError("Image must be 32x32 or 64x64")

    if img.mode != 'P':
        raise ValueError("Image must be indexed")

    data = img.get_flattened_data()
    pal = makepalette(data)
    palette_size, palette_bits = smallest_power_of_2(len(pal))

    bit = 0
    buf = array.array('B')

    # write header
    bit = bitpack(buf, bit, dim_bit, 1)
    bit = bitpack(buf, bit, vertical, 1)
    bit = bitpack(buf, bit, palette_bits, 3)
    # store repeat bits value starting with 2 mapping to 1, and just store 0 for uncompressed
    bit = bitpack(buf, bit, repeat_bits - 1 if repeat_bits > 0 else 0, 3)

    if palette_bits < 6:
        # record the precise number of palette entries
        # store palette size from 0-31 to fit in to 5 bits
        bit = bitpack(buf, bit, len(pal) - 1, 5);
        # if not direct color, write palette
        for color in pal:
            # color should already be 6 bits but yknow
            bit = bitpack(buf, bit, color & 0x3F, 6)

    if repeat_bits == 0:
        print(f"Palette bits: {palette_bits}  Palette count: {palette_size}")
        # uncompressed
        for pixel in data:
            if palette_bits == 6:
                # direct color
                bit = bitpack(buf, bit, pixel, palette_bits)
            else:
                bit = bitpack(buf, bit, pal.index(pixel), palette_bits)
    else:
        # compressed

        # minimum repeat is 1 so the bit size should be able to encode starting at 1
        max_repeat = (1 << repeat_bits)
        hist = [0 for x in range(max_repeat)]

        lastcolor = data[0]
        repeat = 0 # 0 will be iterated over again then be repeats will be 1
        if vertical:
            for x in range(img.width):
                for y in range(img.height):
                    color = data[y * img.height + x]
                    if repeat == max_repeat or color != lastcolor:
                        # map repeat count starting at 1
                        bit = bitpack(buf, bit, repeat - 1, repeat_bits)
                        if palette_size == 64:
                            # direct color
                            bit = bitpack(buf, bit, lastcolor, palette_bits)
                        else:
                            bit = bitpack(buf, bit, pal.index(lastcolor), palette_bits)
                        hist[repeat-1] += 1
                        repeat = 0
                        lastcolor = color
                    repeat += 1
        else:
            for y in range(img.height):
                for x in range(img.width):
                    color = data[y * img.height + x]
                    if repeat == max_repeat or color != lastcolor:
                        bit = bitpack(buf, bit, repeat - 1, repeat_bits)
                        if palette_size == 64:
                            bit = bitpack(buf, bit, lastcolor, palette_bits)
                        else:
                            bit = bitpack(buf, bit, pal.index(lastcolor), palette_bits)
                        hist[repeat-1] += 1
                        repeat = 0
                        lastcolor = color
                    repeat += 1
#        for n, count in enumerate(hist):
#            print(f"{n+1}: {count} {(palette_bits+repeat_bits)*count}b")

    return buf

def main():
    outpath = pathlib.Path(sys.argv[1])
    outpath = outpath.parent / (outpath.stem + '.bin')

    img = Image.open(sys.argv[1])

    encoded = encode(False, 0, img)
    smallest = encoded
    print(f"Uncompressed  size: {len(encoded)}")
    for vertical in (False, True):
        for repeat_bits in (2, 3, 4, 5, 6, 7, 8):
            encoded = encode(vertical, repeat_bits, img)
            print(f"vertical: {vertical}  repeat bits: {repeat_bits}  size: {len(encoded)}")
            if len(encoded) < len(smallest):
                smallest = encoded

    with outpath.open('wb') as outfile:
        outfile.write(smallest)

if __name__ == '__main__':
    main()
