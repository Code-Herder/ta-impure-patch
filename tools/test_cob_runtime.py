"""Compile the production COB safety core natively and exercise its public contract."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest

import cob_audit

ROOT = Path(__file__).resolve().parents[1]


class Record(C.Structure):
    _fields_ = [("status", C.c_uint32), ("pc", C.c_uint32), ("sp", C.c_int32),
                ("sleep", C.c_int32), ("piece", C.c_int32), ("axis", C.c_int32),
                ("child", C.c_int32), ("mask", C.c_uint32), ("callback", C.c_uint32),
                ("stack", C.c_int32 * 128)]


class RuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="cob-runtime-")
        library = Path(cls.temp.name) / "cob.so"
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        "-fsanitize=undefined", "-I", str(ROOT / "tagpu/ddraw/inc"),
                        str(ROOT / "tagpu/ddraw/src/tagpu_cob_safe.c"), "-o", str(library)], check=True)
        cls.lib = C.CDLL(str(library))
        cls.lib.tagpu_cob_id_valid.argtypes = [C.c_int32, C.c_uint32]
        cls.lib.tagpu_cob_build_left.argtypes = [C.c_float]
        cls.lib.tagpu_cob_program.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_size_t]
        cls.lib.tagpu_cob_program.restype = C.c_void_p
        cls.lib.tagpu_cob_program_free.argtypes = [C.c_void_p]
        cls.lib.tagpu_cob_bind_model.argtypes = [C.c_void_p, C.c_uint32, C.c_void_p, C.c_size_t]
        cls.lib.tagpu_cob_check_step.argtypes = [C.c_void_p, C.POINTER(Record), C.c_void_p, C.c_size_t]
        cls.lib.tagpu_cob_save_records.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(Record),
                                                  C.c_void_p, C.c_size_t, C.c_void_p, C.c_size_t]
        cls.lib.tagpu_cob_load_records.argtypes = [C.c_void_p, C.c_uint32, C.c_void_p,
                                                  C.c_size_t, C.POINTER(Record), C.c_void_p,
                                                  C.c_void_p, C.c_size_t]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_full_width_unit_ids_before_narrowing(self):
        for count in (5001, 10001, 15001, 65535):
            for value in (1, count - 1):
                self.assertTrue(self.lib.tagpu_cob_id_valid(value, count))
            for value in (-2147483648, -1, 0, count, 65535, 65536, 65537, 2147483647):
                self.assertFalse(self.lib.tagpu_cob_id_valid(value, count))

    def validate(self, blob):
        data = C.create_string_buffer(blob)
        reason = C.create_string_buffer(256)
        program = self.lib.tagpu_cob_program(data, len(blob), reason, len(reason))
        if program:
            self.lib.tagpu_cob_program_free(program)
        return bool(program), reason.value.decode()

    def test_build_completion_cannot_alias_an_invalid_or_incomplete_unit(self):
        for remaining, expected in ((0, 0), (1, 100), (.5, 50), (.000001, 1)):
            self.assertEqual(self.lib.tagpu_cob_build_left(remaining), expected)
        for remaining in (-1, 2, float('nan'), float('inf')):
            self.assertEqual(self.lib.tagpu_cob_build_left(remaining), -1)

    def test_valid_program_and_truncated_header(self):
        blob = cob_audit.tacob.write_cob(cob_audit.tacob.compile_bos("piece base; Create() { hide base; }"))
        self.assertEqual(self.validate(blob), (True, ""))
        for size in range(44):
            self.assertFalse(self.validate(blob[:size])[0])

    def test_unterminated_name(self):
        blob = cob_audit.tacob.write_cob(cob_audit.tacob.compile_bos("piece base; Create() { hide base; }"))
        self.assertFalse(self.validate(blob[:-1])[0])

    def blob(self, code, pieces=(), statics=0):
        return cob_audit.tacob.write_cob(cob_audit.tacob.Cob([("Test", 0)], pieces, statics, code))

    def test_targets_and_inline_accesses_are_bounded(self):
        op = cob_audit.tacob.OP
        cases = [[op["JUMP"], 1], [op["JUMP"], 999], [op["PUSH_CONSTANT"]],
                 [op["PUSH_LOCAL_VAR"], 128], [op["PUSH_STATIC_VAR"], 0],
                 [op["SHOW"], 0], [op["CALL_SCRIPT"], 1, 0],
                 [op["CALL_SCRIPT"], 0, 129]]
        for code in cases:
            self.assertFalse(self.validate(self.blob(code))[0], code)

    def test_stock_terminal_zero_after_code_is_bounded(self):
        op = cob_audit.tacob.OP
        # write_cob places entry[0] (zero) immediately after the code.
        self.assertTrue(self.validate(self.blob([op["JUMP"], 2]))[0])
        self.assertTrue(self.validate(self.blob([1]))[0])

    def test_stack_capacity_guards_the_next_access(self):
        op = cob_audit.tacob.OP
        data = C.create_string_buffer(self.blob([op["PUSH_CONSTANT"], 0, op["RETURN"]]))
        reason = C.create_string_buffer(256)
        program = self.lib.tagpu_cob_program(data, len(data) - 1, reason, len(reason))
        self.assertTrue(program, reason.value)
        try:
            rec = Record(status=0x1000000, sp=126)
            self.assertEqual(self.lib.tagpu_cob_check_step(program, C.byref(rec), reason, len(reason)), 1)
            rec.sp = 127
            self.assertEqual(self.lib.tagpu_cob_check_step(program, C.byref(rec), reason, len(reason)), 0)
            rec.pc, rec.sp, rec.callback = 2, -1, 1
            self.assertEqual(self.lib.tagpu_cob_check_step(program, C.byref(rec), reason, len(reason)), 0)
            rec.callback = 0
            self.assertEqual(self.lib.tagpu_cob_check_step(program, C.byref(rec), reason, len(reason)), 1)
            rec.status, rec.pc = 0, 0xffffffff
            self.assertEqual(self.lib.tagpu_cob_check_step(program, C.byref(rec), reason, len(reason)), 0)
        finally:
            self.lib.tagpu_cob_program_free(program)

    def test_native_division_faults_are_rejected_before_idiv(self):
        self.check_steps([0x10034000, 0x10065000], (), [
            ([123, 0], 0, "division by zero"),
            ([-2147483648, -1], 0, "division overflow"),
            ([123, -1], 1, ""),
            ([-2147483648, 1], 1, ""),
        ])

    def test_model_binding_allows_unused_names_but_not_reachable_accesses(self):
        for piece, valid in ((0, True), (1, False)):
            data = C.create_string_buffer(self.blob([0x10005000, piece, 0x10065000], ['base', 'unused']))
            reason = C.create_string_buffer(256)
            program = self.lib.tagpu_cob_program(data, len(data) - 1, reason, len(reason))
            self.assertTrue(program, reason.value)
            try:
                self.assertEqual(bool(self.lib.tagpu_cob_bind_model(program, 1, reason, len(reason))), valid)
                if not valid:
                    self.assertIn(b'model allocation', reason.value)
            finally:
                self.lib.tagpu_cob_program_free(program)

    def check_steps(self, code, pieces, cases):
        data = C.create_string_buffer(self.blob(code, pieces))
        reason = C.create_string_buffer(256)
        program = self.lib.tagpu_cob_program(data, len(data) - 1, reason, len(reason))
        self.assertTrue(program, reason.value)
        try:
            for stack, expected, message in cases:
                with self.subTest(code=code, stack=stack):
                    reason.value = b""
                    rec = Record(status=0x01000000, sp=len(stack) - 1)
                    rec.stack[:len(stack)] = stack
                    self.assertEqual(self.lib.tagpu_cob_check_step(
                        program, C.byref(rec), reason, len(reason)), expected)
                    self.assertIn(message, reason.value.decode())
        finally:
            self.lib.tagpu_cob_program_free(program)

    def test_native_vtable_piece_operations_continue_and_bound_their_operands(self):
        self.check_steps([0x10009000, 0, 0x10065000], ["base"], [
            ([1], 0, "underflow"), ([1, 2], 1, "")])
        self.check_steps([0x1000a000, 0, 0x10065000], ["base"], [([], 1, "")])
        for opcode in (0x10009000, 0x1000a000):
            self.assertFalse(self.validate(self.blob([opcode, 1, 0x10065000], ["base"]))[0])
            self.assertFalse(self.validate(self.blob([opcode, 0, 0x10005000, 2], ["base"]))[0])

    def test_expanded_save_round_trip_and_old_save_import_are_transactional(self):
        import struct
        op = cob_audit.tacob.OP
        blob = C.create_string_buffer(self.blob([op['RETURN']]))
        reason = C.create_string_buffer(256)
        program = self.lib.tagpu_cob_program(blob, len(blob) - 1, reason, len(reason))
        self.assertTrue(program, reason.value)
        try:
            records = (Record * 8)()
            records[0] = Record(status=0x01000000, sp=100, callback=0x12345678)
            records[0].stack[100] = 9876
            data = C.create_string_buffer(24 + 8 * C.sizeof(Record))
            self.assertTrue(self.lib.tagpu_cob_save_records(program, 99, records, data, len(data), reason, len(reason)), reason.value)
            result, busy = (Record * 8)(), C.c_uint()
            self.assertTrue(self.lib.tagpu_cob_load_records(program, 99, data, len(data), result, C.byref(busy), reason, len(reason)), reason.value)
            self.assertEqual((busy.value, result[0].sp, result[0].stack[100], result[0].callback), (1, 100, 9876, 0))
            before = bytes(result)
            for size in (0, 4, 23, len(data) - 1):
                self.assertFalse(self.lib.tagpu_cob_load_records(program, 99, data, size, result, C.byref(busy), reason, len(reason)))
                self.assertEqual(bytes(result), before)
            self.assertFalse(self.lib.tagpu_cob_load_records(program, 98, data, len(data), result, C.byref(busy), reason, len(reason)))
            old = struct.pack('<I', 99) + bytes(8 * 164) + struct.pack('<I', 0)
            self.assertTrue(self.lib.tagpu_cob_load_records(program, 99, old, len(old), result, C.byref(busy), reason, len(reason)), reason.value)
            self.assertEqual(busy.value, 0)
            old = bytearray(old)
            struct.pack_into('<III', old, 4, 0x01000000, 0, 32)
            self.assertFalse(self.lib.tagpu_cob_load_records(program, 99, bytes(old), len(old), result, C.byref(busy), reason, len(reason)))
        finally:
            self.lib.tagpu_cob_program_free(program)


if __name__ == "__main__":
    unittest.main()
