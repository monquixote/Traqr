import sys


def pack_l2_row(row_pixels):
    # row_pixels: list of values 0..3
    # 4 pixels per byte: (p0 << 6) | (p1 << 4) | (p2 << 2) | p3
    assert len(row_pixels) % 4 == 0
    bytes_out = []
    for i in range(0, len(row_pixels), 4):
        b = (row_pixels[i] << 6) | (row_pixels[i + 1] << 4) | (row_pixels[i + 2] << 2) | row_pixels[i + 3]
        bytes_out.append(b)
    return bytes_out


def format_c_array(name, byte_list, bytes_per_line=16):
    lines = []
    lines.append(f"const uint8_t {name}[{len(byte_list)}] = {{")
    for i in range(0, len(byte_list), bytes_per_line):
        chunk = byte_list[i:i + bytes_per_line]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"    {hex_str},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def generate_mini():
    w, h = 80, 28
    grid = [[0 for _ in range(w)] for _ in range(h)]

    cy = 13.5
    for y in range(h):
        for x in range(w):
            if x < 24:
                dist = ((x - 24) ** 2 + (y - cy) ** 2) ** 0.5
            elif x > 66:
                dist = ((x - 66) ** 2 + (y - cy) ** 2) ** 0.5
            else:
                dist = abs(y - cy)

            diff = 13.5 - dist
            if diff >= 0.6:
                grid[y][x] = 3
            elif diff >= 0.0:
                grid[y][x] = 2
            elif diff >= -0.6:
                grid[y][x] = 1
            else:
                grid[y][x] = 0

    # Letter definitions 7x14
    letters = {
        'J': [
            "  #####",
            "  #####",
            "    ###",
            "    ###",
            "    ###",
            "    ###",
            "    ###",
            "    ###",
            "    ###",
            "##  ###",
            "#######",
            "###### ",
            " ####  ",
            "  ##   ",
        ],
        'U': [
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "#######",
            "#######",
            " ##### ",
            "  ###  ",
        ],
        'L': [
            "###    ",
            "###    ",
            "###    ",
            "###    ",
            "###    ",
            "###    ",
            "###    ",
            "###    ",
            "###    ",
            "###    ",
            "###    ",
            "#######",
            "#######",
            "#######",
        ],
        'Y': [
            "##   ##",
            "##   ##",
            "### ###",
            "### ###",
            " ##### ",
            " ##### ",
            "  ###  ",
            "  ###  ",
            "  ###  ",
            "  ###  ",
            "  ###  ",
            "  ###  ",
            "  ###  ",
            "  ###  ",
        ],
        'O': [
            " ##### ",
            "#######",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "##   ##",
            "#######",
            " ##### ",
        ],
    }

    word = ['J', 'U', 'L', 'Y', 'O']
    sx = 25
    sy = 7
    for ch in word:
        pat = letters[ch]
        for py, row in enumerate(pat):
            for px, c in enumerate(row):
                if c == '#':
                    grid[sy + py][sx + px] = 0
        sx += 8

    all_bytes = []
    for y in range(h):
        all_bytes.extend(pack_l2_row(grid[y]))

    assert len(all_bytes) == 560
    return grid, format_c_array("juno_mini_nomark_80x28_L2", all_bytes)


def generate_med():
    w, h = 108, 40
    grid = [[0 for _ in range(w)] for _ in range(h)]

    cy = 19.5
    for y in range(h):
        for x in range(w):
            if x < 30:
                dist = ((x - 30) ** 2 + (y - cy) ** 2) ** 0.5
            elif x > 90:
                dist = ((x - 90) ** 2 + (y - cy) ** 2) ** 0.5
            else:
                dist = abs(y - cy)

            diff = 19.5 - dist
            if diff >= 0.7:
                grid[y][x] = 3
            elif diff >= 0.0:
                grid[y][x] = 2
            elif diff >= -0.7:
                grid[y][x] = 1
            else:
                grid[y][x] = 0

    # Letter definitions 9x18
    letters = {
        'J': [
            "  #######",
            "  #######",
            "     ####",
            "     ####",
            "     ####",
            "     ####",
            "     ####",
            "     ####",
            "     ####",
            "     ####",
            "     ####",
            "     ####",
            "###  ####",
            "#########",
            "#########",
            " ####### ",
            "  #####  ",
            "   ###   ",
        ],
        'U': [
            "#### ####",
            "#### ####",
            "#### ####",
            "#### ####",
            "#### ####",
            "#### ####",
            "#### ####",
            "#### ####",
            "#### ####",
            "#### ####",
            "#### ####",
            "#### ####",
            "#########",
            "#########",
            "#########",
            " ####### ",
            "  #####  ",
            "   ###   ",
        ],
        'L': [
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "####     ",
            "#########",
            "#########",
            "#########",
        ],
        'Y': [
            "###   ###",
            "###   ###",
            "#### ####",
            "#### ####",
            " ####### ",
            " ####### ",
            "  #####  ",
            "  #####  ",
            "   ###   ",
            "   ###   ",
            "   ###   ",
            "   ###   ",
            "   ###   ",
            "   ###   ",
            "   ###   ",
            "   ###   ",
            "   ###   ",
            "   ###   ",
        ],
        'O': [
            "  #####  ",
            " ####### ",
            "#########",
            "###   ###",
            "###   ###",
            "###   ###",
            "###   ###",
            "###   ###",
            "###   ###",
            "###   ###",
            "###   ###",
            "###   ###",
            "###   ###",
            "###   ###",
            "#########",
            "#########",
            " ####### ",
            "  #####  ",
        ],
    }

    word = ['J', 'U', 'L', 'Y', 'O']
    sx = 33
    sy = 11
    for ch in word:
        pat = letters[ch]
        for py, row in enumerate(pat):
            for px, c in enumerate(row):
                if c == '#':
                    grid[sy + py][sx + px] = 0
        sx += 11

    all_bytes = []
    for y in range(h):
        all_bytes.extend(pack_l2_row(grid[y]))

    assert len(all_bytes) == 1080
    return grid, format_c_array("juno_med_108x40_L2", all_bytes)


g_mini, mini_code = generate_mini()
g_med, med_code = generate_med()

with open("core/libs/images/juno_mini_nomark_80x28_L2.h", "w") as f:
    f.write("// Generated L2 bitmap (80x28) for JULYO synth\n")
    f.write(mini_code)

with open("core/libs/images/juno_med_108x40_L2.h", "w") as f:
    f.write("// Generated L2 bitmap (108x40) for JULYO synth\n")
    f.write(med_code)

print("Mini preview:")
for row in g_mini:
    print("".join(" .+#"[v] for v in row))
print("Bitmaps written successfully.")
