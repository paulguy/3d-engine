#!/usr/bin/env python3

import sys
from dataclasses import dataclass
from enum import Enum
import struct
import pathlib
import math

linenum : int = 0
warned : bool = False

def warn(msg : str):
    global warned

    print(f"{linenum}: WARNING: {msg}")
    warned = True

class NotEnoughTokens(Exception):
    pass

def check_tokens(tokens : list[str],
                 token : int,
                 amount : int) -> int:
    if len(tokens) - token < amount:
        raise NotEnoughTokens()

    return token + amount

def get_named(T : Type,
              tokens : list[str],
              token : int,
              namedstr : str,
              get_index : bool = False) -> tuple[Any, int]:
    nexttoken : int = check_tokens(tokens, token, 1)
    name : str = tokens[token]
    index : int
    offsetstr : str
    offset : int = 0
    if '+' in tokens[token]:
        name, offsetstr = tokens[token].split('+', maxsplit=1)
        offset = int(offsetstr)

    if name.isidentifier():
        try:
            index = T.named[name]
        except KeyError:
            raise IndexError(f"{linenum}: {name} isn't a valid identifier.")
    elif get_index:
        # even if this is for getting a named object, it could still be used on its own to just fetch an index from storage
        try:
            index = int(name)
        except ValueError:
            raise ValueError(f"{linenum}: {name} isn't a valid integer index.")
    else:
        return None, token

    # get the index
    index += offset

    try:
        if T.storage is None:
            # return the number itself
            return index, nexttoken
        else:
            if not T.aliases is None:
                try:
                    # get the user-expected index
                    index = T.aliases[index]
                except IndexError:
                    raise IndexError(f"{linenum}: {index - offset} + {offset} is out of range for {namedstr} storage.")

            if get_index:
                try:
                    # return the index
                    return index, nexttoken
                except IndexError:
                    raise IndexError(f"{linenum}: {index - offset} + {offset} is out of range for {namedstr} storage.")
            # return the object
            return T.storage[index], nexttoken
    except KeyError:
        raise KeyError(f"{linenum}: Named {namedstr} {name} does not exist.")

    # wasn't an identifier
    return None, token

def get_multi(T : Type,
              innerT : tuple[Type],
              tokens : list[str],
              token : int,
              fieldstr : str) -> tuple[Any, int]:
    nexttoken : int = check_tokens(tokens, token, len(innerT))

    try:
        # parse inner arguments in to their types, then construct their container type
        if T is tuple:
            return tuple(innerT[n](x) for n, x in enumerate(tokens[token:nexttoken])), nexttoken
        return T(*(innerT[n](x) for n, x in enumerate(tokens[token:nexttoken]))), nexttoken
    except ValueError:
        # AWFUL!
        raise ValueError(''.join((f"{linenum}: {fieldstr} field takes ", *(f"{t.__name__} " for t in innerT), f"argument{'' if len(innerT) == 1 else 's'}.")))

def get_multi_named(T : Type,
                    innerT : tuple[Type],
                    tokens : list[str],
                    token : int,
                    fieldstr : str) -> tuple[Any, int]:
    # there's probably a way to indicate this should return T but lazy
    nexttoken : int

    named, nexttoken = get_named(T, tokens, token, fieldstr)
    if not named is None:
        return named, nexttoken

    return get_multi(T, innerT, tokens, token, fieldstr)

def get_single(T : Type,
               tokens : list[str],
               token : int,
               fieldstr : str) -> tuple[Any, int]:
    nexttoken : int = check_tokens(tokens, token, 1)

    try:
        return T(tokens[token]), nexttoken
    except ValueError:
        raise ValueError(f"{linenum}: {fieldstr} field takes {T.__name__} argument.")

def get_single_named(T : Type,
                     tokens : list[str],
                     token : int,
                     fieldstr : str) -> tuple[Any, int]:
    nexttoken : int

    named, nexttoken = get_named(T, tokens, token, fieldstr)
    if not named is None:
        return named, nexttoken

    return get_single(T, tokens, token, fieldstr)

SUB_MASK = 0x100
PARALLAX_MASK = 0x200
def parse_shade(tokens : list[str], token : int) -> tuple[int, int]:
    nexttoken : int
    r : int
    g : int
    b : int
    sub : int = 0
    parallax : int = 0

    while True:
        nexttoken = check_tokens(tokens, token, 1)

        if tokens[token].isdigit():
            color : str = tokens[token]
            if len(color) != 3:
                raise ValueError(f"{linenum}: Color value must be 3 digits")

            r : int = int(color[0])
            g : int = int(color[1])
            b : int = int(color[2])

            if r < 0 or r > 3 or \
               g < 0 or g > 3 or \
               b < 0 or b > 3:
                # might not do anything sensible, but it's nonfatal
                warn("Any RGB value must be between 0 and 3.")
            break
        elif tokens[token].lower() == 'sub':
            sub = SUB_MASK
        elif tokens[token].lower() == 'parallax':
            parallax = PARALLAX_MASK

        token = nexttoken

    # RR.GG.BB
    return ((r << 6) | (g << 3) | b) | sub | parallax, nexttoken

def shade_str(val : int) -> str:
    return f"{'sub ' if val & SUB_MASK else ''}{'parallax ' if val & PARALLAX_MASK else ''}{(val & 0xC0) >> 6}{(val & 0x18) >> 3}{val & 0x03}"

class Action(Enum):
    NONE = 0x00
    WARP = 0x01
    WAYPOINT = 0x02

def parse_action(tokens : list[str], token : int) -> tuple[int, int]:
    nexttoken : int
    actionstr : str
    sector : int

    try:
        actionstr, token = get_single(str, tokens, token, "action")
    except NotEnoughTokens:
        # action token is optional, so just return none and don't consume tokens rather than error
        return 0, token

    # TODO solid/blocking sector
    match actionstr.lower():
        case "none":
            return Action.NONE.value, token
        case "warp":
            args, token = get_multi(tuple, (int, int), tokens, token, "warp destination")
            # 0x01 <8:map> <8:view>
            return Action.WARP.value | \
                   ((int(args[0]) & 0xFF) << 8) | \
                   ((int(args[1]) & 0xFF) << 16), token
        case "waypoint":
            point, token = get_named(Line, tokens, token, "first point", get_index=True)
            count = 1 # default to 1, to set waypoint only to the single first point
            try:
                count, token = get_single(int, tokens, token, "waypoint")
                count = int(count)
            except NotEnoughTokens:
                # count is optional
                pass
            except ValueError:
                # next should be a keyword, and count is optional
                pass
            # 0x02 <16:first point> <8:count>
            # map count starting at 1
            return Action.WAYPOINT.value | \
                   ((int(point) & 0xFFFF) << 8) | \
                   (((int(count) - 1) & 0xFF) << 24), token

    # this implies an action is the last, optional argument to a sector
    # so if none matched, then it's just assumed to be the start of the next
    # declaration, as none share a name.  This is probably fragile..
    # subtract one to unconsume the token
    return 0, token - 1

def action_str(action : int) -> str:
    match Action(action & 0xFF):
        case Action.NONE:
            return "None"
        case Action.WARP:
            return f"Warp Map {(action & 0xFF00) >> 8} View {(action & 0xFF0000) >> 16}"
        case Action.WAYPOINT:
            return f"Waypoint First Point {(action & 0xFFFF00) >> 8} Count {(action & 0xFF000000) >> 24}"

    # shouldn't happen
    return "Unknown"

@dataclass(frozen=True)
class Point:
    x : float
    y : float

    storage = []
    named = {}
    aliases = []

    STRUCT = struct.Struct("<ff")

    def __str__(self):
        return f"({self.x}, {self.y})"

    def serialize(self):
        return Point.STRUCT.pack(self.x, self.y)

    @staticmethod
    def parse(tokens : list[str], token : int) -> tuple[Point, int]:
        return get_multi_named(Point, (float, float), tokens, token, "Point")

@dataclass(frozen=True)
class Matrix2x2:
    xx : float
    xy : float
    yx : float
    yy : float

    storage = []
    named = {}
    aliases = []

    STRUCT = struct.Struct("<ffff")

    def __str__(self):
        return f"[{self.xx}, {self.xy}] [{self.yx}, {self.yy}]"

    def serialize(self):
        return Matrix2x2.STRUCT.pack(self.xx, self.xy, self.yx, self.yy)

    @staticmethod
    def parse(tokens : list[str], token : int) -> tuple[Matrix2x2, int]:
        return get_multi_named(Matrix2x2, (float, float, float, float), tokens, token, "Matrix2x2")

@dataclass(frozen=True)
class Matrix3x2:
    xx : float
    xy : float
    xz : float
    yx : float
    yy : float
    yz : float

    storage = []
    named = {}
    aliases = []

    STRUCT = struct.Struct("<ffffff")

    def __str__(self):
        return f"[{self.xx}, {self.xy}, {self.xz}] [{self.yx}, {self.yy}, {self.yz}]"

    def serialize(self):
        return Matrix3x2.STRUCT.pack(self.xx, self.xy, self.xz, self.yx, self.yy, self.yz)

    @staticmethod
    def parse(tokens : list[str], token : int) -> tuple[Matrix3x2, int]:
        return get_multi_named(Matrix3x2, (float, float, float, float, float, float), tokens, token, "Matrix3x2")

@dataclass(frozen=True)
class Texture:
    texture : int

    storage = None
    named = {}
    aliases = None

    @staticmethod
    def parse(tokens : list[str], token : int) -> tuple[int, int]:
        texture : Texture

        # it's simpler in callers to just get an int back so just unpack it back out and return it
        texture, token = get_multi_named(Texture, (int,), tokens, token, "Texture")
        return texture.texture, token

@dataclass(frozen=True)
class Line:
    point : int
    textures : tuple[int, int]
    shade : tuple[int, int]
    texture_bias : tuple[int, int]
    texture_transform : tuple[int, int]

    storage = []
    named = {}
    aliases = None

    STRUCT = struct.Struct("<HBBHHHHHH")

    def __str__(self):
        return f"Start Point {self.point}  " \
               f"Top Texture {self.textures[0]} {shade_str(self.shade[0])} {self.texture_bias[0]} {self.texture_transform[0]}  " \
               f"Bottom Texture {self.textures[1]} {shade_str(self.shade[1])} {self.texture_bias[1]} {self.texture_transform[1]}"

    def serialize(self):
        return Line.STRUCT.pack(self.point,
                                self.textures[0], self.textures[1],
                                self.shade[0], self.shade[1],
                                self.texture_bias[0], self.texture_bias[1],
                                self.texture_transform[0], self.texture_transform[1])

    @staticmethod
    def parse(tokens : list[str], token : int) -> tuple[Line, int]:
        point : int
        # TODO: store wall texture info in its own class and write it to its own list in the file and refer to it by int
        texture1 : int
        texture2 : int
        shade1 : tuple[int]
        shade2 : tuple[int]
        texture_bias1 : int
        texture_bias2 : int
        texture_transform1 : int
        texture_transform2 : int

        point, token = get_named(Point, tokens, token, "point", get_index=True)

        texture1, token = get_named(Texture, tokens, token, "texture", get_index=True)
        shade1, token = parse_shade(tokens, token)
        texture_bias1, token = get_named(Point, tokens, token, "texture bias", get_index=True)
        texture_transform1, token = get_named(Matrix3x2, tokens, token, "texture transform", get_index=True)

        texture2, token = get_named(Texture, tokens, token, "texture", get_index=True)
        shade2, token = parse_shade(tokens, token)
        texture_bias2, token = get_named(Point, tokens, token, "texture bias", get_index=True)
        texture_transform2, token = get_named(Matrix3x2, tokens, token, "texture transform", get_index=True)

        return Line(point,
                    (texture1, texture2),
                    (shade1, shade2),
                    (texture_bias1, texture_bias2),
                    (texture_transform1, texture_transform2)), token

@dataclass(frozen=True)
class Sector:
    height : tuple[float, float]
    textures : tuple[int, int]
    shade : tuple[int, int]
    texture_bias : tuple[int, int]
    texture_transform : tuple[int, int]
    firstline : int
    lines : int
    action : int

    storage = []
    named = {}
    aliases = None

    STRUCT = struct.Struct("<ffBBHHHHHHHBI")

    def serialize(self):
        return Sector.STRUCT.pack(self.height[0], self.height[1],
                                  self.textures[0], self.textures[1],
                                  self.shade[0], self.shade[1],
                                  self.texture_bias[0], self.texture_bias[1],
                                  self.texture_transform[0], self.texture_transform[1],
                                  self.firstline, self.lines - 3,
                                  self.action)

    def __str__(self):
        return f"Ceiling Height {self.height[0]}  Ceiling Texture {self.textures[0]} {shade_str(self.shade[0])} {self.texture_bias[0]} {self.texture_transform[0]}  " \
               f"Floor Height {self.height[1]}  Floor Texture {self.textures[1]} {shade_str(self.shade[1])} {self.texture_bias[1]} {self.texture_transform[1]}  " \
               f"First Line {self.firstline}  Lines {self.lines}  " \
               f"Action {action_str(self.action)}"

    @staticmethod
    def parse(tokens : list[str], token : int) -> tuple[Sector, int]:
        nexttoken : int
        height : tuple[float, float]
        # TODO probably store these too
        texture1 : int
        texture2 : int
        shade1 : tuple(int)
        shade2 : tuple(int)
        texture_bias1 : int
        texture_bias2 : int
        texture_transform1 : int
        texture_transform2 : int
        linecount : int
        action : int

        line : Line
        sector : Sector

        height, token = get_multi(tuple, (float, float), tokens, token, "Height")

        texture1, token = get_named(Texture, tokens, token, "texture", get_index=True)
        shade1, token = parse_shade(tokens, token)
        texture_bias1, token = get_named(Point, tokens, token, "texture bias", get_index=True)
        texture_transform1, token = get_named(Matrix2x2, tokens, token, "texture transform", get_index=True)

        texture2, token = get_named(Texture, tokens, token, "texture", get_index=True)
        shade2, token = parse_shade(tokens, token)
        texture_bias2, token = get_named(Point, tokens, token, "texture bias", get_index=True)
        texture_transform2, token = get_named(Matrix2x2, tokens, token, "texture transform", get_index=True)

        line, token = get_named(Line, tokens, token, "first line", get_index=True)
        linecount, token = get_single(int, tokens, token, "line count")
        if linecount < 3:
            raise ValueError(f"{linenum}: A sector needs at least 3 lines, or up to 259.")
        if line + linecount > len(Line.storage):
            raise ValueError(f"{linenum}: Lines {line} - {line + linecount} goes past end of lines list ({len(Line.storage)}).")
        # duplicate points on a sector wall breaks rendering?
        # might catch some mistakes too.
        for i in range(linecount):
            for j in range(linecount - i - 1):
                # the points themselves shouldn't need to be compared, but at least 1 needs to be fetched anyway
                # to print the point coordinates
                point1 = Point.storage[Point.aliases[Line.storage[line+i].point]]
                point2 = Point.storage[Point.aliases[Line.storage[line+i+1+j].point]]
                if point1 == point2:
                    warn(f"Lines {i} and {j} share a point {point1.x}, {point1.y}.")

        action, token = parse_action(tokens, token)

        return Sector(height,
                      (texture1, texture2),
                      (shade1, shade2),
                      (texture_bias1, texture_bias2),
                      (texture_transform1, texture_transform2),
                      line, linecount, action), token

@dataclass(frozen=True)
class Link:
    sector : int
    line : int
    lsector : int
    lline : int

    storage = []
    named = None
    aliases = None

    STRUCT = struct.Struct("<HHHH")

    def serialize(self):
        return Link.STRUCT.pack(self.sector, self.line, self.lsector, self.lline)

    def __str__(self):
        return f"First Sector:Line {self.sector}:{self.line}  Second Sector:Line {self.lsector}:{self.lline}"

    @staticmethod
    def parse(tokens : list[str], token : int) -> tuple[Line, int]:
        sectorindex : int
        sector : int
        line : int
        lsectorindex : int
        lsector : int
        lline : int
        found : bool
        lindex : int
        llindex : int
        line1points : tuple[int, int]
        line2points : tuple[int, int]

        sectorindex, token = get_named(Sector, tokens, token, "sector", get_index=True)
        sector = Sector.storage[sectorindex]
        line, token = get_named(Line, tokens, token, "line", get_index=True)
        lsectorindex, token = get_named(Sector, tokens, token, "link sector", get_index=True)
        lsector = Sector.storage[lsectorindex]
        lline, token = get_named(Line, tokens, token, "link line", get_index=True)

        #find lines in each sector
        found = False
        for n, l in enumerate(range(sector.firstline, sector.firstline + sector.lines)):
            if line == l:
                lindex = n
                found = True
                break
        if not found:
            raise ValueError(f"{linenum}: Line {line} is not in sector {sectorindex}.")

        found = False
        for n, l in enumerate(range(lsector.firstline, lsector.firstline + lsector.lines)):
            if lline == l:
                llindex = n
                found = True
                break
        if not found:
            raise ValueError(f"{linenum}: Line {lline} is not in sector {lsectorindex}.")

        # make sure lines share points
        line1points = (Line.storage[sector.firstline + lindex].point,
                       Line.storage[sector.firstline + ((lindex + 1) % sector.lines)].point)
        line2points = (Line.storage[lsector.firstline + llindex].point,
                       Line.storage[lsector.firstline + ((llindex + 1) % lsector.lines)].point)

        # sector lines are clockwise so linked walls will share opposite points
        if line1points[0] != line2points[1] or \
           line2points[1] != line1points[0]:
            # probably will horribly break rendering but it can technically be stored
            warn(f"Linked walls must share points (no \"portal\" rendering). Have: {line1points[0]}-{line1points[1]}, {line2points[0]}-{line2points[1]}")

        return Link(sectorindex, lindex, lsectorindex, llindex), token

@dataclass(frozen=True)
class View:
    start : int
    point : int
    angle : float
    height : float
    fov : float

    storage = []
    named = None
    aliases = None

    STRUCT = struct.Struct("<HHfff")

    def __str__(self):
        return f"Sector {self.start}  Point {self.point}  Angle {self.angle}  Height {self.height}  Field of View {self.fov}"

    def serialize(self):
        return View.STRUCT.pack(self.start, self.point,
                                self.angle / 360.0 * math.tau, self.height,
                                self.fov / 360.0 * math.tau)

    @staticmethod
    def parse(tokens : list[str], token : int) -> tuple[Line, int]:
        start : int
        point : int
        angle : float
        height : float
        fov : float

        start, token = get_named(Sector, tokens, token, "start sector", get_index=True)
        point, token = get_named(Point, tokens, token, "start point", get_index=True)
        angle, token = get_single(float, tokens, token, "angle")
        height, token = get_single(float, tokens, token, "height")
        fov, token = get_single(float, tokens, token, "field of view")

        return View(start, point, angle, height, fov), token

types : dict[str, Type] = {
    'point': Point,
    'matrix2x2': Matrix2x2,
    'matrix3x2': Matrix3x2,
    'texture': Texture,
    'line': Line,
    'sector': Sector,
    'link': Link,
    'view': View
}

def main():
    global linenum

    HDR_STRUCT = struct.Struct("<HHHHHHH")

    outpath = pathlib.Path(sys.argv[1])
    outpath = outpath.parent / (outpath.stem + '.bin')

    with open(sys.argv[1], 'r') as infile:
        tokens : list[str] = []
        typename : str = ""
        objtype : Type
        name : str = ""
        last_was_type : bool = False
        last_was_as : bool = False
        token : int = 0
        index : int

        for line in infile:
            linenum += 1

            # remove comments
            line = line.split('#', maxsplit=1)[0]
            # remove whitespace
            line = line.strip()
            # skip blank lines
            if len(line) == 0:
                continue

            # gather tokens
            tokens.extend(line.split())
            #print(tokens)

            # if the object type isn't known yet, store it
            if len(tokens) > 0:
                if typename == "":
                    typename = tokens[0].lower()
                    if not typename in types:
                        raise ValueError(f"{linenum}: Type {typename} isn't a valid type.")
                    tokens = tokens[1:]
                    last_was_type = True
            else:
                continue

            # if any tokens and the last token was a type and this token is 'as', use the next to name this
            if len(tokens) > 0:
                if last_was_type:
                    if tokens[0].lower() == "as":
                        tokens = tokens[1:]
                        last_was_as = True
                    last_was_type = False
            else:
                continue

            if len(tokens) > 0:
                if last_was_as:
                    if tokens[0].isidentifier():
                        name = tokens[0]
                        tokens = tokens[1:]
                    else:
                        raise ValueError(f"{linenum}: Invalid name string {tokens[0]}, should be a python identifier (at least no leading digits/various symbols).")
                    last_was_as = False
            else:
                continue

            # typename was checked for validity earlier
            objtype = types[typename]
            try:
                obj, token = objtype.parse(tokens, token)
            except NotEnoughTokens:
                # try to read more data
                continue

            # try to store it
            if not objtype.storage is None:
                if not objtype.aliases is None:
                    # if there's an alias list, try to find its index for deduplication
                    try:
                        index = objtype.storage.index(obj)
                    except ValueError:
                        # otherwise, store it
                        objtype.storage.append(obj)
                        index = len(objtype.storage) - 1
                    objtype.aliases.append(index)
                else:
                    objtype.storage.append(obj)
                    index = len(objtype.storage) - 1

            # try to store it by name
            if name != "":
                if not objtype.named is None:
                    if name in objtype.named:
                        raise ValueError(f"{linenum}: Name {name} is already declared")

                    if objtype.storage is None:
                        # if doesn't have storage, store the object
                        objtype.named[name] = obj
                    else:
                        # if it does have storage, store the index
                        if not objtype.aliases is None:
                            # if it has deduplication, store the alias
                            # this is how it's fetched later so entered in indexes are transparent to the user
                            objtype.named[name] = objtype.aliases.index(index)
                        else:
                            objtype.named[name] = index
                else:
                    raise ValueError(f"{linenum}: Type {typename} can't be named.")

            # clip what's consumed
            tokens = tokens[token:]
            token = 0
            typename = ""
            name = ""

    with outpath.open('wb') as outfile:
        print(f"Points {len(Point.storage)}  " \
              f"Matrix2x2s {len(Matrix2x2.storage)}  " \
              f"Matrix3x2s {len(Matrix3x2.storage)}  " \
              f"Lines {len(Line.storage)}  " \
              f"Sectors {len(Sector.storage)}  " \
              f"Links {len(Link.storage)}  " \
              f"Views {len(View.storage)}")
        outfile.write(HDR_STRUCT.pack(len(Point.storage),
                                      len(Matrix2x2.storage),
                                      len(Matrix3x2.storage),
                                      len(Line.storage),
                                      len(Sector.storage),
                                      len(Link.storage),
                                      len(View.storage)))

        for n, point in enumerate(Point.storage):
            print(n, point)
            outfile.write(point.serialize())

        for n, matrix2x2 in enumerate(Matrix2x2.storage):
            print(n, matrix2x2)
            outfile.write(matrix2x2.serialize())

        for n, matrix3x2 in enumerate(Matrix3x2.storage):
            print(n, matrix3x2)
            outfile.write(matrix3x2.serialize())

        for n, line in enumerate(Line.storage):
            print(n, line)
            outfile.write(line.serialize())

        for n, sector in enumerate(Sector.storage):
            print(n, sector)
            outfile.write(sector.serialize())

        for n, link in enumerate(Link.storage):
            print(n, link)
            outfile.write(link.serialize())

        for n, view in enumerate(View.storage):
            print(n, view)
            outfile.write(view.serialize())

    if warned:
        print("There were warnings.")

if __name__ == '__main__':
    main()
