#!/usr/bin/env python3

with open("palette.gpl", 'w') as outfile:
    outfile.write("GIMP Palette\n")
    outfile.write("Name: Pebble 3D Engine\n")
    outfile.write("Columns: 8\n")

    for i in range(0, 255):
        if i & 0x24:
            outfile.write(f"255 0 255 NULL{i}\n")
        else:
            outfile.write(f"{((i & 0xC0) >> 6) * 85} {((i & 0x18) >> 3) * 85} {(i & 0x3) * 85} Color{i}\n")
