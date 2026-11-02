"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/shaders.js` holds the GLSL pieces of `source/Shaders.cpp` (kQuadVertex,
the kCommon library, and the table and foil fragments) plus kVersion. That is
two copies of the same text, and two copies drift -- quietly, because a foil
drawn by a *plausible* shader looks exactly like one drawn by the right one.
The whole claim of the page is that it runs the plugin's own shaders, so the
claim needs something enforcing it. `hftest` drives the real plugin class and
has never heard of this page, and `tools/glslc.sh` compiles the C++ copies and
never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each piece out of `source/Shaders.cpp` and the matching backtick literal
out of `demo/shaders.js`, and compares them exactly -- no whitespace
normalisation, no comment stripping: a comment updated on one side only is
exactly the drift worth catching. `kVersion`, a plain string, is compared too.

A piece may be several adjacent raw strings (MSVC refuses one literal over
about 16 KB, C2026); this joins them as the compiler does, with nothing
between. None of holofoil's pieces needs that today.

It also fails when Shaders.cpp grows a piece this script does not list (a new
pass the page would silently lack), and it checks the JOIN as well as the
pieces: `Assemble()` in Shaders.cpp is kVersion + kCommon + the pass's
fragment (the table's or the foil's); InitGL in Holofoil.cpp builds the vertex
stage as kVersion + kQuadVertex and each fragment stage with Assemble().
plugin.js must assemble them the same way. A change to either side's assembly
fails here and needs a reader.

The one transformation is a decode, not a normalisation: a backtick cannot
appear raw inside a template literal, so --write escapes one as \\`. This undoes that escape and REJECTS any other backslash
or any `${` on the JS side; there are none in the C++ bodies, so either could
only be somebody hiding a difference. kFoilFragment has four backticks, in
the comment above lineColour().

`--write` produces shaders.js from the C++ by exactly that escape, so the copy
is spliced by this script and never typed.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half: Controls.cpp (the mappings), Card.cpp (the
frame, the lamp's covariance, the motion and the colour matrix) and the frame
logic of Holofoil.cpp's ProcessOpenGL and ensureTable in demo/plugin.js are a
hand translation. demo/tools/crosscheck/ renders hftest and the page over the
same frames and compares the pictures; it needs Chrome and a GPU, so it is run
by hand, not by verify.sh. When you change one of those, change the port too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# JS constant, C++ symbol (all in source/Shaders.cpp), in the order they are
# declared there.
SHADERS = [
    ("QUAD_VERTEX", "kQuadVertex"),
    ("COMMON", "kCommon"),
    ("TABLE_FRAGMENT", "kTableFragment"),
    ("FOIL_FRAGMENT", "kFoilFragment"),
]

# How the pieces are joined, on each side. Each entry is (file, text that must
# appear in it verbatim). The C++ lines are the plugin's assembly; the JS lines
# are plugin.js's copy of it. A change to either is a change to what runs.
ASSEMBLY = [
    ("source/Shaders.cpp",
     "std::string Assemble( Pass pass )\n{\n"
     "\tstd::string out = kVersion;\n"
     "\tout += kCommon;\n"
     "\tout += pass == Pass::Table ? kTableFragment : kFoilFragment;\n"
     "\treturn out;\n}"),
    ("source/Holofoil.cpp", "const std::string vertex = std::string( shaders::kVersion ) + shaders::kQuadVertex;"),
    ("source/Holofoil.cpp", '{ &tableShader, shaders::Pass::Table, "table" },'),
    ("source/Holofoil.cpp", '{ &foilShader, shaders::Pass::Foil, "foil" },'),
    ("source/Holofoil.cpp", "const std::string fragment = shaders::Assemble( stage.pass );"),
    ("source/Holofoil.cpp", "if( stage.shader->Compile( vertex.c_str(), fragment.c_str() ) )"),
    ("demo/plugin.js", "const vertex = S.VERSION + S.QUAD_VERTEX;"),
    ("demo/plugin.js", "const assemble = (fragment) => S.VERSION + S.COMMON + fragment;"),
    ("demo/plugin.js", "table: new Program(gl, vertex, assemble(S.TABLE_FRAGMENT), 'table'),"),
    ("demo/plugin.js", "foil: new Program(gl, vertex, assemble(S.FOIL_FRAGMENT), 'foil'),"),
]

HEADER = """// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \\` for a backtick inside a comment.
"""


def read(*parts):
    with open(os.path.join(REPO, *parts)) as handle:
        return handle.read()


def cpp_version(source):
    match = re.search(r'const char\* const kVersion = "(.*?)";', source)
    return None if match is None else match.group(1)


def from_cpp(source, symbol):
    """The piece's text: its raw strings, joined as the compiler joins them.

    Returns (text, count of raw strings), or (None, 0) if the symbol is not
    there or what follows its `=` is not a run of adjacent raw strings ending
    in `;`.
    """
    match = re.search(r'const char\* const ' + symbol + r' = ((?:R"\(.*?\)"\s*)+);', source, re.S)
    if match is None:
        return None, 0
    parts = re.findall(r'R"\((.*?)\)"', match.group(1), re.S)
    rebuilt = re.sub(r'R"\(.*?\)"', "", match.group(1), flags=re.S)
    if rebuilt.strip() != "":
        return None, 0
    return "".join(parts), len(parts)


def cpp_pieces(source):
    """Every raw-string piece Shaders.cpp defines, by symbol."""
    return re.findall(r'const char\* const (k\w+) = R"\(', source)


def from_js(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def write(cpp):
    out = [HEADER]
    version = cpp_version(cpp)
    if version is None:
        print("FAIL  kVersion not found in source/Shaders.cpp")
        return 1
    # kVersion is "#version 410 core\n" in the C++, the \n an escape there.
    out.append(f"export const VERSION = '{version}';\n")
    for name, symbol in SHADERS:
        body, count = from_cpp(cpp, symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp (or not a run of adjacent raw strings)")
            return 1
        if "\\" in body or "${" in body:
            print(f"FAIL  {symbol} holds a backslash or ${{; the escape scheme cannot carry it")
            return 1
        joined = f", {count} adjacent raw strings joined" if count > 1 else ""
        out.append(f"\n// {symbol}, source/Shaders.cpp{joined}\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
        handle.write("".join(out))
    print("wrote demo/shaders.js")
    return 0


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check(cpp, js):
    problems = 0
    version_cpp = cpp_version(cpp)
    version_js = re.search(r"^export const VERSION = '(.*?)';$", js, re.M)
    if version_cpp is None or version_js is None or version_cpp != version_js.group(1):
        print("FAIL  VERSION does not match kVersion")
        problems += 1
    else:
        print(f"ok    {'VERSION':<20} matches kVersion")

    listed = {symbol for _, symbol in SHADERS}
    for symbol in cpp_pieces(cpp):
        if symbol not in listed:
            print(f"FAIL  source/Shaders.cpp defines {symbol}, which this check does not list -- "
                  "add it to SHADERS (and to the page, or say on the page why not)")
            problems += 1

    for name, symbol in SHADERS:
        cpp_text, count = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp (or not a run of adjacent raw strings)")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            joined = f", {count} raw strings joined" if count > 1 else ""
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars{joined})")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    for path, text in ASSEMBLY:
        try:
            body = read(*path.split("/"))
        except FileNotFoundError:
            print(f"FAIL  {path} is missing")
            problems += 1
            continue
        if text in body:
            print(f"ok    assembly: {path} still says {text.splitlines()[0][:60]}")
        else:
            print(f"FAIL  assembly: {path} no longer says: {text.splitlines()[0]}")
            problems += 1
    return problems


def main(argv):
    cpp = read("source", "Shaders.cpp")
    if "--write" in argv:
        return write(cpp)
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check(cpp, js)
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for a drifted copy "
              "(never edit shaders.js by hand); an assembly or unlisted-piece failure needs a reader")
        return 1
    print(f"all {len(SHADERS) + 1} shader pieces and their assembly are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
