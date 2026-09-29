"""Control-flow and capacity regressions for the offline COB audit."""
import unittest

import cob_audit as audit


class AuditTests(unittest.TestCase):
    def check_bos(self, source):
        return audit.audit_blob(audit.tacob.write_cob(audit.tacob.compile_bos(source)))

    def test_deep_expression_and_getter(self):
        expression = " || ".join("get UNIT_HEIGHT(1) == " + str(i) for i in range(45))
        report = self.check_bos("piece base; Detect() { if (" + expression + ") { hide base; } }")
        script = report["scripts"][0]
        self.assertEqual(script["gets"], [11])
        self.assertFalse(script["issues"])
        self.assertGreater(report["peak_words"], 0)

    def test_long_stack_is_not_silently_clipped(self):
        op = audit.tacob.OP
        code = [word for _ in range(85) for word in (op["PUSH_CONSTANT"], 1)]
        code += [op["POP_STACK"]] * 84 + [op["RETURN"]]
        cob = audit.tacob.Cob([("Deep", 0)], [], 0, code)
        report = audit.audit_blob(audit.tacob.write_cob(cob))
        self.assertEqual(report["peak_words"], 85)
        self.assertFalse(report["scripts"][0]["issues"])

    def test_loop_and_arguments(self):
        report = self.check_bos("piece base; Create() { while (TRUE) { start-script Child(1, 2); sleep 30; } } Child(a,b) { return (a+b); }")
        self.assertTrue(report["scripts"][0]["calls"])
        self.assertTrue(all(not s["issues"] for s in report["scripts"]))

    def test_underflow(self):
        cob = audit.tacob.Cob([("Bad", 0)], [], 0, [audit.tacob.OP["SET"]])
        result = audit.audit_blob(audit.tacob.write_cob(cob))
        self.assertIn("underflow", result["scripts"][0]["issues"][0]["reason"])

    def test_jump_into_operand(self):
        op = audit.tacob.OP
        cob = audit.tacob.Cob([("Bad", 0)], [], 0,
                            [op["PUSH_CONSTANT"], 0, op["JUMP"], 1])
        result = audit.audit_blob(audit.tacob.write_cob(cob))
        self.assertIn("instruction operand", result["scripts"][0]["issues"][0]["reason"])

    def test_conditional_locals_join_at_return(self):
        op = audit.tacob.OP
        cob = audit.tacob.Cob([("Upgrade", 0)], [], 0,
                            [op["PUSH_CONSTANT"], 75, op["GET_UNIT_VALUE"],
                             op["JUMP_NOT_EQUAL"], 6, op["CREATE_LOCAL_VAR"],
                             op["PUSH_CONSTANT"], 0, op["RETURN"]])
        result = audit.audit_blob(audit.tacob.write_cob(cob))
        self.assertFalse(result["scripts"][0]["issues"])
        self.assertEqual(result["peak_words"], 2)

    def test_constant_branch_does_not_reach_invalid_target(self):
        op = audit.tacob.OP
        cob = audit.tacob.Cob([("Loop", 0)], [], 0,
                            [op["PUSH_CONSTANT"], 1, op["JUMP_NOT_EQUAL"], 99,
                             op["JUMP"], 0])
        result = audit.audit_blob(audit.tacob.write_cob(cob))
        self.assertFalse(result["scripts"][0]["issues"])

    def test_computed_literal_getter_is_counted(self):
        op = audit.tacob.OP
        cob = audit.tacob.Cob([("Probe", 0)], [], 0,
                            [op["PUSH_CONSTANT"], 1, op["PUSH_CONSTANT"], 1,
                             op["EQUAL"], op["GET_UNIT_VALUE"], op["RETURN"]])
        result = audit.audit_blob(audit.tacob.write_cob(cob))["scripts"][0]
        self.assertEqual(result["gets"], [1])
        self.assertFalse(result["dynamic_get"])

    def test_literal_arithmetic_wraps_to_signed_word(self):
        self.assertEqual(audit.literal_result("ADD", [2147483647, 1]), -2147483648)
        self.assertIsNone(audit.literal_result("DIV", [1, 0]))

    def test_growing_stack_loop_is_bounded_analysis(self):
        op = audit.tacob.OP
        cob = audit.tacob.Cob([("Bad", 0)], [], 0,
                            [op["CREATE_LOCAL_VAR"], op["JUMP"], 0])
        result = audit.audit_script(cob, "Bad", 0, analysis_limit=8)
        self.assertTrue(any("stack limit" in i["reason"] for i in result["issues"]))

    def test_worklist_is_bounded(self):
        op = audit.tacob.OP
        cob = audit.tacob.Cob([("Long", 0)], [], 0,
                            [op["PUSH_CONSTANT"], 0, op["RETURN"]])
        result = audit.audit_script(cob, "Long", 0, state_limit=1)
        self.assertTrue(any("state limit" in i["reason"] for i in result["issues"]))

    def test_alias_has_complete_code(self):
        cob = audit.tacob.Cob([("One", 0), ("Two", 0)], [], 0,
                            [audit.tacob.OP["PUSH_CONSTANT"], 0, audit.tacob.OP["RETURN"]])
        result = audit.audit_blob(audit.tacob.write_cob(cob))
        self.assertEqual([s["peak_words"] for s in result["scripts"]], [1, 1])

    def test_shared_tail_and_unreachable_data(self):
        op = audit.tacob.OP
        cob = audit.tacob.Cob([("One", 0), ("Two", 3)], [], 0,
                            [op["JUMP"], 3, 0xDEADBEEF,
                             op["PUSH_CONSTANT"], 0, op["RETURN"], 0xDEADBEEF])
        result = audit.audit_blob(audit.tacob.write_cob(cob))
        self.assertTrue(all(not s["issues"] for s in result["scripts"]))

    def test_interleaved_local_declarations(self):
        op = audit.tacob.OP
        cob = audit.tacob.Cob([("One", 0)], [], 0,
                            [op["CREATE_LOCAL_VAR"], op["PUSH_CONSTANT"], 1,
                             op["POP_LOCAL_VAR"], 0, op["CREATE_LOCAL_VAR"],
                             op["PUSH_LOCAL_VAR"], 0, op["POP_LOCAL_VAR"], 1,
                             op["PUSH_LOCAL_VAR"], 1, op["RETURN"]])
        result = audit.audit_blob(audit.tacob.write_cob(cob))["scripts"][0]
        self.assertFalse(result["issues"])
        self.assertEqual(result["local_words"], 2)


if __name__ == "__main__":
    unittest.main()
