"""The COB VM core, tagpu_cobvm.c, compiled natively under UBSan and driven through ctypes.

The nine cobtrace fixtures run through this build as they do through `tools/tacob run --all`;
the rest pins what the fixtures do not reach: Impure's rules beside the retail engine's, the
effect log of a host without a commit, refusals, the shared getters (tagpu_cob_getter), the
engine layout, and that two VMs run at once on two threads.
research/notes/ingame-wiki-cob-plan.md is the design."""
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
        obj = Path(cls.temp.name) / "tagpu_cobvm.o"
        subprocess.run(["cc", "-std=c99", "-O2", "-c", "-I", str(DDRAW / "inc"),
                        str(DDRAW / "src" / "tagpu_cobvm.c"), "-o", str(obj)], check=True)
        cls.symbols = subprocess.run(["nm", str(obj)], check=True, capture_output=True, text=True).stdout

    @classmethod
    def tearDownClass(cls):
        tacob._LIB = cls.saved
        cls.temp.cleanup()

    def vm(self, source, rules=tacob.RULES_RETAIL, world=None, trace=None):
        cob = tacob.compile_bos(PRELUDE + source, "test.bos")
        vm = tacob.CobVM(cob, world or tacob.UnitWorld(), trace, unit=1, type_name="T", rules=rules)
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


class Asan(unittest.TestCase):
    @unittest.skipIf("COBVM_SANITIZE" in os.environ, "already the sanitizer child")
    def test_the_core_under_address_sanitizer(self):
        runtime = subprocess.run(["cc", "-print-file-name=libasan.so"], capture_output=True, text=True).stdout.strip()
        if not os.path.isabs(runtime):
            self.skipTest("no ASan runtime for this compiler")
        env = dict(os.environ, COBVM_SANITIZE="address,undefined", LD_PRELOAD=runtime,
                   ASAN_OPTIONS="detect_leaks=0")
        child = subprocess.run([sys.executable, "-m", "unittest", "test_cobvm.Core"], cwd=Path(__file__).parent,
                               env=env, capture_output=True, text=True)
        self.assertEqual(child.returncode, 0, child.stderr[-4000:])


if __name__ == "__main__":
    unittest.main()
