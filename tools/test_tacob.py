#!/usr/bin/env python3
"""Offline tests for tacob — no game files, no network.

The compiler's output is executed by nobody here; what these tests pin is the
*shape* of the bytecode Scriptor emits (measured over the stock corpus, see the
module docstring of tools/tacob) and the file layout, because a COB that is one
word off loads fine and misbehaves days later. The stock-corpus gate itself is
`tools/tacob roundtrip --all`, which needs the game's archives.

    python3 tools/test_tacob.py [-v]
"""

import struct
import unittest
from importlib.machinery import SourceFileLoader
from pathlib import Path

tacob = SourceFileLoader("tacob", str(Path(__file__).with_name("tacob"))).load_module()
OP = tacob.OP


def compile_words(text, **kw):
    return tacob.compile_bos(text, "test.bos", **kw).code


def words(*items):
    """Mnemonics and integers into words: words("PUSH_CONSTANT", 5, "RETURN")."""
    out = []
    for item in items:
        out.append(OP[item] if isinstance(item, str) else item & 0xFFFFFFFF)
    return out


HEADERS = {
    "exptype.h": (tacob.INCLUDE_DIR / "exptype.h").read_text(),
    "sfxtype.h": (tacob.INCLUDE_DIR / "sfxtype.h").read_text(),
}
PRELUDE = "piece base, turret, barrel, flare;\nstatic-var s1, s2;\n"


def script(name, body, params=""):
    return f"{PRELUDE}{name}({params})\n{{\n{body}\n}}\n"


class Literals(unittest.TestCase):
    def test_truncation_toward_zero(self):
        # <20> = 3640.9 -> 3640, <105> = 19114.7 -> 19114, <-100> -> -18204 (stock values)
        self.assertEqual(tacob.angular_word(20), 3640)
        self.assertEqual(tacob.angular_word(105), 19114)
        self.assertEqual(tacob.angular_word(-100), -18204)
        self.assertEqual(tacob.linear_word(1.375), 90112)
        self.assertEqual(tacob.linear_word(-1.374985), -90111)

    def test_format_reproduces_every_word(self):
        for w in (0, 1, -1, 3640, 19114, -18204, 16384, 27306, 54613, -90111, 90112, 2457600, 7):
            self.assertEqual(tacob.angular_word(float(tacob.format_angular(w)[1:-1])), w, w)
            self.assertEqual(tacob.linear_word(float(tacob.format_linear(w)[1:-1])), w, w)
        self.assertEqual(tacob.format_angular(16384), "<90>")
        self.assertEqual(tacob.format_linear(90112), "[1.375]")

    def test_negative_literal_folds_but_negated_name_subtracts(self):
        self.assertEqual(compile_words(script("F", "sleep -5;")),
                         words("PUSH_CONSTANT", -5, "SLEEP", "PUSH_CONSTANT", 0, "RETURN"))
        self.assertEqual(compile_words(script("F", "sleep -s1;")),
                         words("PUSH_CONSTANT", 0, "PUSH_STATIC_VAR", 0, "SUB", "SLEEP",
                               "PUSH_CONSTANT", 0, "RETURN"))


class Lexing(unittest.TestCase):
    def test_hyphenated_keywords_versus_subtraction(self):
        toks = tacob.lex_line("wait-for-turn turret around y-axis; x = heading-pitch;", "t", 1)
        self.assertEqual([t.value for t in toks],
                         ["wait-for-turn", "turret", "around", "y-axis", ";", "x", "=",
                          "heading", "-", "pitch", ";"])

    def test_angle_literal_does_not_eat_comparisons(self):
        toks = tacob.lex_line("if( a < 5 && b > <90> )", "t", 1)
        kinds = [(t.kind, t.value) for t in toks]
        self.assertIn(("op", "<"), kinds)
        self.assertIn(("ang", 90.0), kinds)

    def test_comments_keep_line_numbers(self):
        text = "a /* two\nlines */ b // tail\nc"
        self.assertEqual(tacob.strip_comments(text), "a \n b \nc")

    def test_soft_keyword_as_identifier(self):
        code = compile_words(script("SetSpeed", "s1 = speed * 2;", "speed"))
        self.assertEqual(code, words("CREATE_LOCAL_VAR", "PUSH_LOCAL_VAR", 0, "PUSH_CONSTANT", 2,
                                     "MUL", "POP_STATIC_VAR", 0, "PUSH_CONSTANT", 0, "RETURN"))


class Preprocessing(unittest.TestCase):
    def test_define_include_ifdef(self):
        text = ('#include "exptype.h"\n#define SIG_AIM 2\n#ifdef SIG_AIM\n' + PRELUDE +
                'F()\n{\n\tsignal SIG_AIM;\n\texplode base type SHATTER | BITMAP1;\n}\n'
                '#else\nthis is not compiled\n#endif\n')
        code = compile_words(text, reader=HEADERS.get)
        self.assertEqual(code, words("PUSH_CONSTANT", 2, "SIGNAL", "PUSH_CONSTANT", 1,
                                     "PUSH_CONSTANT", 256, "BITWISE_OR", "EXPLODE", 0,
                                     "PUSH_CONSTANT", 0, "RETURN"))

    def test_function_like_macro_refused(self):
        with self.assertRaises(tacob.CompileError):
            compile_words("#define SQ(x) x*x\n" + script("F", "sleep 1;"))

    def test_missing_include_and_bad_directive(self):
        with self.assertRaises(tacob.CompileError):
            compile_words('#include "nope.h"\n', reader=lambda n: None)
        with self.assertRaises(tacob.CompileError):
            compile_words("#if 1\n#endif\n")

    def test_headers_agree_with_the_tables(self):
        pre = tacob.Preprocessor(reader=HEADERS.get)
        pre.run('#include "exptype.h"\n#include "sfxtype.h"\n', "t")
        got = {name: toks[0].value for name, toks in pre.macros.items()}
        self.assertEqual(got, {**tacob.EXPLODE_FLAGS, **tacob.SFX_TYPES})


class CodeShapes(unittest.TestCase):
    def test_prologue_params_then_vars_hoisted(self):
        code = compile_words(script("F", "var a;\n\tif( p ) { var b; b = a; }", "p"))
        self.assertEqual(code[:3], words("CREATE_LOCAL_VAR", "CREATE_LOCAL_VAR", "CREATE_LOCAL_VAR"))
        self.assertIn(OP["POP_LOCAL_VAR"], code)
        self.assertEqual(code[code.index(OP["POP_LOCAL_VAR"]) + 1], 2)  # b is local 2

    def test_move_turn_push_trailing_clause_first(self):
        code = compile_words(script("F", "turn turret to y-axis heading speed <90>;", "heading, pitch"))
        self.assertEqual(code[2:], words("PUSH_CONSTANT", 16384, "PUSH_LOCAL_VAR", 0, "TURN", 1, 1,
                                         "PUSH_CONSTANT", 0, "RETURN"))
        code = compile_words(script("F", "move barrel to z-axis [-6] now;"))
        self.assertEqual(code, words("PUSH_CONSTANT", -393216, "MOVE_NOW", 2, 2,
                                     "PUSH_CONSTANT", 0, "RETURN"))

    def test_spin_defaults_and_order(self):
        code = compile_words(script("F", "spin turret around y-axis speed <150>;"))
        self.assertEqual(code, words("PUSH_CONSTANT", 0, "PUSH_CONSTANT", 27306, "SPIN", 1, 1,
                                     "PUSH_CONSTANT", 0, "RETURN"))
        code = compile_words(script("F", "stop-spin turret around y-axis decelerate <10>;"))
        self.assertEqual(code, words("PUSH_CONSTANT", 1820, "STOP_SPIN", 1, 1,
                                     "PUSH_CONSTANT", 0, "RETURN"))

    def test_get_forms(self):
        code = compile_words(script("F", "s1 = get HEALTH;"))
        self.assertEqual(code, words("PUSH_CONSTANT", 4, "GET_UNIT_VALUE", "POP_STATIC_VAR", 0,
                                     "PUSH_CONSTANT", 0, "RETURN"))
        code = compile_words(script("F", "s1 = get PIECE_XZ( turret );"))
        self.assertEqual(code, words("PUSH_CONSTANT", 7, "PUSH_CONSTANT", 1, "PUSH_CONSTANT", 0,
                                     "PUSH_CONSTANT", 0, "PUSH_CONSTANT", 0, "GET",
                                     "POP_STATIC_VAR", 0, "PUSH_CONSTANT", 0, "RETURN"))
        with self.assertRaises(tacob.CompileError):
            compile_words(script("F", "s1 = get NOTHING;"))

    def test_set_and_attach(self):
        code = compile_words(script("F", "set INBUILDSTANCE to 1;\n\tattach-unit u to turret;\n"
                                         "\tdrop-unit u;", "u"))
        self.assertEqual(code[1:], words("PUSH_CONSTANT", 5, "PUSH_CONSTANT", 1, "SET",
                                         "PUSH_LOCAL_VAR", 0, "PUSH_CONSTANT", 1, "PUSH_CONSTANT", 0,
                                         "ATTACH_UNIT", "PUSH_LOCAL_VAR", 0, "DROP_UNIT",
                                         "PUSH_CONSTANT", 0, "RETURN"))

    def test_if_while_else(self):
        code = compile_words(script("F", "while( s1 ) { if( s2 ) { sleep 1; } }"))
        self.assertEqual(code, words("PUSH_STATIC_VAR", 0, "JUMP_NOT_EQUAL", 13,
                                     "PUSH_STATIC_VAR", 1, "JUMP_NOT_EQUAL", 11,
                                     "PUSH_CONSTANT", 1, "SLEEP", "JUMP", 0,
                                     "PUSH_CONSTANT", 0, "RETURN"))
        code = compile_words(script("F", "if( s1 ) sleep 1; else sleep 2;"))
        self.assertEqual(code, words("PUSH_STATIC_VAR", 0, "JUMP_NOT_EQUAL", 9,
                                     "PUSH_CONSTANT", 1, "SLEEP", "JUMP", 12,
                                     "PUSH_CONSTANT", 2, "SLEEP",
                                     "PUSH_CONSTANT", 0, "RETURN"))

    def test_precedence_left_assoc_no_folding(self):
        code = compile_words(script("F", "s1 = 4 | 8 | 16 & 2 + 3 * TRUE;"))
        self.assertEqual(code, words("PUSH_CONSTANT", 4, "PUSH_CONSTANT", 8, "BITWISE_OR",
                                     "PUSH_CONSTANT", 16, "PUSH_CONSTANT", 2, "PUSH_CONSTANT", 3,
                                     "PUSH_CONSTANT", 1, "MUL", "ADD", "BITWISE_AND", "BITWISE_OR",
                                     "POP_STATIC_VAR", 0, "PUSH_CONSTANT", 0, "RETURN"))

    def test_implicit_return_rule(self):
        # an empty script is exactly the implicit return
        self.assertEqual(compile_words(script("F", "")), words("PUSH_CONSTANT", 0, "RETURN"))
        # a script whose last opcode is already RETURN gets none — even inside an if
        code = compile_words(script("F", "if( s1 ) { return (0); }"))
        self.assertEqual(code, words("PUSH_STATIC_VAR", 0, "JUMP_NOT_EQUAL", 7,
                                     "PUSH_CONSTANT", 0, "RETURN"))
        code = compile_words(script("F", "return (1);"))
        self.assertEqual(code, words("PUSH_CONSTANT", 1, "RETURN"))
        # two scripts: the second's emptiness is judged on its own words
        two = PRELUDE + "A()\n{\n\treturn (1);\n}\nB()\n{\n}\n"
        cob = tacob.compile_bos(two, "t")
        self.assertEqual(cob.code, words("PUSH_CONSTANT", 1, "RETURN", "PUSH_CONSTANT", 0, "RETURN"))
        self.assertEqual(cob.scripts, [("A", 0), ("B", 3)])

    def test_calls_and_errors(self):
        text = PRELUDE + "A(x, y)\n{\n}\nB()\n{\n\tstart-script A(1, s1);\n\tcall-script A();\n}\n"
        cob = tacob.compile_bos(text, "t")
        self.assertEqual(cob.code[5:], words("PUSH_CONSTANT", 1, "PUSH_STATIC_VAR", 0,
                                             "START_SCRIPT", 0, 2, "CALL_SCRIPT", 0, 0,
                                             "PUSH_CONSTANT", 0, "RETURN"))
        for bad in ("start-script Nope();", "show nothing;", "x = 1;", "for(;;) {}",
                    "play-sound \"x\";", "sleep 1"):
            with self.assertRaises(tacob.CompileError, msg=bad):
                compile_words(script("F", bad))


class Container(unittest.TestCase):
    def test_layout_is_scriptor_layout(self):
        cob = tacob.Cob([("Create", 0), ("Killed", 3)], ["base", "turret"], 1,
                        words("PUSH_CONSTANT", 0, "RETURN", "PUSH_CONSTANT", 0, "RETURN"))
        blob = tacob.write_cob(cob)
        header = struct.unpack_from("<11I", blob, 0)
        code_bytes = 6 * 4
        self.assertEqual(header, (4, 2, 2, 6, 1, 0,
                                  44 + code_bytes,            # entry table
                                  44 + code_bytes + 8,        # script-name table
                                  44 + code_bytes + 16,       # piece-name table
                                  44,                         # code
                                  44 + code_bytes + 24))      # strings
        self.assertEqual(blob[44 + code_bytes + 24:], b"Create\0Killed\0base\0turret\0")
        back = tacob.read_cob(blob)
        self.assertEqual((back.scripts, back.pieces, back.statics, back.code),
                         (cob.scripts, cob.pieces, 1, cob.code))
        self.assertEqual(tacob.write_cob(back), blob)

    def test_refuses_other_versions_and_junk(self):
        blob = bytearray(tacob.write_cob(tacob.Cob([], [], 0, [])))
        struct.pack_into("<I", blob, 0, 6)
        with self.assertRaises(tacob.TacobError):
            tacob.read_cob(bytes(blob))
        with self.assertRaises(tacob.TacobError):
            tacob.read_cob(b"\x04\0\0\0short")


class Decompiling(unittest.TestCase):
    SOURCE = ('#include "exptype.h"\n#include "sfxtype.h"\n\n'
              'piece base, turret, barrel, flare;\n\nstatic-var static_var_1;\n\n'
              'AimPrimary(heading, pitch)\n{\n'
              '\tsignal 2;\n\tset-signal-mask 2;\n'
              '\tturn turret to y-axis heading speed <90>;\n'
              '\tturn barrel to x-axis <0> - pitch speed <50>;\n'
              '\twait-for-turn turret around y-axis;\n'
              '\tif( static_var_1 )\n\t{\n\t\tsleep 100;\n\t}\n\telse\n\t{\n\t\tsleep 200;\n\t}\n'
              '\twhile( get BUILD_PERCENT_LEFT )\n\t{\n\t\tsleep 400;\n\t}\n'
              '\treturn (1);\n}\n\n'
              'QueryPrimary(piecenum)\n{\n\tpiecenum = flare;\n}\n\n'
              'Killed(severity, corpsetype)\n{\n'
              '\tvar var1;\n'
              '\tvar1 = get PIECE_XZ( turret ) - 2457600;\n'
              '\texplode barrel type FALL | SMOKE | FIRE | EXPLODE_ON_HIT | BITMAP1;\n'
              '\temit-sfx SFXTYPE_BLACKSMOKE from base;\n'
              '\tspin turret around y-axis speed <150> accelerate <10>;\n'
              '\tattach-unit var1 to turret;\n'
              '\tset ARMORED to (var1 + 1) * 2;\n'
              '\tif( severity <= 25 == 1 )\n\t{\n\t\tcorpsetype = 1;\n\t}\n}\n')

    def test_round_trip_is_exact_and_readable(self):
        cob = tacob.compile_bos(self.SOURCE, "t", reader=HEADERS.get)
        text, failures = tacob.decompile_cob(cob, "t")
        self.assertEqual(failures, [])
        # the decompiler's text is the source, minus the header comment it adds
        self.assertEqual(text.split("\n", 1)[1], self.SOURCE)
        again = tacob.compile_bos(text, "t2", reader=HEADERS.get)
        self.assertEqual(tacob.write_cob(again), tacob.write_cob(cob))

    def test_names_from_conventions_and_callers(self):
        text = PRELUDE + "Helper(a, b)\n{\n}\nCreate()\n{\n\tcall-script Helper(1, 2);\n}\n"
        cob = tacob.compile_bos(text, "t")
        out, failures = tacob.decompile_cob(cob)
        self.assertEqual(failures, [])
        self.assertIn("Helper(arg1, arg2)", out)
        self.assertEqual(tacob.weapon_params("AimWeapon7"), ["heading", "pitch"])
        self.assertEqual(tacob.weapon_params("QueryWeapon12"), ["piecenum"])

    def test_refuses_unstructured_code_loudly(self):
        # a forward JUMP that is neither a while nor an else: no goto is ever emitted
        code = words("PUSH_CONSTANT", 1, "JUMP", 5, "PUSH_CONSTANT", 0, "RETURN")
        cob = tacob.Cob([("Create", 0)], ["base"], 0, code)
        text, failures = tacob.decompile_cob(cob)
        self.assertEqual(len(failures), 1)
        self.assertIn("word 2", failures[0][1])
        self.assertIn("could not decompile Create", text)
        self.assertNotIn("goto", text)
        # an unknown opcode names its word too
        cob = tacob.Cob([("Create", 0)], [], 0, [0x12345678, OP["RETURN"]])
        _text, failures = tacob.decompile_cob(cob)
        self.assertIn("0x12345678", failures[0][1])

    def test_roundtrip_one_reports_the_first_differing_word(self):
        cob = tacob.compile_bos(self.SOURCE, "t", reader=HEADERS.get)
        blob = tacob.write_cob(cob)
        result, _ = tacob.roundtrip_one(blob, "t")
        self.assertTrue(result["ok"], result)
        # a static var the code never touches still has to survive as a declaration
        cob.statics = 3
        result, text = tacob.roundtrip_one(tacob.write_cob(cob), "t")
        self.assertTrue(result["ok"], result)
        self.assertIn("static_var_1, static_var_2, static_var_3", text)


if __name__ == "__main__":
    unittest.main()
