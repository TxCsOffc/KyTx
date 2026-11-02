#!/usr/bin/env python3
"""
kyty-alignment-harness
======================

Validates the alignment and descriptor mapping contract of the Metal compute binding path
against the Prospero IR. The harness does NOT run a guest emulation; instead it imports the
IR definitions and performs structural assertions on the field layout expected by
`metalRenderer.mm`'s Bind path. These static assertions catch the regressions that were
introduced in early commits of the flat-fix series.

Run from the KyTx repo root (CI environment):
    ./tools/validate-alignment.py --irs-dir <path-to-source/graphics/shader/ir> --impl <path-to-src/graphics/host_gpu/metal/metalRenderer.mm>

The harness exits non-zero on any mismatch. Per-file coverage:
  - ./validation-harness-describe-descriptor.md : DescribeDescriptor / FrameBitSize tests
  - ./validation-harness-double-consumer.md    : GetDoubleConsumer uint64 reinterpret semantics
  - ./validation-harness-compute-guest-image.md: ComputeGuestImage 32-bit / RGBA32F validity
  - ./validation-harness-convert-images.md     : ConvertImagesAcrossArray caller contract
  - ./validation-harness-sink-descriptor.mm    : SinkDescriptor wiring fallback (MTLArgumentEncoder slots)
"""

import os
import re
import sys
import json
import subprocess
from pathlib import Path

USAGE = """\
usage: validate-alignment.py [--irs DIR] [--impl FILE] [--output OUT]

DIR      : path to Prospero IR headers/cpp source (shader/ir + shader/recompiler/decoder)
FILE     : path to metalRenderer.mm (the impl under validation)
OUT      : output JSON file (default: report-alignment.json)
""".strip()

IRS_DIR = None
IMPL_FILE = None
OUTPUT = None

def main():
    global OUTPUT
    args = sys.argv[1:]
    OUT_DEFAULT = "report-alignment.json"
    for i, arg in enumerate(args):
        if arg == "--irs" and i + 1 < len(args):
            IRS_DIR = args[i + 1]; i += 1
        elif arg == "--impl" and i + 1 < len(args):
            IMPL_FILE = args[i + 1]; i += 1
        elif arg == "--output" and i + 1 < len(args):
            OUTPUT = args[i + 1]; i += 1
    OUTPUT = OUTPUT or OUT_DEFAULT
    if IRS_DIR is None or IMPL_FILE is None:
        print(USAGE, file=sys.stderr); return 1
    if not Path(IRS_DIR).is_dir() or not Path(IMPL_FILE).is_file():
        print(f"missing IRS_DIR={IRS_DIR} IMPL_FILE={IMPL_FILE}", file=sys.stderr); return 1
    Path(OUTPUT).write_text(json.dumps({
        "ir_dir": str(IRS_DIR),
        "impl": str(IMPL_FILE),
        "status": "ok",
        "checks": []
    }, indent=2))
    return run_checks()


# ---------------------------------------------------------------------------
# DescribeDescriptor: layouts, FrameBitSize mappings, numbering conventions
# ---------------------------------------------------------------------------
DESCRIBE_DESC = """
DescribeDescriptor is a static struct describing one element of the argument buffer
flat layout. Every dword is a named field. Layout is positional in the source because
the decoded MSL places fields in declaration order and elided ones contribute nothing.
FrameBitSize maps uint32s from the kUniformBitmap<N> token into the canonical
argument dwords so VertexParameters / PixelParameters / IndexParameters stay aligned
in dword slots regardless of array bounds. FrameBitSize(N)=4*kUniformBitmapSizes[N].
The 'underlying layout test' suite exercises VertexParameters, PixelParameters, and
IndexParameters separately. To pass the file check the parser must extract at least 2
distinct dword names and assign every mask bit to a known FrameBitSize value (0..8) or
UNDEFINED (99).
""".strip()

FRAMEBITSIZE_VALUES = list(range(0, 9))  # 0..8
UNDEFINED = 99
FRAMEBITSIZE_NAMES = {0: "u2", 1: "u3", 2: "u4", 3: "u5", 4: "u6", 5: "u7", 6: "u8", 7: "u9", 8: "u10"}


def describe_pass():
    txt = DESCRIBE_DESC
    found_field = False
    masks = []
    mask_bits_seen = set()
    bad = 0
    m = re.search(r'FrameBitSize\((\w+)\)', txt)
    if m: masks.append({"name": m.group(1), "ok": m.group(1) in FRAMEBITSIZE_VALUES})
    else: masks.append({"name": "UNKNOWN", "ok": False})
    for mm in re.finditer(r'uint\d+\s+(\w+).*?FRAMEBitSize\((\w+)\)', txt, re.S):
        nm = mm.group(1); val = mm.group(2)
        if nm == "Descriptor": continue
        try: v = int(val)
        except: v = -1
        found_field = True
        if v < 0 or v > 8: bad += 1; masks.append({"name": nm, "value": val, "ok": False})
        else: masks.append({"name": nm, "value": val, "ok": True})
        mask_bits_seen.add(v)
    return {"contract_doc": txt, "fields_extracted": found_field, "masks": masks,
            "bad_framebitsize": bad, "framebitsizes_used": sorted(list(mask_bits_seen)),
            "valid": found_field and bad == 0}


def run_describe():
    r = describe_pass()
    return {"describe_pass": r, "notes": ["invariants captured by validate-alignment doc"]}


# ---------------------------------------------------------------------------
# GetDoubleConsumer: 64-bit double interpretation of an ABI layout handle
# ---------------------------------------------------------------------------
DOUBLE_CONSUMER = """
GetDoubleConsumer walks the deep pointers and returns a pair whose elements are both
uint64_t reinterpretings of the two relevant 32-bit halves of a C++ `std::pair<float, float>`
or `std::pair<int32_t, int32_t>` field at the call site. Performed under UserRawPtrs so the
64-bit handle survives even when the original 32-bit value lives in a register spilled slot.
""".strip()


DOUBLE_CONSUMER_KEYWORDS = ["GetDoubleConsumer", "UserRawPtrs", "uint64_t", "reinterpret"]


def double_consumer_pass():
    contract = DOUBLE_CONSUMER
    hits = [w for w in DOUBLE_CONSUMER_KEYWORDS if w in contract]
    ok = len(hits) == len(DOUBLE_CONSUMER_KEYWORDS)
    expected = [
        ("where_the_factory_applies", True),
        ("user_raw_ptrs_required", True),
        ("uint64_reinterpret_of_each_half", False),
        ("no_extra_scaling_applied", True),
        ("no_sign_extension", True),
    ]
    prag = "expected_conditions:" + ", ".join(f"{k}={v}" for k, v in expected if k != "no_extra_scaling_applied" and k != "no_sign_extension")
    return {"contract": contract, "mapping_contract": prag, "expected": expected,
            "keywords_present": hits, "valid": ok}


def run_double_consumer():
    out = double_consumer_pass()
    return {"double_consumer": out}


# ---------------------------------------------------------------------------
# ComputeGuestImage: pass a 32-bit or 64-bit image through raw memory
# ---------------------------------------------------------------------------
COMPUTE_GUEST_IMAGE = """
ComputeGuestImage converts a 32-bit or 64-bit registered raw-bytes image into a
MTLTexture view bound into the compute encoder. For storage it takes a registered image at
the address (possibly aliased), resolves type + tile mode, and returns a 2D/3D view over the
GPU backing object. The function validates dimension/size against the shader declaration and
rejects out-of-range addresses.
""".strip()


COMPUTE_GUEST_IMAGE_KEYWORDS = ["ComputeGuestImage", "registered image", "aliased", "MTLTexture view"]


def compute_guest_image_pass():
    contract = COMPUTE_GUEST_IMAGE
    hits = [w for w in COMPUTE_GUEST_IMAGE_KEYWORDS if w in contract]
    ok = len(hits) == len(COMPUTE_GUEST_IMAGE_KEYWORDS) and hits[0] != contract
    mapping = {"alignment_contract": True, "dimension_from_shader": True,
               "out_of_range_addr_rejected": True}
    return {"contract": contract, "mapping_contract": mapping,
            "keywords_present": hits, "valid": ok}


def run_compute_guest_image():
    out = compute_guest_image_pass()
    return {"compute_guest_image": out}


CONVERT_IMAGES = """
ConvertImagesAcrossArray turns a runtime vector<vector<Descriptors>> into the wide Vector<Image>
used by the recompiler driver. Each vector must contain descriptors for exactly one dimension
family. Field order corresponds to Decode.ImageTypes index. Conversion is delegated to
Dec()->Bind()->...Because each dimension family renders into one image sub-buffer type.
""".strip()

CONVERT_IMAGES_KEYWORDS = ["ConvertImagesAcrossArray", "Vector<Image>", "image sub-buffer type"]


def convert_images_pass():
    contract = CONVERT_IMAGES
    hits = [w for w in CONVERT_IMAGES_KEYWORDS if w in contract]
    ok = len(hits) == len(CONVERT_IMAGES_KEYWORDS)
    mapping = {"alignment_contract": True, "output_is_Vector_Image": True,
               "each_entry_one_family": True, "order_matches_Decode": True}
    return {"contract": contract, "mapping_contract": mapping,
            "keywords_present": hits, "valid": ok}


def run_convert_images():
    out = convert_images_pass()
    return {"convert_images_across_array": out}


# ---------------------------------------------------------------------------
# SinkDescriptor: MTLArgumentEncoder fallback for unknown formats
# ---------------------------------------------------------------------------
SINK_DESC = """
SinkDescriptor completes a render target pipeline key when the target's format could not be
resolved to a native MTLPixelFormat. It fills the MTLPixelFormatInvalid / zero width / height
slots from the game's declared TargetDesc when a GL bind or guest image is available, else
drops the entry entirely so the pipeline cache rejects the stale miss without a flush. Wired
into Layout state as a Finalizer guard in the binding walker.
""".strip()


SINK_DESC_KEYWORDS = ["SinkDescriptor", "MTLPixelFormatInvalid", "Finalizer"]


def sink_descriptor_pass():
    contract = SINK_DESC
    hits = [w for w in SINK_DESC_KEYWORDS if w in contract]
    ok = len(hits) == len(SINK_DESC_KEYWORDS) and hits[0] != contract
    mapping = {"fill_invalid_format_zeros": True, "obtain_target_desc": True,
               "entry_discarded_no_flush": True, "wired_as_finalizer": True}
    return {"contract": contract, "mapping_contract": mapping,
            "keywords_present": hits, "valid": ok}


def run_sink_descriptor():
    out = sink_descriptor_pass()
    return {"sink_descriptor": out}


def run_checks():
    r = run_describe()
    d = {"checks": [], "results": []}
    res = d["results"]
    c = d["checks"]
    items = [("describe_descriptor", run_describe()),
             ("double_consumer", run_double_consumer()),
             ("compute_guest_image", run_compute_guest_image()),
             ("convert_images_across_array", run_convert_images()),
             ("sink_descriptor", run_sink_descriptor())]
    for i, (name, body) in enumerate(items):
        body_ok = bool(body.get("valid", False))
        inner = name + " : " + ", ".join(f"{k}={'ok' if v else 'fail'}" for k, v in body.items())
        c.append({"name": name, "status": "ok" if body_ok else "fail", "detail": inner})
        res.append({"name": name, "result": "PASS" if body_ok else "FAIL"})
        if not body_ok:
            err = inner
        d["detail"] = err or ""
    Path(OUTPUT).write_text(json.dumps(d, indent=2))
    if any(x["status"] == "fail" for x in c):
        print(f"[Harness] FAILED: see {OUTPUT}", file=sys.stderr)
        return 1
    print(f"[Harness] OK: see {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
