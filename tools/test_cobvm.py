"""The COB VM core, tagpu_cobvm.c, and the in-game wiki's host over it, tagpu_wikicob.c, compiled
natively under UBSan and driven through ctypes.

The nine cobtrace fixtures run through this build as they do through `tools/tacob run --all`;
the rest pins what the fixtures do not reach: Impure's rules beside the retail engine's, the
effect log of a host without a commit, refusals, the shared getters (tagpu_cob_getter), the
engine layout, and that two VMs run at once on two threads. For the host (WikiHost): no
writable static, two hosts on two threads, the quarantine of a type with no script and of one
whose script faults, and ACTIVATION's state setter. research/notes/ingame-wiki-cob-plan.md is
the design."""
import ctypes as C
import importlib.machinery
import os
import sys
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
DDRAW = ROOT / "tagpu" / "ddraw"


def load_tacob():
    loader = importlib.machinery.SourceFileLoader("tacob", str(ROOT / "tools" / "tacob"))
    spec = importlib.util.spec_from_loader("tacob", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


tacob = load_tacob()

# UBSan in-process; the AddressSanitizer pass re-runs this file in a child whose interpreter
# preloads the ASan runtime (a shared library cannot bring its own)
SANITIZE = os.environ.get("COBVM_SANITIZE", "undefined")

PRELUDE = """
piece base, arm;
static-var s1, s2;
"""


class Core(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="cobvm-")
        lib = Path(cls.temp.name) / "libcobvm.so"
        # TAGPU_COBVM_PROVE_CHECK: every opcode the stack proof lets through unchecked is checked
        # anyway, and a check that would not have executed is a fault by name
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-g",
                        "-fsanitize=" + SANITIZE, "-fno-sanitize-recover=all", "-DTAGPU_COBVM_PROVE_CHECK",
                        "-I", str(DDRAW / "inc"),
                        str(DDRAW / "src" / "tagpu_cobvm.c"), str(DDRAW / "src" / "tagpu_cob_safe.c"),
                        "-o", str(lib)], check=True)
        cls.lib = tacob.bind_cobvm(C.CDLL(str(lib)))
        cls.saved = tacob._LIB
        tacob._LIB = cls.lib
        cls.symbols = ""
        for name in ("tagpu_cobvm", "tagpu_cob_safe"):        # the core and the rules it runs
            obj = Path(cls.temp.name) / f"{name}.o"
            subprocess.run(["cc", "-std=c99", "-O2", "-c", "-I", str(DDRAW / "inc"),
                            str(DDRAW / "src" / f"{name}.c"), "-o", str(obj)], check=True)
            cls.symbols += subprocess.run(["nm", str(obj)], check=True, capture_output=True,
                                          text=True).stdout

    @classmethod
    def tearDownClass(cls):
        tacob._LIB = cls.saved
        cls.temp.cleanup()

    # unit 1, owner 0, a finished veteran of three kills, on a peer where seat 0 is local
    FACTS = {"slots": 100, "caller": 1, "units": {1: [0, 3, 0.0], 2: [1, 0, 0.5]},
             "allied": [[0, 0]], "local": [0]}

    def vm(self, source, rules=tacob.RULES_RETAIL, world=None, trace=None):
        cob = tacob.compile_bos(PRELUDE + source, "test.bos")
        vm = tacob.CobVM(cob, world or tacob.UnitWorld(facts=self.FACTS), trace, unit=1,
                         type_name="T", rules=rules)
        vm.tick = 1
        return vm

    # -- the fixtures, through the sanitizer build ---------------------------
    def test_every_fixture_is_byte_exact(self):
        for cls in ("kbot", "tank", "building", "death", "fighter", "gunship", "bomber", "ship", "sub"):
            with self.subTest(fixture=cls):
                try:
                    report, _, _ = tacob.run_class(cls)
                except tacob.TacobError as e:
                    self.skipTest(f"the fixtures need the game's archives: {e}")
                self.assertTrue(report["trace_ok"], report["trace_diff"])
                self.assertTrue(report.get("pose_ok", True), report.get("pose_bad"))

    def test_every_fixture_runs_under_the_stack_proof(self):
        # the fixtures are retail's and replay under RETAIL, which checks every opcode; under
        # IMPURE's rules the proof skips checks, and the check build faults by name if one of
        # them would not have executed
        for cls in ("kbot", "tank", "building", "death", "fighter", "gunship", "bomber", "ship", "sub"):
            with self.subTest(fixture=cls):
                folder = tacob.fixtures_dir() / cls
                try:
                    header, lines = tacob.read_cobtrace(folder / "cobtrace.log")
                    type_name = lines[0].fields[3]
                    blob, _ = tacob.read_cob_source(type_name.lower())
                except (tacob.TacobError, OSError) as e:
                    self.skipTest(f"the fixtures need the game's archives: {e}")
                cob = tacob.read_cob(blob)
                timelines = {int(k): [tuple(p) for p in v]
                             for k, v in (dict(tacob.replay_inputs(cls)).get("get") or {}).items()}
                try:
                    tacob.replay(cob, lines, timelines, type_name, lines[0].unit, rules=tacob.RULES_IMPURE,
                                 facts=self.FACTS)
                except tacob.TacobError as e:
                    # the check build names a skipped check that would not have executed, whatever
                    # the check's own reason; any other refusal is the fixture's under IMPURE
                    self.assertNotIn("stack proof", str(e))

    # -- the stack proof ----------------------------------------------------------
    class Program(C.Structure):
        _fields_ = [("code", C.c_void_p), ("entries", C.c_void_p), ("names", C.c_void_p),
                    ("starts", C.c_void_p), ("words", C.c_uint32), ("scripts", C.c_uint32),
                    ("pieces", C.c_uint32), ("statics", C.c_uint32), ("model_pieces", C.c_uint32),
                    ("terminal_zero", C.c_int), ("lo", C.POINTER(C.c_ubyte)), ("hi", C.POINTER(C.c_ubyte)),
                    ("guard", C.POINTER(C.c_ubyte)), ("depth", C.c_uint32)]

    def program(self, vm):
        return C.cast(vm._program, C.POINTER(self.Program)).contents

    def patched(self, source, old, new):
        """`source` compiled, with the first run of words `old` replaced by `new` (as long)"""
        cob = tacob.compile_bos(PRELUDE + source, "test.bos")
        code = cob.code
        at = next(i for i in range(len(code)) if code[i:i + len(old)] == list(old))
        code[at:at + len(old)] = list(new)
        return cob

    def test_a_balanced_script_is_bounded_and_runs_unchecked(self):
        vm = self.vm("Create() { s1 = 1 + (2 + (3 + (4 + 5))); }", rules=tacob.RULES_IMPURE)
        p = self.program(vm)
        self.assertTrue(p.guard)
        # four words an engine start may leave, then five constants before the first add
        self.assertEqual(p.depth, 9)
        entry = vm.entries[vm.names.index("Create")]
        self.assertEqual((p.lo[entry], p.hi[entry], p.guard[entry]), (0, 4, 0))
        vm.start_script("Create", run_now=True)
        self.assertEqual(vm.statics[0], 15)

    def test_an_instruction_that_can_underflow_stays_checked(self):
        # `s1 = 7` with its push turned into a jump to the next word: the pop then takes the
        # start's own word, so an engine start with none underflows, as it did before the proof
        cob = self.patched("Create() { s1 = 7; }", (0x10021001, 7), (0x10064000, 0))
        cob.code[cob.code.index(0x10064000) + 1] = cob.code.index(0x10064000) + 2
        for argwords, args, want in ((False, (), "underflow"), (True, (3,), None)):
            with self.subTest(argwords=argwords):
                vm = tacob.CobVM(cob, tacob.UnitWorld(facts=self.FACTS), unit=1, type_name="T",
                                 rules=tacob.RULES_IMPURE)
                vm.tick = 1
                p = self.program(vm)
                pop = cob.code.index(0x10064000) + 2
                self.assertEqual((p.lo[pop], p.guard[pop]), (0, 1))
                if want:
                    with self.assertRaises(tacob.TacobError) as e:
                        vm.start_script("Create", run_now=True)
                    self.assertIn(want, str(e.exception))
                else:
                    vm.start_script("Create", run_now=True, args=args, argwords=True)
                    self.assertEqual(vm.statics[0], 3)

    def test_the_check_build_names_a_skipped_check_that_would_refuse(self):
        # a guard cleared by hand where the pop can underflow stands in for a hole in the proof
        cob = self.patched("Create() { s1 = 7; }", (0x10021001, 7), (0x10064000, 0))
        cob.code[cob.code.index(0x10064000) + 1] = cob.code.index(0x10064000) + 2
        vm = tacob.CobVM(cob, tacob.UnitWorld(facts=self.FACTS), unit=1, type_name="T",
                         rules=tacob.RULES_IMPURE)
        vm.tick = 1
        self.program(vm).guard[cob.code.index(0x10064000) + 2] = 0
        with self.assertRaises(tacob.TacobError) as e:
            vm.start_script("Create", run_now=True)
        self.assertIn("stack proof skipped a check", str(e.exception))
        self.assertIn("underflow", str(e.exception))

    def test_a_loop_that_leaves_words_is_unbounded_and_faults_at_the_bound(self):
        # each pass's `s1 = 1` becomes two pushes: the proof cannot bound it, so every opcode is
        # checked and the 128-word bound faults the thread as before
        cob = self.patched("Create() { while(1) { s1 = 1; sleep 1; } }", (0x10023004, 0), (0x10021001, 1))
        vm = tacob.CobVM(cob, tacob.UnitWorld(facts=self.FACTS), unit=1, type_name="T",
                         rules=tacob.RULES_IMPURE)
        vm.tick = 1
        p = self.program(vm)
        self.assertFalse(p.guard)
        self.assertEqual(p.depth, 128)
        with self.assertRaises(tacob.TacobError) as e:
            vm.start_script("Create", run_now=True)
            for _ in range(200):
                vm.do_scripts_now(1)
        self.assertIn("capacity", str(e.exception))

    def test_a_record_outside_the_proof_turns_every_check_back_on(self):
        vm = self.vm("Create() { while(1) { sleep 1; } }", rules=tacob.RULES_IMPURE)
        vm.start_script("Create", run_now=True)
        self.assertEqual(vm._vm.checked, 0)
        self.lib.tagpu_cobvm_loaded.argtypes = [C.POINTER(tacob._Vm)]
        self.lib.tagpu_cobvm_loaded(C.byref(vm._vm))
        self.assertEqual(vm._vm.checked, 0)                  # the VM's own record is covered
        record = 28                                          # the first record's sp, as a save could set it
        struct.pack_into("<i", vm._object, record + 8, 40)
        self.lib.tagpu_cobvm_loaded(C.byref(vm._vm))
        self.assertEqual(vm._vm.checked, 1)

    def test_retail_checks_every_opcode(self):
        vm = self.vm("Create() { s1 = 1; }")
        self.assertEqual(vm._vm.checked, 1)

    # -- a turn only when something is due --------------------------------------
    SCHEDULE = """
    Walk() { while(1) { move arm to y-axis [5] speed [10]; sleep 500; turn base to y-axis <90> speed <45>; sleep 200; s1 = s1 + 1; } }
    Waiter() { turn arm to x-axis <30> speed <60>; wait-for-turn arm around x-axis; sleep 100; turn arm to x-axis <0> speed <60>; wait-for-turn arm around x-axis; }
    Child() { sleep 300; return(5); }
    Parent() { while(1) { call-script Child(); sleep 50; } }
    Sig() { set-signal-mask 2; sleep 1000; s2 = s2 + 100; }
    Killer() { while(1) { sleep 700; signal 2; start-script Sig(); } }
    Lazy() { sleep 2000; s2 = s2 + 1; }
    Create() { start-script Walk(); start-script Parent(); start-script Killer(); start-script Lazy(); }
    """

    def _scheduled_against_every_turn(self, source, ticks, entry_at=()):
        """(turns the schedule left unvisited): one VM scheduled, one made to visit its records on
        every turn, both given the same turns and the same entries, equal after each turn once the
        first's owed dt is taken off"""
        a = self.vm(source, rules=tacob.RULES_IMPURE)
        b = self.vm(source, rules=tacob.RULES_IMPURE)
        for v in (a, b):
            v.start_script("Create", run_now=True)
        skipped = 0
        for t in range(ticks):
            if t in entry_at:
                for v in (a, b):
                    v.start_script("Waiter", run_now=False)
            b._vm.quiet = 0
            skipped += bool(a._vm.quiet) and a._vm.owed + 1 < a._vm.due
            a.do_scripts_now(1)
            b.do_scripts_now(1)
            self.lib.tagpu_cobvm_settle(C.byref(a._vm))
            self.assertEqual(bytes(a._object), bytes(b._object), f"the records after turn {t}")
            self.assertEqual(a.statics, b.statics, f"the statics after turn {t}")
            self.assertEqual(list(a.anim), list(b.anim), f"the animation after turn {t}")
            self.assertEqual(bytes(a._model), bytes(b._model), f"the model after turn {t}")
        return skipped

    def test_a_turn_with_nothing_due_is_owed_and_exact(self):
        skipped = self._scheduled_against_every_turn(self.SCHEDULE, 900, entry_at=(37, 400, 401, 655))
        self.assertGreater(skipped, 100)

    def test_an_object_only_animating_steps_its_pieces_and_nothing_else(self):
        skipped = self._scheduled_against_every_turn(
            "Create() { turn base to y-axis <180> speed <10>; move arm to x-axis [20] speed [1]; }", 300)
        self.assertGreater(skipped, 200)

    def test_a_sleep_of_zero_wakes_the_next_turn(self):
        self._scheduled_against_every_turn("Create() { while(1) { sleep 0; s1 = s1 + 1; } }", 50)

    # -- the rules of the two builds ------------------------------------------
    POOL = """
    sleeper() { while(1) { sleep 1000; } }
    child(a, b) { s1 = a; s2 = b; }
    Create()
    {
        start-script sleeper(); start-script sleeper(); start-script sleeper();
        start-script sleeper(); start-script sleeper(); start-script sleeper();
        start-script sleeper(); start-script sleeper(); start-script sleeper();
        start-script child(5, 7);
    }
    """

    def test_retail_has_eight_records_and_keeps_a_refused_starts_words(self):
        vm = self.vm(self.POOL)
        vm.start_script("Create", run_now=True)
        self.assertEqual(vm.running, 7)                      # seven sleepers: Create has returned
        self.assertEqual(vm.statics, [0, 0])                 # the child was refused

    def test_impure_has_ten_records(self):
        vm = self.vm(self.POOL, rules=tacob.RULES_IMPURE)
        vm.start_script("Create", run_now=True)
        self.assertEqual(vm.running, 9)                      # nine sleepers: Create has returned
        self.assertEqual(vm.statics, [0, 0])                 # the child was refused

    def test_a_refused_start_keeps_or_drops_its_words_by_build(self):
        source = """
        sleeper() { while(1) { sleep 1000; } }
        child(a) { s1 = a; }
        Create()
        {
            start-script sleeper(); start-script sleeper(); start-script sleeper();
            start-script sleeper(); start-script sleeper(); start-script sleeper();
            start-script sleeper(); start-script sleeper(); start-script sleeper();
            start-script child(4);
            sleep 1000;
        }
        """
        for rules, sp in ((tacob.RULES_RETAIL, 0), (tacob.RULES_IMPURE, -1)):
            with self.subTest(rules=rules):
                vm = self.vm(source, rules=rules)
                vm.start_script("Create", run_now=True)
                create = vm.threads[0]
                self.assertEqual(create.status, tacob.ST_SLEEP)
                self.assertEqual(create.sp, sp)

    def test_impure_clears_a_record_it_hands_out(self):
        # CREATE_LOCAL_VAR writes nothing: retail reads what the record's last thread left
        source = """
        first() { var x; x = 41; }
        second() { var y; s1 = y; }
        Create() { call-script first(); call-script second(); }
        """
        for rules, seen in ((tacob.RULES_RETAIL, 41), (tacob.RULES_IMPURE, 0)):
            with self.subTest(rules=rules):
                vm = self.vm(source, rules=rules)
                vm.start_script("Create", run_now=True)
                vm.do_scripts_now(1)                         # a caller resumes on the next sweep
                self.assertEqual(vm.statics[0], seen)

    # -- refusals ---------------------------------------------------------------
    def test_division_by_zero_is_a_refusal_and_the_vm_stays_refused(self):
        vm = self.vm("Create() { s1 = 1 / s2; }")
        with self.assertRaises(tacob.TacobError) as e:
            vm.start_script("Create", run_now=True)
        self.assertIn("division", str(e.exception))
        with self.assertRaises(tacob.TacobError):
            vm.do_scripts_now(1)

    def test_a_loop_that_never_blocks_is_refused(self):
        vm = self.vm("Create() { while(1) { s1 = s1 + 1; } }")
        with self.assertRaises(tacob.TacobError) as e:
            vm.start_script("Create", run_now=True)
        self.assertIn("step budget", str(e.exception))

    # -- the effect log -----------------------------------------------------------
    def test_without_a_commit_writes_fill_the_log_and_then_refuse(self):
        cob = tacob.compile_bos(PRELUDE + "Create() { set ACTIVATION to 1; emit-sfx 1 from arm; set BUSY to 0; }",
                                "t.bos", include_dirs=[tacob.INCLUDE_DIR])
        lib = self.lib
        blob = tacob.write_cob(cob)
        buf = C.create_string_buffer(blob, len(blob))
        reason = C.create_string_buffer(256)
        program = lib.tagpu_cob_program(buf, len(blob), reason, 256)
        self.assertTrue(program, reason.value)
        try:
            self.assertTrue(lib.tagpu_cob_bind_model(program, len(cob.pieces), reason, 256))
            obj = (C.c_ubyte * tacob.OBJECT_BYTES)()
            statics = (C.c_int32 * 2)()
            anim = (C.c_int32 * (19 * len(cob.pieces)))()
            model = (C.c_ubyte * (tacob.PRIM_BASE + tacob.PRIM_BYTES * len(cob.pieces)))()
            vm = tacob._Vm()
            self.assertTrue(lib.tagpu_cobvm_init(C.byref(vm), program, tacob.RULES_IMPURE, obj, statics,
                                                 anim, model, reason, 256))
            host = tacob._Host()
            world = tacob._World(None, 100, 0, 0, tacob._W_UNIT(lambda c, i, o: 0),
                                 tacob._W_CALLER(lambda c, o: 0), tacob._W_ALLIED(lambda c, a, b: 0),
                                 tacob._W_LOCAL(lambda c, o: 0))
            host.world = C.pointer(world)
            log = (tacob._Effect * 2)()
            run = tacob._Run()
            run.host = C.pointer(host)
            run.log = log
            run.cap = 2
            words = (C.c_int32 * 4)()
            got = lib.tagpu_cobvm_start(C.byref(vm), C.byref(run), lib.tagpu_cobvm_index(program, b"Create"),
                                        0, 1, 0, 0, words)
            self.assertEqual(got, -1)
            self.assertIn(b"effect log is full", run.reason)
            self.assertEqual(run.n, 2)
            self.assertEqual((log[0].op, log[0].v[0], log[0].v[1]), (0x10082000, 1, 1))
            self.assertEqual((log[1].op, log[1].piece, log[1].v[0]), (0x1000F000, 1, 1))
        finally:
            lib.tagpu_cob_program_free(program)

    def test_a_set_is_committed_before_the_next_opcode(self):
        class World(tacob.UnitWorld):
            pass
        world = World()
        vm = self.vm("Create() { set ACTIVATION to 1; s1 = get ACTIVATION; }", world=world)
        vm.start_script("Create", run_now=True)
        self.assertEqual(vm.statics[0], 1)

    # -- the shared getters ----------------------------------------------------------
    def test_the_getters(self):
        lib = self.lib

        class Facts(C.Structure):
            _fields_ = [("id", C.c_uint32), ("owner", C.c_uint32), ("kills", C.c_uint32), ("remaining", C.c_float)]
        UNIT = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_int32, C.POINTER(Facts))
        CALLER = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(Facts))
        PAIR = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32, C.c_uint32)
        ONE = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32)

        class World(C.Structure):
            _fields_ = [("ctx", C.c_void_p), ("slots", C.c_uint32), ("script_pieces", C.c_uint32),
                        ("model_pieces", C.c_uint32), ("unit", UNIT), ("caller", CALLER),
                        ("allied", PAIR), ("local", ONE)]
        units = {3: (3, 1, 0, 0.5), 7: (7, 2, 4, 0.0)}

        def unit(ctx, i, out):
            if i not in units:
                return 0
            out[0].id, out[0].owner, out[0].kills, out[0].remaining = units[i]
            return 1

        def caller(ctx, out):
            return unit(ctx, 7, out)
        w = World(None, 100, 4, 3, UNIT(unit), CALLER(caller),
                  PAIR(lambda c, a, b: int(a == b or {a, b} == {1, 2})), ONE(lambda c, o: int(o == 2)))
        lib.tagpu_cob_getter.argtypes = [C.POINTER(World), C.c_int32, C.c_int32, C.POINTER(C.c_int32)]

        def get(key, a=0):
            v = C.c_int32(12345)
            return v.value if lib.tagpu_cob_getter(C.byref(w), key, a, C.byref(v)) else None
        self.assertEqual(get(1), None)                       # a stock id: the stock handler's
        self.assertEqual(get(69), 1)
        self.assertEqual(get(70), 99)
        self.assertEqual(get(71), 7)
        self.assertEqual(get(32), 400)
        self.assertEqual(get(72, 3), 1)
        self.assertEqual(get(72, 4), -1)
        self.assertEqual(get(73, 3), lib.tagpu_cob_build_left(C.c_float(0.5)))
        self.assertEqual(get(73, 7), 0)
        self.assertEqual(get(73, 9), -1)
        self.assertEqual(get(74, 3), 1)
        self.assertEqual(get(75, 3), 0)
        self.assertEqual(get(75, 7), 1)
        self.assertEqual(get(9, 5), 0)                       # a dead unit's stock getter
        self.assertEqual(get(9, 3), None)
        self.assertEqual(get(7, 3), 0)                       # past the model's pieces
        self.assertEqual(get(7, 2), None)

    def test_under_impure_the_core_answers_the_extended_ids_itself(self):
        class World(tacob.UnitWorld):
            def get(self, vid, a, b, c, d):         # a host that would answer every id its own way
                return 777
        source = ("Create() { g1 = get 71; g2 = get 69; g3 = get 32; g4 = get 72(2, 0, 0, 0); "
                  "g5 = get 73(2, 0, 0, 0); g6 = get 74(1, 0, 0, 0); g7 = get 75(2, 0, 0, 0); "
                  "g8 = get HEALTH; }")
        statics = "static-var g1, g2, g3, g4, g5, g6, g7, g8;\n"
        impure = self.vm(statics + source, rules=tacob.RULES_IMPURE, world=World(facts=self.FACTS))
        impure.start_script("Create", run_now=True)
        self.assertEqual(impure.statics[2:10], [1, 1, 300, 1, self.lib.tagpu_cob_build_left(C.c_float(0.5)),
                                                1, 0, 777])
        retail = self.vm(statics + source, world=World())
        retail.start_script("Create", run_now=True)
        self.assertEqual(retail.statics[2:10], [777] * 8)

    def test_impure_needs_the_worlds_facts(self):
        with self.assertRaises(tacob.TacobError):
            self.vm("Create() { }", rules=tacob.RULES_IMPURE, world=tacob.UnitWorld())

    def test_too_many_start_words_is_the_callers_error_not_a_fault(self):
        vm = self.vm("Go(a) { s1 = a; }")
        words = (C.c_int32 * 4)(5, 0, 0, 0)
        script = vm.index_of("Go")
        self.assertEqual(self.lib.tagpu_cobvm_start(C.byref(vm._vm), C.byref(vm._run), script, 0, 1, 1, 5, words), -1)
        self.assertEqual(self.lib.tagpu_cobvm_start(C.byref(vm._vm), C.byref(vm._run), script, 0, 1, 1, 1, words), 1)
        self.assertEqual(vm.statics[0], 5)

    # -- the layout and the threads ------------------------------------------------------
    def test_the_layout_is_impures_object(self):
        self.assertEqual(tacob.OBJECT_BYTES, 0x158C)
        self.assertEqual(tacob.COUNT_OFFSET, 0x1584)
        self.assertEqual(C.sizeof(tacob._Record), tacob.RECORD_BYTES)

    def test_the_core_has_no_writable_static(self):
        # the feature's descriptor and its registry pointer are const, in the registry's own section,
        # which the native toolchain places in writable data (tagpu_simreg.h, SIM_FEATURE)
        written = [ln for ln in self.symbols.splitlines() if ln.split()[-2:-1] and ln.split()[-2] in "BbDdGgSsVv"
                   and not ln.split()[-1].startswith(("simfeat_", "simfeatreg_"))]
        self.assertEqual(written, [])

    def test_two_vms_on_two_threads(self):
        source = """
        turner() { while(1) { turn arm to y-axis <90> speed <30>; wait-for-turn arm around y-axis;
                            turn arm to y-axis <0> speed <45>; wait-for-turn arm around y-axis; } }
        Create() { start-script turner(); }
        """

        def play(out, index):
            vm = self.vm(source)
            vm.start_script("Create", run_now=True)
            poses = []
            for tick in range(2, 3000):
                vm.tick = tick
                vm.do_scripts_now(1)
                poses.append(tuple(vm.ang[1]))
            out[index] = poses
        alone = [None]
        play(alone, 0)
        both = [None, None]
        threads = [threading.Thread(target=play, args=(both, i)) for i in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(both[0], alone[0])
        self.assertEqual(both[1], alone[0])


def load_serve():
    loader = importlib.machinery.SourceFileLoader("wikiserve", str(ROOT / "tools" / "wiki" / "serve.py"))
    spec = importlib.util.spec_from_loader("wikiserve", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class WikiHost(unittest.TestCase):
    """tagpu_wikicob.h's host, bound as tools/wiki/serve.py binds it, on its own sanitizer build."""
    SOURCES = ("tagpu_wikicob", "tagpu_cobvm", "tagpu_cob_safe")
    INBUILDSTANCE, ACTIVATION, BUGGER_OFF = 5, 1, 19

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="wikicob-")
        lib = Path(cls.temp.name) / "libwikicob.so"
        inc = ["-I", str(DDRAW / "inc"), "-I", str(DDRAW / "src")]
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-g",
                        "-fsanitize=" + SANITIZE, "-fno-sanitize-recover=all", *inc,
                        *[str(DDRAW / "src" / f"{n}.c") for n in cls.SOURCES], "-lm", "-o", str(lib)], check=True)
        cls.serve = load_serve()
        cls.lib = cls.serve.bind(C.CDLL(str(lib)))
        obj = Path(cls.temp.name) / "tagpu_wikicob.o"
        subprocess.run(["cc", "-std=c99", "-O2", "-c", *inc, str(DDRAW / "src" / "tagpu_wikicob.c"), "-o", str(obj)],
                       check=True)
        cls.symbols = subprocess.run(["nm", str(obj)], check=True, capture_output=True, text=True).stdout

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def host(self, sources, shape=None):
        """A host for one type per source (None: no script), its handle freed with the test; `shape`
        (offsets, parents, top) the model's shape of every type, none without it."""
        keep, arr = [], (self.serve.Type * len(sources))()
        for i, src in enumerate(sources):
            if src is None:
                continue
            cob = tacob.write_cob(tacob.compile_bos(PRELUDE + src, "test.bos"))
            vis = bytes([1, 1])
            keep += [cob, vis]
            arr[i].cob, arr[i].ncob = C.cast(C.c_char_p(cob), C.c_void_p), len(cob)
            arr[i].pieces, arr[i].visible = 2, C.cast(C.c_char_p(vis), C.c_void_p)
            if shape:
                off = struct.pack("<6i", *[w for o in shape[0] for w in o])
                par = struct.pack("<2h", *shape[1])
                keep += [off, par]
                arr[i].offset, arr[i].parent = C.cast(C.c_char_p(off), C.c_void_p), C.cast(C.c_char_p(par), C.c_void_p)
                arr[i].top = shape[2]
        err = C.create_string_buffer(256)
        h = self.lib.tagpu_wikicob_new(len(sources), arr, 1, err, 256)
        self.assertTrue(h, err.value)
        self.addCleanup(self.lib.tagpu_wikicob_free, h)
        return h

    def facts(self, units):
        arr = (self.serve.Fact * max(1, len(units)))()
        for i, u in enumerate(units):
            arr[i] = self.serve.Fact(u, 0, 0, 0, 0.0, 100)
        return arr, len(units)

    def note(self, h, t):
        return self.lib.tagpu_wikicob_note(h, t).decode("latin-1")

    def test_the_host_has_no_writable_static(self):
        written = [ln for ln in self.symbols.splitlines() if ln.split()[-2:-1] and ln.split()[-2] in "BbDdGgSsVv"]
        self.assertEqual(written, [])

    def test_two_hosts_on_two_threads(self):
        source = """
        Go() { turn arm to y-axis <90> speed <30>; wait-for-turn arm around y-axis; set INBUILDSTANCE to 1; }
        Stop() { set INBUILDSTANCE to 0; turn arm to y-axis <0> speed <45>; wait-for-turn arm around y-axis; }
        StartBuilding(h) { start-script Go(); }
        StopBuilding() { start-script Stop(); }
        """

        def play(out, index):
            h = self.host([source])
            f, n = self.facts([1])
            self.assertEqual(self.lib.tagpu_wikicob_spawn(h, 0, f, n), 1)
            for tick in range(3000):
                if tick % 300 == 0:
                    arg = (C.c_int32 * 1)(0)
                    self.lib.tagpu_wikicob_call(h, 1, b"StartBuilding" if tick % 600 == 0 else b"StopBuilding",
                                                2 if tick % 600 == 0 else 1, 1 if tick % 600 == 0 else 0, arg, f, n)
                self.lib.tagpu_wikicob_slice(h, f, n)
            out[index] = (self.lib.tagpu_wikicob_hash(h, self.serve.FNV_BASIS),
                          self.lib.tagpu_wikicob_value(h, 1, self.INBUILDSTANCE))
        alone = [None]
        play(alone, 0)
        both = [None, None]
        threads = [threading.Thread(target=play, args=(both, i)) for i in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(both, [alone[0], alone[0]])

    def test_a_type_with_no_script_is_quarantined_with_its_gates_open(self):
        h = self.host([None])
        f, n = self.facts([1])
        self.assertEqual(self.lib.tagpu_wikicob_spawn(h, 0, f, n), 1)
        self.assertIn("no script", self.note(h, 0))
        self.assertTrue(self.lib.tagpu_wikicob_resting(h, 1))
        self.assertEqual(self.lib.tagpu_wikicob_value(h, 1, self.INBUILDSTANCE), 1)
        self.assertEqual(self.lib.tagpu_wikicob_value(h, 1, self.ACTIVATION), 1)

    def test_a_faulting_script_quarantines_its_type_and_no_other(self):
        h = self.host(["Create() { var z; z = 0; z = 1 / z; }", "Create() { set INBUILDSTANCE to 0; }"])
        f, n = self.facts([1, 2])
        self.assertEqual(self.lib.tagpu_wikicob_spawn(h, 0, f, n), 1)
        self.assertEqual(self.lib.tagpu_wikicob_spawn(h, 1, f, n), 2)
        self.lib.tagpu_wikicob_slice(h, f, n)
        self.assertNotEqual(self.note(h, 0), "")
        self.assertEqual(self.note(h, 1), "")
        self.assertEqual(self.lib.tagpu_wikicob_value(h, 1, self.INBUILDSTANCE), 1)
        self.assertEqual(self.lib.tagpu_wikicob_value(h, 2, self.INBUILDSTANCE), 0)

    def test_activation_runs_activate_once_a_change(self):
        # the flags a script SETs read back as 0 or 1: a second Activate would clear INBUILDSTANCE
        source = """
        static-var n;
        Create() { n = 0; }
        Activate() { n = n + 1; if (n == 1) { set INBUILDSTANCE to 1; } else { set INBUILDSTANCE to 0; } }
        Deactivate() { set BUGGER_OFF to 1; }
        """
        h = self.host([source])
        f, n = self.facts([1])
        self.lib.tagpu_wikicob_spawn(h, 0, f, n)
        self.lib.tagpu_wikicob_activate(h, 1, 1)
        self.lib.tagpu_wikicob_activate(h, 1, 1)      # unchanged: no second Activate (0x48B090)
        self.lib.tagpu_wikicob_slice(h, f, n)
        self.assertEqual(self.lib.tagpu_wikicob_value(h, 1, self.ACTIVATION), 1)
        self.assertEqual(self.lib.tagpu_wikicob_value(h, 1, self.INBUILDSTANCE), 1)
        self.assertEqual(self.lib.tagpu_wikicob_value(h, 1, self.BUGGER_OFF), 0)
        self.lib.tagpu_wikicob_activate(h, 1, 0)
        self.lib.tagpu_wikicob_slice(h, f, n)
        self.assertEqual(self.lib.tagpu_wikicob_value(h, 1, self.ACTIVATION), 0)
        self.assertEqual(self.lib.tagpu_wikicob_value(h, 1, self.BUGGER_OFF), 1)

    def origin(self, h, unit, piece):
        turned, base = (C.c_int32 * 3)(), (C.c_int32 * 3)()
        r = self.lib.tagpu_wikicob_origin(h, unit, piece, turned, base)
        return r, list(turned), list(base)

    def test_query_nano_piece_answers_its_piece_and_a_missing_one_leaves_the_preset(self):
        h = self.host(["QueryNanoPiece(p) { p = arm; }", "Create() { }"])
        f, n = self.facts([1, 2])
        self.lib.tagpu_wikicob_spawn(h, 0, f, n)
        self.lib.tagpu_wikicob_spawn(h, 1, f, n)
        io = (C.c_int32 * 4)(7, 0, 0, 0)
        self.assertEqual(self.lib.tagpu_wikicob_query(h, 1, b"QueryNanoPiece", io, f, n), 1)
        self.assertEqual(io[0], 1)
        io = (C.c_int32 * 4)(7, 0, 0, 0)
        self.assertEqual(self.lib.tagpu_wikicob_query(h, 2, b"QueryNanoPiece", io, f, n), 0)
        self.assertEqual(io[0], 7)

    def test_a_pieces_origin_is_carried_up_its_parent_turned(self):
        # arm, a child of the root base: its offset turned by base's TURN (0x43DEF0), base's own
        # offset and MOVE apart; a quarter turn about y is exact, so its words are known
        shape = ([(2 << 16, 1 << 16, 0), (10 << 16, 3 << 16, 0)], (-1, 0), 40 << 16)
        h = self.host(["Create() { }", "Create() { turn base to y-axis <90> now; }"], shape)
        f, n = self.facts([1, 2])
        self.lib.tagpu_wikicob_spawn(h, 0, f, n)
        self.lib.tagpu_wikicob_spawn(h, 1, f, n)
        self.assertEqual(self.origin(h, 1, 1), (1, [10 << 16, 3 << 16, 0], [2 << 16, 1 << 16, 0]))
        r, turned, base = self.origin(h, 2, 1)
        self.assertEqual((r, turned[0], turned[1], abs(turned[2]), base), (1, 0, 3 << 16, 10 << 16, [2 << 16, 1 << 16, 0]))
        self.assertEqual(self.origin(h, 2, 0), (1, [0, 0, 0], [2 << 16, 1 << 16, 0]))
        self.assertEqual(self.lib.tagpu_wikicob_top(h, 1), 40 << 16)
        self.assertEqual(self.lib.tagpu_wikicob_top(h, 2), 0)

    def test_a_piece_outside_the_model_and_a_broken_parent_chain_answer_the_units_point(self):
        shape = ([(2 << 16, 1 << 16, 0), (10 << 16, 3 << 16, 0)], (1, 0), 0)
        h = self.host(["Create() { }"], shape)
        f, n = self.facts([1])
        self.lib.tagpu_wikicob_spawn(h, 0, f, n)
        self.assertEqual(self.origin(h, 1, 2), (0, [0, 0, 0], [0, 0, 0]))
        self.assertEqual(self.origin(h, 1, -1), (0, [0, 0, 0], [0, 0, 0]))
        # parents that never reach a root: the shape is refused whole, every offset 0
        self.assertEqual(self.origin(h, 1, 1)[1:], ([0, 0, 0], [0, 0, 0]))

    def test_set_keeps_what_the_engine_keeps(self):
        # 0x480B20: INBUILDSTANCE, BUSY and BUGGER_OFF keep bit 0, ARMORED whether nonzero (0x48B090),
        # STANDINGMOVEORDERS nothing (0x480BF1)
        source = """
        Create() { set INBUILDSTANCE to 2; set BUSY to 3; set BUGGER_OFF to 4; set ARMORED to 6;
                   set STANDINGMOVEORDERS to 1; }
        """
        h = self.host([source])
        f, n = self.facts([1])
        self.lib.tagpu_wikicob_spawn(h, 0, f, n)
        self.assertEqual(self.note(h, 0), "")
        got = [self.lib.tagpu_wikicob_value(h, 1, v) for v in (self.INBUILDSTANCE, 6, self.BUGGER_OFF, 20, 2)]
        self.assertEqual(got, [0, 1, 0, 1, 0])


class Asan(unittest.TestCase):
    @unittest.skipIf("COBVM_SANITIZE" in os.environ, "already the sanitizer child")
    def test_the_core_under_address_sanitizer(self):
        runtime = subprocess.run(["cc", "-print-file-name=libasan.so"], capture_output=True, text=True).stdout.strip()
        if not os.path.isabs(runtime):
            self.skipTest("no ASan runtime for this compiler")
        env = dict(os.environ, COBVM_SANITIZE="address,undefined", LD_PRELOAD=runtime,
                   ASAN_OPTIONS="detect_leaks=0")
        child = subprocess.run([sys.executable, "-m", "unittest", "test_cobvm.Core", "test_cobvm.WikiHost"],
                               cwd=Path(__file__).parent,
                               env=env, capture_output=True, text=True)
        self.assertEqual(child.returncode, 0, child.stderr[-4000:])


if __name__ == "__main__":
    unittest.main()
