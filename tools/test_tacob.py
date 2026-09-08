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
tacob_app = SourceFileLoader(
    "tacob_app", str(Path(__file__).with_name("tacob_app.py"))).load_module()
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


class Vm(unittest.TestCase):
    """The engine rules the VM reproduces (exe-reverse-engineering.md §"The COB engine").

    The nine-fixture gate is `tools/tacob run --all`, which needs the game's archives
    and the landing-2 logs; these pin the rules a fixture would only fail on obliquely,
    and the two paths no stock script reaches at all — the refused start and the
    opcodes this engine does not implement."""

    def build(self, source, **kw):
        return tacob.compile_bos(source, "test.bos", **kw)

    def run_vm(self, source, script="Create", ticks=0, args=(), run_now=True, **kw):
        cob = self.build(source, **kw)
        out = []
        vm = tacob.CobVM(cob, tacob.UnitWorld(), out.append, unit=1, type_name="T")
        vm.tick = 1
        vm.start_script(script, run_now=run_now, args=args, argwords=bool(args))
        for tick in range(2, 2 + ticks):
            vm.tick = tick
            vm.do_scripts_now(1)
        return vm, out

    def test_the_ninth_start_is_refused(self):
        # the silent failure extra-weapons.md snag 10 is about, and the one path no
        # stock fixture reaches: eight records busy, the ninth `start-script` returns -1
        source = PRELUDE + """
        sleeper()
        {
            sleep 10000;
        }
        Create()
        {
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
        }
        """
        vm, out = self.run_vm(source)
        # Create holds slot 0 while it runs, seven sleepers fill 1..7, and the eighth
        # and ninth `start-script`s find no record — two X lines, no eighth sleeper
        self.assertEqual([l.split("\t")[0] for l in out].count("X"), 2)
        self.assertIn("X\t1\t1\tsleeper\tC", out[-2])
        self.assertEqual(vm.running, 7)          # Create's own return freed slot 0
        self.assertEqual([t.status for t in vm.threads].count(tacob.ST_FREE), 1)

    def test_a_call_on_a_full_pool_blocks_the_caller_for_ever(self):
        source = PRELUDE + """
        sleeper()
        {
            sleep 10000;
        }
        Create()
        {
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            start-script sleeper();
            call-script sleeper();
            return (1);
        }
        """
        vm, _out = self.run_vm(source, ticks=40)
        self.assertEqual(vm.threads[0].child, -1)
        self.assertEqual(vm.threads[0].status, tacob.ST_CALL)   # nothing ever wakes it

    def test_sleep_truncates_onto_ticks(self):
        # 0x4B1363: ms * 30 / 1000, truncating — `sleep 150` is four ticks, not five
        vm, out = self.run_vm(script("Create", "sleep 150;\nreturn (7);"), ticks=6)
        self.assertEqual([l.split("\t")[0] for l in out], ["S", "R"])
        self.assertEqual(out[-1].split("\t")[1], "5")          # started tick 1, wakes tick 5

    def test_a_local_the_caller_did_not_pass_reads_the_record_s_last_words(self):
        # CREATE_LOCAL_VAR only bumps the index (0x4B1386) and nothing clears a record
        source = PRELUDE + """
        leave(a, b)
        {
            a = 111;
            b = 222;
        }
        peek(x, y)
        {
            s1 = x;
            s2 = y;
        }
        Create()
        {
            call-script leave(0, 0);
            start-script peek();
        }
        """
        vm, _out = self.run_vm(source, ticks=3)
        self.assertEqual(vm.statics[:2], [111, 222])

    def test_turn_takes_the_short_way(self):
        # 0x4B0FBA: the speed is negated when (|delta| > 0x8000) xor (delta < 0)
        vm, _ = self.run_vm(script("Create", "turn base to y-axis <-1> speed <360>;\n"
                                             "wait-for-turn base around y-axis;"), ticks=3)
        self.assertEqual(vm.ang[0][1], 65354)                   # <-1> = -182 & 0xFFFF
        base = 0 * tacob.PIECE_FIELDS
        self.assertEqual(vm.anim[base + tacob.F_TURN_SPEED + 1], 0)

    def test_signal_frees_a_thread_without_running_its_return(self):
        source = PRELUDE + """
        victim()
        {
            sleep 10000;
        }
        Create()
        {
            start-script victim();
            set-signal-mask 2;
            sleep 100;
            signal 1;
            return (0);
        }
        """
        # the child inherits the mask the parent had *when it started* (0x4B18EB), so
        # Create's later `set-signal-mask 2` keeps its own `signal 1` off itself
        _vm, out = self.run_vm(source, ticks=8)
        kinds = [l.split("\t")[0] for l in out]
        self.assertEqual(kinds.count("K"), 1)
        self.assertEqual(out[kinds.index("K")].split("\t")[4], "victim")
        self.assertEqual(kinds.count("R"), 1)                   # only Create's

    def test_a_script_without_a_return_runs_into_the_next_one(self):
        # measured on ARMSTUMP: HitByWeapon's thread ends under SweetSpot's name
        source = PRELUDE + """
        first()
        {
            if( s1 )
            {
                return (0);
            }
        }
        second()
        {
            s2 = 9;
        }
        """
        cob = self.build(source)
        entry = dict(cob.scripts)
        # no implicit return was appended: the only RETURN in `first` is the one inside
        # the `if`, so the false branch walks straight into `second`'s first word
        self.assertEqual(cob.code[entry["first"]:entry["second"]].count(OP["RETURN"]), 1)
        out = []
        vm = tacob.CobVM(cob, tacob.UnitWorld(), out.append, unit=1, type_name="T")
        vm.tick = 1
        vm.start_script("first", run_now=True)          # s1 is 0: the `if` falls off the end
        self.assertEqual(vm.statics, [0, 9])
        self.assertEqual(out[-1].split("\t")[4], "second")     # the R line names the body

    def test_play_sound_and_map_command_kill_the_thread(self):
        # 0x4B1B48 tests only SET/ATTACH/DROP above EXPLODE: 0x10072000 falls into
        # the silent kill at 0x4B1B60, so this engine has no `play-sound`
        cob = tacob.Cob([("Create", 0)], ["base"], 0,
                        [tacob.OP["PLAY_SOUND"], 1, tacob.OP["PUSH_CONSTANT"], 0,
                         tacob.OP["RETURN"]])
        out = []
        vm = tacob.CobVM(cob, tacob.UnitWorld(), out.append, unit=1, type_name="T")
        vm.tick = 1
        vm.start_script("Create", run_now=True)
        self.assertEqual([l.split("\t")[0] for l in out], ["S"])   # no R line: killed
        self.assertEqual(vm.running, 0)

    def test_mod_is_the_same_handler_as_div(self):
        # 0x10034001 & 0x100FF000 == 0x10034000: the dispatch masks the low bits away
        vm, _ = self.run_vm(script("Create", "s1 = 17 % 5;\ns2 = 17 / 5;"))
        self.assertEqual(vm.statics, [3, 3])

    def test_a_run_now_start_steps_every_record(self):
        # 0x4B0B86: runNow runs all eight with dt = 0, not only the new thread
        source = PRELUDE + """
        other()
        {
            s2 = s2 + 1;
            sleep 10000;
        }
        Create()
        {
            start-script other();
            s1 = 1;
        }
        """
        vm, _ = self.run_vm(source)
        self.assertEqual(vm.statics, [1, 1])                    # `other` ran inside the call


class Director(unittest.TestCase):
    def test_the_call_site_table_answers_every_start_the_fixtures_make(self):
        for name in ("create", "aimprimary", "queryprimary", "sweetspot", "killed",
                     "startmoving", "setmaxreloadtime", "rockunit", "activate"):
            self.assertIn(name, tacob.ENGINE_STARTS)
        kinds = {k for k, _ in tacob.ENGINE_STARTS.values()}
        self.assertEqual(kinds, {"query", "now", "later"})
        phases = {p for _, p in tacob.ENGINE_STARTS.values()}
        self.assertEqual(phases, {"pre", "post"})

    def test_health_comes_out_of_smokeunit_s_rand_spacing(self):
        # trunc(h * 50 * 30 / 1000): a 51-tick gap names health 34, a 28-tick gap 19
        log = ["S\t91\t1\tT\tSmokeUnit\t1\tC:0\t",
               "D\t241\t1\t1\t58", "D\t292\t1\t1\t64", "D\t320\t1\t1\t3"]
        lines = [tacob.TraceLine(l) for l in log]
        timeline, gaps = tacob.fit_health(lines)
        self.assertEqual(gaps, [])
        self.assertEqual(timeline, [(0, 100), (241, 34), (292, 19), (320, 19)])

    def test_a_draw_from_another_script_is_not_health(self):
        log = ["S\t91\t1\tT\tMoveRate2\t0\tE\t", "D\t202\t1\t0\t8"]
        timeline, _ = tacob.fit_health([tacob.TraceLine(l) for l in log])
        self.assertEqual(timeline, [])


class ValueIds(unittest.TestCase):
    """`get` and `set`, as 0x480770 and 0x480B20 compute them — read out of the
    retail binary on 2026-09-07, exe-reverse-engineering.md §"`get` and `set`"."""

    def world(self, **kw):
        w = tacob.EditorWorld()
        for key, value in kw.items():
            setattr(w, key, value)
        return w

    def test_the_xz_pack_survives_a_negative_z(self):
        # 0x480821 *adds* the z integer to the x half instead of or-ing it, so the
        # borrow has to be undone on the way back — which every unpacking handler does
        for x, z in ((100, 200), (100, -200), (-100, -200), (0, -1), (-1, 0)):
            packed = tacob.pack_xz(x << 16, z << 16)
            back = tacob.unpack_xz(packed)
            self.assertEqual(back, (x << 16, z << 16), (x, z))

    def test_xz_atan_subtracts_the_unit_heading_and_atan_does_not(self):
        w = self.world()
        w.body[1] = 0x4000                               # a quarter turn
        packed = tacob.pack_xz(10 << 16, 0)              # due +x from the unit
        self.assertEqual(w.get(tacob.VALUE_IDS["XZ_ATAN"], packed, 0, 0, 0), 0)
        self.assertEqual(w.get(tacob.VALUE_IDS["ATAN"], 10 << 16, 0, 0, 0), 0x4000)

    def test_hypot_truncates_and_piece_y_is_raw_fixed_point(self):
        w = self.world()
        # _ftol chops: hypot(3, 4) of 16.16 values is exactly 5 << 16, and a value
        # just under it must come back one short, not rounded up
        self.assertEqual(w.get(tacob.VALUE_IDS["HYPOT"], 3 << 16, 4 << 16, 0, 0), 5 << 16)
        self.assertEqual(w.get(tacob.VALUE_IDS["HYPOT"], 1, 0, 0, 0), 1)
        self.assertEqual(tacob.ta_hypot(65535.9, 0), 65535)

    def test_build_percent_left_is_zero_only_when_it_is_finished(self):
        w = self.world()
        vid = tacob.VALUE_IDS["BUILD_PERCENT_LEFT"]
        self.assertEqual(w.get(vid, 0, 0, 0, 0), 0)      # build_left 0.0 -> the early out
        w.build_left = 1.0
        self.assertEqual(w.get(vid, 0, 0, 0, 0), 100)    # 1 - (int)(1.0 * -99)
        w.build_left = 0.5
        self.assertEqual(w.get(vid, 0, 0, 0, 0), 50)     # 1 - (int)(-49.5), chopped
        w.build_left = 0.001
        self.assertEqual(w.get(vid, 0, 0, 0, 0), 1)      # never 0 while it is building

    def test_health_is_a_percent_and_ids_over_twenty_are_zero(self):
        w = self.world(hp=125, maxhp=250)
        self.assertEqual(w.get(tacob.VALUE_IDS["HEALTH"], 0, 0, 0, 0), 50)
        self.assertEqual(w.get(107, 0, 0, 0, 0), 0)      # HEALTH_VAL, a TADR extension
        self.assertIn(107, w.unknown)

    def test_set_writes_six_ids_and_silently_drops_the_other_fourteen(self):
        w = self.world()
        w.set(tacob.VALUE_IDS["ACTIVATION"], 1)
        self.assertEqual(w.get(tacob.VALUE_IDS["ACTIVATION"], 0, 0, 0, 0), 1)
        w.set(tacob.VALUE_IDS["HEALTH"], 7)              # no case in 0x480B20
        self.assertEqual(w.get(tacob.VALUE_IDS["HEALTH"], 0, 0, 0, 0), 100)
        self.assertEqual([vid for _tick, vid in w.ignored_sets],
                         [tacob.VALUE_IDS["HEALTH"]])
        self.assertEqual(len(tacob.SET_HANDLED), 6)

    def test_the_sim_rng_is_park_miller_and_refuses_a_range_below_two(self):
        rng = tacob.SimRandom(1)
        self.assertEqual(rng.draw(1), 0)                 # 0x4B6C38: n < 2 -> 0, no draw
        self.assertEqual(rng.seed, 1)
        self.assertEqual(rng.draw(1000000), 16807 % 1000000)
        self.assertEqual(rng.seed, 16807)
        self.assertEqual(rng.draw(1000000), (16807 * 16807) % 2147483647 % 1000000)


class PieceTransform(unittest.TestCase):
    """`0x43DEF0` + `0x4B6CC0`: the composition the pose and PIECE_XZ both use.

    `tools/tacob pose-check --all` is the live gate — it rebuilds the eight
    fixtures' posed vertices and diffs them against the engine's own vertex
    buffer. These pin the parts a fixture cannot: no stock unit in the corpus
    turns one piece about two axes at once, so only a synthetic model can show
    that the order is Z, X, Y and not one of the other five."""

    def frame(self):
        f = tacob.PieceFrame(["root", "arm"])
        f.offset = [[0, 0, 0], [65536, 0, 0]]            # the arm one unit out on +x
        f.parent = [-1, 0]
        return f

    def test_a_zero_angle_word_leaves_the_pair_untouched(self):
        self.assertEqual(tacob.rotate2(0, 12345, -678), (12345, -678))

    def test_the_order_is_z_then_x_then_y(self):
        f = self.frame()
        pos = [[0, 0, 0], [0, 0, 0]]
        ang = [[0x4000, 0x4000, 0], [0, 0, 0]]           # the root turns about x and y
        got = tacob.piece_offset(f, pos, ang, 1)
        # Rz(0) then Rx(90) then Ry(90) on (1,0,0): x/y untouched, y/z untouched,
        # then the x/z pair rotates x into z -> (0, 0, 1), and the return negates z
        self.assertEqual(got, [0, 0, -65536])
        # the other order (Y then X) would leave it at (0, -1, 0) instead
        self.assertNotEqual(got, [0, -65536, 0])

    def test_a_piece_s_own_turn_does_not_move_its_origin_but_moves_its_mesh(self):
        f = self.frame()
        pos = [[0, 0, 0], [0, 0, 0]]
        ang = [[0, 0, 0], [0, 0x4000, 0]]                # the arm turns about y
        self.assertEqual(tacob.piece_offset(f, pos, ang, 1), [65536, 0, 0])
        # a point one unit along the arm's own +x does swing round to +z
        self.assertEqual(tacob.piece_vertex(f, pos, ang, 1, [65536, 0, 0]),
                         [65536, 0, 65536])

    def test_move_is_a_delta_in_the_parent_frame_added_before_the_rotation(self):
        f = self.frame()
        ang = [[0, 0x4000, 0], [0, 0, 0]]                # the root turns a quarter
        self.assertEqual(tacob.piece_offset(f, [[0, 0, 0], [0, 0, 0]], ang, 1),
                         [0, 0, -65536])
        # MOVE the arm another unit out: it must swing with the root, not stay on +x
        self.assertEqual(tacob.piece_offset(f, [[0, 0, 0], [65536, 0, 0]], ang, 1),
                         [0, 0, -131072])

    def test_the_body_turn_goes_on_at_the_root_only(self):
        f = self.frame()
        pos = [[0, 0, 0], [0, 0, 0]]
        ang = [[0, 0, 0], [0, 0, 0]]
        self.assertEqual(tacob.piece_offset(f, pos, ang, 1, body=(0, 0x4000, 0)),
                         [0, 0, -65536])
        self.assertEqual(tacob.piece_offset(f, pos, ang, 0, body=(0, 0x4000, 0)),
                         [0, 0, 0])          # the root's own origin does not move


class Lints(unittest.TestCase):
    """Each rule is a snag that cost a live run; the message says which note."""

    def lint(self, source, frame=None):
        program, diags = tacob.lint_bos(source, "test.bos", frame=frame)
        self.assertIsNotNone(program, diags)
        return {d["rule"]: d for d in diags}

    def test_an_aim_script_for_slot_four_may_not_wait(self):
        found = self.lint(PRELUDE + """
        AimWeapon4(heading, pitch)
        {
            turn turret to y-axis heading speed <300>;
            wait-for-turn turret around y-axis;
            return (1);
        }
        """)
        self.assertEqual(found["aim-waits"]["level"], "error")
        self.assertIn("aim-no-signal", found)
        self.assertIn("extra-weapons.md", found["aim-waits"]["message"])

    def test_a_stock_aim_that_waits_wants_the_signal_pair(self):
        body = """
            %s
            turn turret to y-axis heading speed <300>;
            wait-for-turn turret around y-axis;
            return (1);
        """
        pair = "signal SIG_AIM; set-signal-mask SIG_AIM;"
        self.assertIn("aim-no-signal",
                      self.lint(PRELUDE + "AimPrimary(heading, pitch)\n{" + body % "" + "}"))
        self.assertNotIn("aim-no-signal",
                         self.lint("#define SIG_AIM 2\n" + PRELUDE +
                                   "AimPrimary(heading, pitch)\n{" + body % pair + "}"))

    def test_a_set_the_engine_drops_and_a_value_id_it_does_not_have(self):
        found = self.lint(PRELUDE + """
        Create()
        {
            set HEALTH to 50;
            static_1 = get 107;
        }
        """.replace("static_1", "s1"))
        self.assertEqual(found["set-ignored"]["level"], "warning")
        self.assertIn("0x480B20", found["set-ignored"]["message"])
        self.assertIn("get-extension", found)

    def test_a_muzzle_piece_create_never_hides(self):
        source = PRELUDE + """
        Create() { hide barrel; }
        QueryPrimary(piecenum) { piecenum = flare; return (0); }
        """
        self.assertIn("query-piece-shown", self.lint(source))
        # ... and not when Create does hide it
        hidden = source.replace("hide barrel;", "hide barrel; hide flare;")
        self.assertNotIn("query-piece-shown", self.lint(hidden))
        # ... nor when the model builder hides it for us (0x45AF1B, under three vertices)
        frame = tacob.PieceFrame(["base", "turret", "barrel", "flare"])
        frame.vertices = [8, 8, 8, 1]
        self.assertNotIn("query-piece-shown", self.lint(source, frame=frame))

    def test_the_thread_peak_counts_what_can_be_live_together(self):
        holder = """
        %s()
        {
            turn turret to y-axis <0> speed <300>;
            wait-for-turn turret around y-axis;
            return (1);
        }
        """
        names = ["AimPrimary", "AimSecondary", "AimTertiary"] + \
                [f"AimWeapon{n}" for n in range(4, 12)]
        found = self.lint(PRELUDE + "".join(holder % n for n in names))
        self.assertIn("thread-peak", found)
        self.assertIn("the pool is 8", found["thread-peak"]["message"])
        self.assertNotIn("thread-peak", self.lint(PRELUDE + (holder % "AimPrimary")))

    def test_a_piece_the_model_does_not_have(self):
        frame = tacob.PieceFrame(["base", "turret", "barrel", "flare"])
        frame.bound = [True, True, True, False]
        found = self.lint(PRELUDE + "Create() { hide flare; }", frame=frame)
        self.assertIn("piece-unknown", found)
        self.assertIn("flare", found["piece-unknown"]["message"])

    def test_the_two_opcodes_this_engine_kills_the_thread_on(self):
        for statement in ("play-sound 1;", "map-command 1, 2;"):
            with self.assertRaises(tacob.CompileError) as caught:
                tacob.compile_bos(PRELUDE + "Create() { %s }" % statement, "t.bos")
            self.assertIn("0x4B1B60", str(caught.exception))


class Editor(unittest.TestCase):
    """The template, the class guess, and the shape of a served frame."""

    def test_the_weapon_template_compiles_and_lints_clean(self):
        source = PRELUDE + tacob.weapon_template(4, "turret", "barrel", "flare")
        cob = tacob.compile_bos(source, "t.bos")
        self.assertEqual([n for n, _ in cob.scripts],
                         ["AimWeapon4", "FireWeapon4", "AimFromWeapon4", "QueryWeapon4"])
        _program, diags = tacob.lint_bos(source, "t.bos")
        self.assertEqual([d for d in diags if d["level"] == "error"], [])
        # the muzzle it hands back is a flare, so the lint asks Create to hide it
        self.assertEqual([d["rule"] for d in diags], ["query-piece-shown"])

    def test_the_template_returns_inside_its_own_tick(self):
        cob = tacob.compile_bos(PRELUDE + tacob.weapon_template(4, "turret", "barrel", "flare"),
                                "t.bos")
        vm = tacob.CobVM(cob, tacob.UnitWorld())
        vm.tick = 1
        cb = tacob.AimCallback()
        vm.start_script("AimWeapon4", run_now=True, args=(0, 0), argwords=True, cb=cb)
        self.assertEqual(cb.aimed, 1)                    # aimed before the tick is out
        self.assertEqual(vm.running, 0)                  # and holding no record

    def test_the_category_field_is_a_word_list_not_a_substring(self):
        peewee = {"category": "ARM KBOT LEVEL1 WEAPON NOTAIR NOTSUB CTRL_W",
                  "bmcode": "1", "canmove": "1"}
        self.assertEqual(tacob.classify(peewee), "kbot")     # "NOTSUB" is not a submarine
        snake = {"category": "CORE UNDERWATER LEVEL1 TORP WEAPON NOTAIR CTRL_W",
                 "bmcode": "1", "canmove": "1", "waterline": "20"}
        self.assertEqual(tacob.classify(snake), "sub")
        self.assertEqual(tacob.classify({"bmcode": "0"}), "building")
        self.assertEqual(tacob.classify({"canfly": "1", "hoverattack": "1"}), "gunship")

    def test_a_slot_with_no_aim_script_still_fires(self):
        # ARMHAWK, ARMBRAWL, ARMTHUND and CORSUB carry no Aim* at all: the engine
        # aims them and the script only points at the muzzle
        source = PRELUDE + """
        Create() { return (0); }
        FirePrimary() { show flare; sleep 100; hide flare; return (0); }
        QueryPrimary(piecenum) { piecenum = flare; return (0); }
        """
        cob = tacob.compile_bos(source, "t.bos")
        world = tacob.EditorWorld(tacob.PieceFrame(cob.pieces))
        vm = tacob.CobVM(cob, world)
        world.vm = vm
        slot = tacob.WeaponSlot(0, "TEST", 400, 10)
        director = tacob.Director(vm, world, motion="static", slots=[slot])
        world.target = [0, 0, 0]
        for tick in range(60):
            director.step(tick)
        self.assertGreater(slot.shots, 1)

    def test_a_stock_aim_is_not_restarted_while_one_is_still_slewing(self):
        # start one every tick and each new signal kills the last before it can
        # arrive — extra-weapons.md snag 1's `full` row, and what an early version
        # of this director did
        source = "#define SIG 2\n" + PRELUDE + """
        Create() { return (0); }
        AimPrimary(heading, pitch)
        {
            signal SIG;
            set-signal-mask SIG;
            turn turret to y-axis heading speed <60>;
            wait-for-turn turret around y-axis;
            return (1);
        }
        FirePrimary() { return (0); }
        """
        cob = tacob.compile_bos(source, "t.bos")
        out = []
        world = tacob.EditorWorld(tacob.PieceFrame(cob.pieces))
        vm = tacob.CobVM(cob, world, out.append)
        world.vm = vm
        slot = tacob.WeaponSlot(0, "TEST", 4000, 10)
        director = tacob.Director(vm, world, motion="static", slots=[slot])
        world.target = [100 * 65536, 0, 100 * 65536]
        for tick in range(120):
            director.step(tick)
        self.assertEqual([l for l in out if l.startswith("K")], [])
        self.assertGreater(slot.shots, 0)


class Serve(unittest.TestCase):
    """The server the page polls, on a temporary project and no game files.

    It exists mostly for the two rules a route must not break: a POST may not
    name where the tool writes, and a page from somewhere else may not drive it."""

    SOURCE = (PRELUDE + "Create()\n{\n\tspin turret around y-axis speed <180>;\n}\n"
              "AimPrimary(heading, pitch)\n{\n\treturn (1);\n}\n")

    def setUp(self):
        import json
        import tempfile
        self.tmp = tempfile.TemporaryDirectory(prefix="tacob-serve-")
        root = Path(self.tmp.name) / "unitx"
        root.mkdir()
        (root / "unitx.bos").write_text(self.SOURCE, encoding="latin-1")
        (root / "project.json").write_text(json.dumps({"unit": "unitx", "class": "tank",
                                                       "fbi": {}}))
        self.project = tacob.Project(root)
        self.session = tacob.Session(self.project)
        self.server = tacob.Server(self.session, port=0)
        self.server.start()

    def tearDown(self):
        self.server.close()
        self.tmp.cleanup()

    def get(self, path):
        import json
        import urllib.request
        with urllib.request.urlopen(self.server.base + path, timeout=5) as res:
            return json.loads(res.read())

    def post(self, path, body, origin=None):
        import json
        import urllib.error
        import urllib.request
        req = urllib.request.Request(self.server.base + path,
                                     data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
        if origin:
            req.add_header("Origin", origin)
        try:
            with urllib.request.urlopen(req, timeout=5) as res:
                return res.status, json.loads(res.read())
        except urllib.error.HTTPError as err:
            return err.code, json.loads(err.read())

    def test_a_session_survives_a_model_it_cannot_resolve(self):
        # ta3do.die() prints and raises SystemExit, not Exception; the session must
        # survive it and keep what it said — this is also the no-game-directory path
        state = self.get("/state")
        self.assertIn("no model called", state["error"] or "")
        self.assertEqual(state["pieces"], ["base", "turret", "barrel", "flare"])
        self.assertIsNone(state["model"])

    def test_step_poses_the_model_and_the_trace_is_cobtrace(self):
        code, out = self.post("/transport", {"action": "step", "n": 30})
        self.assertEqual(code, 200)
        frame = self.get("/pose?tick=head")
        self.assertEqual(frame["tick"], 30)
        self.assertNotEqual(frame["ang"][1 * 3 + 1], 0)      # the turret is spinning
        self.assertEqual(len(frame["gl"]["t"]), 3 * 4)
        lines = self.get("/trace?from=0")["lines"]
        self.assertTrue(lines[0].startswith("S\t0\t1\tUNITX\tCreate\t0\tE"), lines[:2])

    def test_a_build_restarts_and_a_bad_one_changes_nothing(self):
        self.post("/transport", {"action": "step", "n": 10})
        code, out = self.post("/build", {"source": self.SOURCE.replace("<180>", "<90>")})
        self.assertTrue(out["ok"], out)
        self.assertEqual(self.get("/state")["tick"], 0)      # restarted from Create
        code, bad = self.post("/build", {"source": "Broken(\n"})
        self.assertFalse(bad["ok"])
        self.assertEqual(bad["diagnostics"][0]["rule"], "parse")
        self.assertEqual(self.project.bos.read_text(encoding="latin-1").count("<90>"), 1)

    def test_pack_writes_only_where_the_operator_said(self):
        outside = Path(self.tmp.name) / "outside.ufo"
        code, out = self.post("/pack", {"out": str(outside), "install": self.tmp.name})
        self.assertEqual(code, 200, out)
        self.assertFalse(outside.exists(), "the request body named the output path")
        self.assertNotIn("installed", out, "the request body named an install directory")
        self.assertEqual(Path(out["path"]), self.project.path / "unitx.ufo")

    def test_a_page_from_somewhere_else_cannot_drive_it(self):
        code, out = self.post("/transport", {"action": "step"},
                              origin="http://evil.example")
        self.assertEqual(code, 403)
        self.assertIn("cross-origin", out["error"])
        self.assertEqual(self.get("/state")["tick"], 0)
        for host in ("127.0.0.1", "localhost"):
            code, _out = self.post("/transport", {"action": "step"},
                                   origin=f"http://{host}:{self.server.port}")
            self.assertEqual(code, 200, host)


class Packaging(unittest.TestCase):
    """Landing 5: the paths, the config and the import map the packaged folder needs.

    Every one of these is a *layout* rule rather than a behaviour, and layout is
    what breaks when the same code runs out of a PyInstaller folder instead of a
    checkout — so each is pinned here rather than found again in a bundle, where
    the symptom is always a path and never says which one."""

    def setUp(self):
        import os
        import tempfile
        self.tmp = tempfile.TemporaryDirectory(prefix="tacob-pack-")
        self.home = Path(self.tmp.name)
        self.old = os.environ.get("TACOB_HOME")
        os.environ["TACOB_HOME"] = str(self.home)

    def tearDown(self):
        import os
        if self.old is None:
            os.environ.pop("TACOB_HOME", None)
        else:
            os.environ["TACOB_HOME"] = self.old
        self.tmp.cleanup()

    def test_the_user_directory_is_where_projects_and_config_go(self):
        self.assertEqual(tacob.user_dir(), self.home)
        self.assertEqual(tacob.projects_dir(), self.home / "projects")
        self.assertEqual(tacob.read_config(), {})
        tacob.write_config({"gamedir": "/somewhere", "unit": "armpw"})
        self.assertEqual(tacob.read_config()["unit"], "armpw")
        self.assertTrue((self.home / "config.json").is_file())

    def test_a_config_that_is_not_a_dict_is_no_config(self):
        (self.home / "config.json").write_text("[1, 2]")
        self.assertEqual(tacob.read_config(), {})
        (self.home / "config.json").write_text("{oops")
        self.assertEqual(tacob.read_config(), {})

    def test_the_game_folder_comes_from_the_flag_then_the_config(self):
        import os
        self.assertIsNone(tacob.resolve_gamedir())
        tacob.write_config({"gamedir": str(self.home / "saved")})
        self.assertEqual(tacob.resolve_gamedir(), str(self.home / "saved"))
        self.assertEqual(tacob.resolve_gamedir("/asked/for"), "/asked/for")
        # $TA3DO_GAMEDIR wins over the config, and `ta3do` is what reads it — so
        # the answer here is None, meaning "do not override what ta3do decides".
        os.environ["TA3DO_GAMEDIR"] = str(self.home)
        try:
            self.assertIsNone(tacob.resolve_gamedir())
            self.assertEqual(tacob.resolve_gamedir("/asked/for"), "/asked/for")
        finally:
            os.environ.pop("TA3DO_GAMEDIR")

    def test_a_folder_is_a_game_folder_when_an_archive_is_in_it(self):
        self.assertFalse(tacob.looks_like_gamedir(self.home))
        self.assertFalse(tacob.looks_like_gamedir(self.home / "nothing-here"))
        (self.home / "TOTALA1.HPI").write_bytes(b"not really, but named right")
        self.assertTrue(tacob.looks_like_gamedir(self.home))

    def test_a_folder_with_no_archives_says_so_instead_of_exiting(self):
        # ta3do.die() raises SystemExit; the packaged tool must answer with a
        # sentence a modder can act on, and the server must be able to 400 it.
        with self.assertRaises(tacob.TacobError) as caught:
            tacob.load_assets(str(self.home))
        self.assertIn("no Total Annihilation archives", str(caught.exception))
        with self.assertRaises(tacob.TacobError) as caught:
            tacob.load_assets(str(self.home / "not-there"))
        self.assertIn("no game folder at", str(caught.exception))

    def test_the_developer_gates_say_they_are_not_shipped(self):
        old = tacob.FIXTURES
        tacob.FIXTURES = self.home / "no-fixtures"
        try:
            with self.assertRaises(tacob.TacobError) as caught:
                tacob.fixtures_dir()
        finally:
            tacob.FIXTURES = old
        self.assertIn("does not ship them", str(caught.exception))

    def test_webview2_is_never_found_off_windows(self):
        import sys
        if sys.platform != "win32":
            self.assertFalse(tacob.webview2_present())


class ImportMap(unittest.TestCase):
    """The one rewrite that makes the page work with no network: every CDN URL in
    its import map swapped for the copy `tacob-build.py vendor` fetched."""

    PAGE = ('<html>\n<script type="importmap">\n'
            '{ "imports": {\n'
            '  "three": "https://cdn.jsdelivr.net/npm/three@0.169.0/build/three.module.js",\n'
            '  "three/addons/": "https://cdn.jsdelivr.net/npm/three@0.169.0/examples/jsm/",\n'
            '  "crelt": "https://cdn.jsdelivr.net/npm/crelt@1.0.6/index.js"\n'
            '} }\n</script>\n<body>x</body>\n')

    def setUp(self):
        import tempfile
        self.tmp = tempfile.TemporaryDirectory(prefix="tacob-vendor-")
        self.vendor = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def add(self, rel, body=b"//\n"):
        path = self.vendor / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)

    def imports(self, text):
        import json
        import re
        block = re.search(r'<script type="importmap">(.*?)</script>', text, re.S)
        return json.loads(block.group(1))["imports"]

    def test_nothing_is_rewritten_without_a_vendor_directory(self):
        out = tacob.rewrite_import_map(self.PAGE, self.vendor / "absent")
        self.assertEqual(out, self.PAGE)

    def test_a_file_entry_is_rewritten_only_when_the_file_is_there(self):
        self.add("npm/crelt@1.0.6/index.js")
        imports = self.imports(tacob.rewrite_import_map(self.PAGE, self.vendor))
        self.assertEqual(imports["crelt"], "/vendor/npm/crelt@1.0.6/index.js")
        # not fetched: keep the CDN URL, so a half-vendored tree still loads online
        self.assertTrue(imports["three"].startswith("https://cdn.jsdelivr.net/"))

    def test_a_prefix_entry_needs_a_directory_and_keeps_its_slash(self):
        # `three/addons/` is a prefix mapping — the map resolves
        # `three/addons/loaders/GLTFLoader.js` through it — so it must rewrite to
        # a path that still ends in a slash, and only when the tree is there.
        imports = self.imports(tacob.rewrite_import_map(self.PAGE, self.vendor))
        self.assertTrue(imports["three/addons/"].startswith("https://"))
        self.add("npm/three@0.169.0/examples/jsm/loaders/GLTFLoader.js")
        imports = self.imports(tacob.rewrite_import_map(self.PAGE, self.vendor))
        self.assertEqual(imports["three/addons/"],
                         "/vendor/npm/three@0.169.0/examples/jsm/")

    def test_the_page_that_ships_has_every_entry_on_the_one_cdn(self):
        # `tacob-build.py vendor` only knows jsdelivr, and mirrors its paths; an
        # entry from anywhere else would be fetched by nobody and silently stay
        # a CDN URL in the packaged folder.
        imports = self.imports(tacob.EDITOR_HTML.read_text(encoding="utf-8"))
        for specifier, url in imports.items():
            self.assertTrue(url.startswith(tacob.CDN), f"{specifier} -> {url}")


class Launcher(unittest.TestCase):
    """The entry point's one decision: which subcommand an empty command line means."""

    def test_no_arguments_means_the_window(self):
        self.assertEqual(tacob_app.command_line([]), ["gui"])
        self.assertEqual(tacob_app.command_line(["--no-open"]), ["gui", "--no-open"])
        self.assertEqual(tacob_app.command_line(["armpw"]), ["armpw"])
        self.assertEqual(tacob_app.command_line(["roundtrip", "--all"]),
                         ["roundtrip", "--all"])
        for flag in ("-h", "--help"):
            self.assertEqual(tacob_app.command_line([flag]), [flag])


class FirstRun(unittest.TestCase):
    """The server before there is a session: the packaged tool's first run, where
    nothing is known yet about where the game is installed."""

    def setUp(self):
        import os
        import tempfile
        self.tmp = tempfile.TemporaryDirectory(prefix="tacob-first-")
        self.old = os.environ.get("TACOB_HOME")
        os.environ["TACOB_HOME"] = self.tmp.name
        self.server = tacob.Server(None, port=0)
        self.server.start()

    def tearDown(self):
        import os
        self.server.close()
        if self.old is None:
            os.environ.pop("TACOB_HOME", None)
        else:
            os.environ["TACOB_HOME"] = self.old
        self.tmp.cleanup()

    def fetch(self, path, body=None):
        import json
        import urllib.error
        import urllib.request
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.server.base + path, data=data,
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=5) as res:
                return res.status, res.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as err:
            return err.code, err.read().decode("utf-8", "replace")

    def test_the_page_at_the_root_is_the_picker(self):
        code, body = self.fetch("/")
        self.assertEqual(code, 200)
        self.assertIn("Where Total Annihilation is installed", body)
        self.assertNotIn('id="threads"', body)

    def test_the_picker_says_what_it_knows_and_where_it_will_write(self):
        import json
        code, body = self.fetch("/setup")
        state = json.loads(body)
        self.assertEqual(code, 200)
        self.assertTrue(state["needed"])
        self.assertIsNone(state["open"])
        self.assertTrue(state["config"].endswith("config.json"))
        self.assertIn("projects", state["projects"])

    def test_every_other_route_says_why_it_cannot_answer(self):
        import json
        for path in ("/state", "/pose", "/trace", "/events", "/source"):
            code, body = self.fetch(path)
            self.assertEqual(code, 503, path)
            self.assertIn("no game folder yet", json.loads(body)["error"])
        code, body = self.fetch("/transport", {"action": "step"})
        self.assertEqual(code, 503)

    def test_a_folder_that_is_not_a_game_is_refused_with_a_sentence(self):
        import json
        code, body = self.fetch("/setup", {"gamedir": self.tmp.name, "unit": "armpw"})
        self.assertEqual(code, 400)
        self.assertIn("no Total Annihilation archives", json.loads(body)["error"])
        code, body = self.fetch("/setup", {"gamedir": "", "unit": "armpw"})
        self.assertEqual(code, 400)
        self.assertIn("give the folder", json.loads(body)["error"])
        # nothing was saved, and nothing is running
        self.assertEqual(tacob.read_config(), {})
        self.assertIsNone(self.server.session)


if __name__ == "__main__":
    unittest.main()

