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


if __name__ == "__main__":
    unittest.main()
