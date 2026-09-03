#!/usr/bin/env python3

import sys
import pathlib
import array
import itertools
from PIL import Image

outpath = pathlib.Path(sys.argv[1])
outpath = outpath.parent / (outpath.stem + '.bin')

def main():
    with Image.open(sys.argv[1]) as img:
        if img.mode != 'P':
            print("Image must be indexed")
            return
        if (img.width != 32 and img.height != 32) and \
           (img.width != 64 and img.height != 64):
            print("Image must be 32x32 or 64x64")
            return

        buf = array.array('B', itertools.repeat(0, img.width // 4 * 3))
        data = img.get_flattened_data()
        with outpath.open('wb') as outfile:
            for y in range(img.height):
                for x in range(img.width // 4):
                    pixel1 = data[y * img.width + (x * 4)]
                    pixel2 = data[y * img.width + (x * 4) + 1]
                    # ..###### -> ###### ## <- ..##....
                    buf[x * 3] = ((pixel1 & 0x3F) << 2) | ((pixel2 & 0x30) >> 4)
                    pixel1 = data[y * img.width + (x * 4) + 2]
                    # ....#### -> #### #### <- ..####..
                    buf[x * 3 + 1] = ((pixel2 & 0x0F) << 4) | ((pixel1 & 0x3C) >> 2)
                    pixel2 = data[y * img.width + (x * 4) + 3]
                    # ......## -> ## ###### <- ..######
                    buf[x * 3 + 2] = ((pixel1 & 0x03) << 6) | (pixel2 & 0x3F)
                outfile.write(buf)

if __name__ == '__main__':
    main()
