import os
import subprocess
import tempfile
import unittest

from tools.msg_codegen import CodeGenerator, IDLParser, ParseError


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


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

    def test_multifile_import_and_split_domain_headers(self):
        msg_path = os.path.join(ROOT, "msg", "adas_msgs.msg")
        parser = IDLParser.from_file(msg_path)
        self.assertIn("Header", parser.structs)
        self.assertIn("TrafficLightList", parser.structs)
        self.assertIn("PredictionBatch", parser.structs)
        self.assertIn("NavPath", parser.structs)

        with tempfile.TemporaryDirectory() as tmp:
            gen = CodeGenerator(parser)
            files = gen.generate_split(tmp, "adas_msgs")
            self.assertIn("gen/common.h", files)
            self.assertIn("gen/map.h", files)
            self.assertIn("gen/planning.h", files)
            self.assertIn("gen/prediction.h", files)
            self.assertIn("gen/adas_msgs_gen.h", files)

    def test_cyclic_import_raises_parse_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = os.path.join(tmp, "a.msg")
            b = os.path.join(tmp, "b.msg")
            with open(a, "w", encoding="utf-8") as f:
                f.write('import "b.msg"\nstruct A { uint32 x }\n')
            with open(b, "w", encoding="utf-8") as f:
                f.write('import "a.msg"\nstruct B { uint32 y }\n')
            with self.assertRaises(ParseError):
                IDLParser.from_file(a)

    def test_wire_size_equals_sizeof_and_varlen_roundtrip_c(self):
        msg_path = os.path.join(ROOT, "msg", "adas_msgs.msg")
        parser = IDLParser.from_file(msg_path)
        gen = CodeGenerator(parser)

        with tempfile.TemporaryDirectory() as tmp:
            gen.generate_split(tmp, "adas_msgs")
            checks = "\n".join(
                f'    if (sizeof({name}) != {name.upper()}_WIRE_SIZE) return {idx + 1};'
                for idx, name in enumerate(sorted(parser.structs.keys()))
            )
            c_src = f"""
#include <stdio.h>
#include <string.h>
#include "adas_msgs_gen.h"

void flow_registry_on_type_registered(const TypeRegistryEntry* e) {{ (void)e; }}

int main(void) {{
{checks}

    /* Varlen builder + reader round-trip: NavPath */
    uint8_t buf[1024];
    FlowMsgBuilder b;
    if (NavPath_init_builder(&b, buf, sizeof(buf)) != 0) return 100;
    NavPath* hdr = (NavPath*)buf;
    hdr->header.seq = 42;
    hdr->header.stamp_us = 123456789ULL;
    hdr->route_id = 7;
    hdr->total_length = 250.5f;
    if (NavPath_set_route_name(&b, "park_to_park") != 0) return 101;
    Point3f* pts = NavPath_alloc_waypoints(&b, 3);
    if (!pts) return 102;
    pts[0] = (Point3f){{1.0f, 2.0f, 3.0f}};
    pts[1] = (Point3f){{4.0f, 5.0f, 6.0f}};
    pts[2] = (Point3f){{7.0f, 8.0f, 9.0f}};
    uint32_t total = NavPath_finish(&b);
    if (total <= sizeof(NavPath)) return 103;

    const NavPath* rx = (const NavPath*)buf;
    uint32_t name_len = 0;
    const char* rname = NavPath_route_name_checked(rx, total, &name_len);
    if (!rname || strcmp(rname, "park_to_park") != 0) return 104;
    uint32_t wp_cnt = 0;
    const Point3f* rpts = NavPath_waypoints_checked(rx, total, &wp_cnt);
    if (!rpts || wp_cnt != 3 || rpts[2].z != 9.0f) return 105;

    /* Truncated buffer must fail checked reader */
    if (NavPath_waypoints_checked(rx, total - 1, &wp_cnt) != NULL) return 106;

    /* Serialize must append the variable-length payload (not just the
     * FlowSeqRef descriptor), and deserialize_into must materialize it. */
    uint8_t wire[1024];
    size_t wire_size = 0;
    if (NavPath_serialize(rx, wire, &wire_size) != 0 || wire_size < total) return 107;
    uint8_t dstbuf[1024];
    NavPath* dst = (NavPath*)dstbuf;
    if (NavPath_deserialize_into(dst, sizeof(dstbuf), wire, wire_size) != 0) return 108;
    uint32_t n2 = 0;
    const char* rn2 = NavPath_route_name_checked(dst, wire_size, &n2);
    const Point3f* rp2 = NavPath_waypoints_checked(dst, wire_size, &wp_cnt);
    if (!rn2 || strcmp(rn2, "park_to_park") != 0 || !rp2 || wp_cnt != 3 || rp2[2].z != 9.0f) return 109;
    if (NavPath_deserialize(dst, wire, wire_size) != -2) return 110;
    return 0;
}}
"""
            c_file = os.path.join(tmp, "test_v2.c")
            bin_file = os.path.join(tmp, "test_v2")
            with open(c_file, "w", encoding="utf-8") as f:
                f.write(c_src)

            res = subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra",
                    f"-I{os.path.join(ROOT, 'include')}",
                    f"-I{os.path.join(tmp, 'gen')}",
                    c_file,
                    os.path.join(ROOT, "src", "core", "serializer.c"),
                    "-o", bin_file,
                ],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(res.returncode, 0, f"cc failed:\n{res.stderr}")
            run = subprocess.run([bin_file], capture_output=True, text=True, check=False)
            self.assertEqual(run.returncode, 0, f"binary returned {run.returncode}")


if __name__ == "__main__":
    unittest.main()
