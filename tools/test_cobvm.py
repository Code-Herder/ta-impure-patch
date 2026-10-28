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
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-g",
                        "-fsanitize=" + SANITIZE, "-fno-sanitize-recover=all", "-I", str(DDRAW / "inc"),
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
        written = [ln for ln in self.symbols.splitlines() if ln.split()[-2:-1] and ln.split()[-2] in "BbDdGgSsVv"]
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

    def host(self, sources):
        """A host for one type per source (None: no script), its handle freed with the test."""
        keep, arr = [], (self.serve.Type * len(sources))()
        for i, src in enumerate(sources):
            if src is None:
                continue
            cob = tacob.write_cob(tacob.compile_bos(PRELUDE + src, "test.bos"))
            vis = bytes([1, 1])
            keep += [cob, vis]
            arr[i].cob, arr[i].ncob = C.cast(C.c_char_p(cob), C.c_void_p), len(cob)
            arr[i].pieces, arr[i].visible = 2, C.cast(C.c_char_p(vis), C.c_void_p)
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
