#!/usr/bin/env python3

with open("palette.gpl", 'w') as outfile:
    outfile.write("GIMP Palette\n")
    outfile.write("Name: Pebble 3D Engine\n")
    outfile.write("Columns: 8\n")

    for i in range(0, 64):
            outfile.write(f"{((i & 0x30) >> 4) * 85} {((i & 0x0C) >> 2) * 85} {(i & 0x03) * 85} Color{i}\n")
