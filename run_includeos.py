#!/usr/bin/env python3
import os
import sys

shared_dir = sys.argv[1]

json_file = f"""{{
    "mem" : 3072,
    "virtiofs" : {{
        "shared" : "{shared_dir}"
    }}
}}
"""

with open("vm.json", "w") as f:
    f.write(json_file)
# Get path of the vm.json
vm_json_path = os.path.realpath("./vm.json")

# Execute benchmark
os.system(f"boot --sudo --kvm {shared_dir}/virtiofs_bench -j {vm_json_path}")

# Cleanup
os.system("rm ./vm.json")
