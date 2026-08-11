import unittest

from tools.msg_codegen import CodeGenerator, IDLParser


class MessageCodegenTest(unittest.TestCase):
    def test_fixed_arrays_use_runtime_loops(self):
        parser = IDLParser(
            """
            struct Point {
                float x
                float y
            }

            struct PointCloud {
                uint32 count
                Point points[2048]
            }
            """
        )
        parser.parse()
        generated = CodeGenerator(parser).generate()

        self.assertIn("for (size_t i = 0; i < 2048; i++)", generated)
        self.assertIn("src->points[i]", generated)
        self.assertNotIn("src->points[2047]", generated)


if __name__ == "__main__":
    unittest.main()
