#!/usr/bin/env python3
"""test_diag -- diag.py's arguments and its extraction, offline, against a small fixture log.

    python3 tools/impai/test_diag.py"""
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bar  # noqa: E402
import diag  # noqa: E402

LOG = "\n".join([
    "log: run 20261117-000000-1 part 1 of tagpu.log",
    "impai: seats 1 (impure.cfg) of 3 records",
    "impai: pulse seat=1 t=7 built=1 mroots=0 eroots=0 army=0",
    "units: alive=2",
    "impai: mil seat=1 t=900 attack=0 raid=0 scout=1 army=1",
    "impai: pulse seat=1 t=1807 built=3 mroots=0 eroots=3 army=0",
    "impai: plant seat=1 gametime=1987 want=first chose=CORLAB",
    "impai: mil seat=1 t=2400 attack=1 raid=0 scout=0 army=4",
    "impai: pulse seat=1 t=3607 built=6 mroots=2 eroots=7 army=4",
])


def game(folder: Path, gid: str, log: str, outcome="cap", end=4000, seed=7):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / f"{gid}.json").write_text(json.dumps({"id": gid, "outcome": outcome, "end_tick": end, "seed": seed}))
    (folder / f"{gid}.scenario.json").write_text("{}")
    (folder / f"{gid}.tagpu.log").write_text(log)


def run(argv):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = diag.main(argv)
    return rc, buf.getvalue().splitlines()


class Extraction(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)

    def test_a_line_s_gametime(self):
        self.assertEqual(diag.gametime("impai: mil seat=1 t=900 attack=0"), 900)
        self.assertEqual(diag.gametime("impai: plant seat=1 gametime=1987 want=first"), 1987)
        self.assertIsNone(diag.gametime("impai: seats 1 (impure.cfg) of 3 records"))

    def test_each_pattern_s_lines_in_the_log_s_order_and_only_impai_lines(self):
        m = diag.matched(LOG, ["pulse", "alive"])
        self.assertEqual([diag.gametime(ln) for ln in m["pulse"]], [7, 1807, 3607])
        self.assertEqual(m["alive"], [])

    def test_at_keeps_the_lines_near_its_gametimes(self):
        m = diag.matched(LOG, ["seat=1"], at=[1900], w=100)
        self.assertEqual([diag.gametime(ln) for ln in m["seat=1"]], [1807, 1987])
        # a line with no GameTime is left out under --at
        self.assertEqual(diag.matched(LOG, ["seats"], at=[7]), {"seats": []})

    def test_the_report_caps_each_pattern_and_names_the_rest(self):
        game(self.tmp / "g", "bar-x-1", LOG)
        out = diag.report([self.tmp / "g"], ["pulse"], per=2)
        self.assertEqual(out[0], "== bar-x-1: at the cap, end 4000, seed 7")
        self.assertEqual(len([ln for ln in out if "impai: pulse" in ln]), 2)
        self.assertEqual(out[-1], "   ... 1 more lines of /pulse/")

    def test_the_report_holds_to_its_total_and_says_what_it_cut(self):
        for k in range(10):
            game(self.tmp / "g", f"bar-x-{k}", LOG)
        out = diag.report([self.tmp / "g"], ["seat=1"], most=12)
        self.assertEqual(len(out), 12)
        self.assertRegex(out[-1], r"^diag: \d+ more lines cut at --max 12")

    def test_no_pattern_counts_the_impai_lines(self):
        game(self.tmp / "g", "bar-x-1", LOG)
        self.assertEqual(diag.report([self.tmp / "g"], [])[1], "   7 impai lines (--grep to print some)")

    def test_a_game_that_could_not_run(self):
        game(self.tmp / "g", "bar-x-1", "", outcome="could_not_run")
        self.assertIn("COULD NOT RUN", diag.report([self.tmp / "g"], ["pulse"])[0])

    def test_two_folders_give_the_first_parting(self):
        game(self.tmp / "a" / "games", "bar-x-1", LOG)
        game(self.tmp / "b" / "games", "bar-x-1", LOG.replace("t=2400 attack=1", "t=2400 attack=0"))
        out = diag.report([self.tmp / "a" / "games", self.tmp / "b" / "games"], ["mil"], per=1)
        i = out.index("   FIRST PARTING at GameTime 2400:")
        self.assertTrue(out[i + 1].startswith("     a/games: impai: mil seat=1 t=2400 attack=1"))
        self.assertTrue(out[i + 2].startswith("     b/games: impai: mil seat=1 t=2400 attack=0"))
        # beyond the lines printed: --lines 1 printed only t=900, the same in both
        game(self.tmp / "b" / "games", "bar-x-1", LOG)
        out = diag.report([self.tmp / "a" / "games", self.tmp / "b" / "games"], ["mil"], per=1)
        self.assertIn("   the two agree in every matched line", out)

    def test_a_failed_build_prints_the_compiler_s_lines_alone(self):
        text = "\n".join(["i686-w64-mingw32-gcc -Iinc -O2 -c src/impai_mil.c",
                          "src/impai_mil.c:12:5: error: 'x' undeclared",
                          "src/impai_mil.c:20:1: warning: unused variable 'y'",
                          "make: *** [src/impai_mil.o] Error 1"])
        self.assertEqual(diag.compiler_lines(text), ["src/impai_mil.c:12:5: error: 'x' undeclared",
                                                     "src/impai_mil.c:20:1: warning: unused variable 'y'"])

    def test_a_game_cut_short_parts_where_its_lines_end(self):
        self.assertEqual(diag.parting(["impai: x t=5", "impai: y t=9"], ["impai: x t=5"]),
                         (9, "impai: y t=9", None))


class Arguments(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)
        self.dll = self.tmp / "ddraw.dll"
        self.dll.write_bytes(b"MZ")

    def test_run_names_its_games_one_way(self):
        rc, out = run(["run", "--dll", str(self.dll), "--ids", "bar-hota-pincushion-aa-1", "--situation", "air-rush"])
        self.assertEqual(rc, 2)
        self.assertIn("--ids or --situation", out[0])
        rc, out = run(["run", "--dll", str(self.dll)])
        self.assertEqual(rc, 2)

    def test_run_takes_one_or_two_dlls_that_exist(self):
        rc, out = run(["run"] + ["--dll", str(self.dll)] * 3 + ["--situation", "air-rush"])
        self.assertEqual((rc, out), (2, ["diag: COULD NOT RUN: --dll at most twice"]))
        rc, out = run(["run", "--dll", str(self.tmp / "none.dll"), "--situation", "air-rush"])
        self.assertEqual(rc, 2)
        self.assertIn("no DLL", out[0])

    def test_a_pattern_that_does_not_compile_is_refused(self):
        rc, out = run(["logs", str(self.tmp), "--grep", "mil("])
        self.assertEqual(rc, 2)

    def test_logs_takes_one_or_two_folders_that_exist(self):
        rc, out = run(["logs", str(self.tmp / "none")])
        self.assertEqual(rc, 2)
        rc, out = run(["logs", str(self.tmp), str(self.tmp), str(self.tmp)])
        self.assertEqual(rc, 2)

    def test_build_refuses_a_bad_name_or_define(self):
        self.assertEqual(run(["build", "--export", "../x"])[0], 2)
        self.assertEqual(run(["build", "--define", "1X=2"])[0], 2)

    def test_run_plays_each_dll_into_its_own_folder_then_reports(self):
        played = []

        def play(games, pa, out):
            played.append((pa.dll, out.name, [g["id"] for g in games], pa.gpu, pa.draw))
            game(out, games[0]["id"], LOG if out.name == "dll1" else LOG.replace("built=3", "built=2"))
            return 0
        other = self.tmp / "other.dll"
        other.write_bytes(b"MZ")
        with mock.patch.object(bar, "play_games", play):
            rc, out = run(["run", "--dll", str(self.dll), "--dll", str(other), "--ids", "bar-hota-pincushion-aa-1",
                           "--out", str(self.tmp / "o"), "--grep", "pulse"])
        self.assertEqual(rc, 0)
        self.assertEqual(played, [(str(self.dll), "dll1", ["bar-hota-pincushion-aa-1"], False, False),
                                  (str(other), "dll2", ["bar-hota-pincushion-aa-1"], False, False)])
        self.assertIn("   FIRST PARTING at GameTime 1807:", out)
        self.assertLessEqual(len(out), diag.MAX_LINES + 1)


if __name__ == "__main__":
    unittest.main()
