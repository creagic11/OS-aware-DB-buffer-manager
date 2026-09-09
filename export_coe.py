"""
Xilinx COE (Coefficient) Generator & Verifier for Artix-7 BRAM Deployment
========================================================================
Extracts the INT8 quantized synaptic weights from the trained model's
state_dict and formats them into standard Xilinx Coefficient (.coe) files.

Deliverables Verified:
  1. Syntax Compliance: Validates header syntax ('memory_initialization_radix=10;',
     'memory_initialization_vector='), comma separation, and terminating semicolon ';'.
  2. Data Type Consistency: Validates that all elements are signed INT8 integers (-128 to 127).
  3. Dimension Alignment: Validates that vector length matches exactly N x M for BRAM blocks.
"""

import pathlib
import torch
from torch.utils.data import DataLoader
from torchvision import datasets, transforms
import torch.ao.quantization as quantization

from train import SpikingNet, quantize_model_ptq, DATA_DIR, CKPT_DIR, BATCH_SIZE


def write_coe_file(
    file_path: pathlib.Path,
    weights_tensor: torch.Tensor,
    layer_name: str,
    values_per_line: int = 16,
):
    """Format and write integer weights to a standard Xilinx .coe file."""
    flat_weights = weights_tensor.flatten().tolist()
    total_weights = len(flat_weights)
    file_path.parent.mkdir(parents=True, exist_ok=True)

    with open(file_path, "w", encoding="utf-8") as f:
        # Required Xilinx syntax headers
        f.write("memory_initialization_radix=10;\n")
        f.write("memory_initialization_vector=\n")

        # Comma-separated integer stream
        for i in range(0, total_weights, values_per_line):
            chunk = flat_weights[i : i + values_per_line]
            is_last_chunk = (i + values_per_line) >= total_weights

            if is_last_chunk:
                line_str = ", ".join(str(int(w)) for w in chunk[:-1])
                if len(chunk) > 1:
                    line_str += f", {int(chunk[-1])};\n"
                else:
                    line_str += f"{int(chunk[0])};\n"
                f.write(line_str)
            else:
                line_str = ", ".join(str(int(w)) for w in chunk) + ",\n"
                f.write(line_str)

    print(f"[COE Created] {file_path.name:20s} | Layer: {layer_name}")
    print(f"              Target: Artix-7 BRAM Block | Array Shape: {list(weights_tensor.shape)}")
    print(f"              Total Values: {total_weights:,} INT8 integers")


def verify_coe_file(file_path: pathlib.Path, expected_shape: tuple[int, int]):
    """Verify Syntax Compliance, Data Type Consistency, and Dimension Alignment."""
    print("\n" + "-" * 80)
    print(f"  VERIFYING DELIVERABLES FOR: {file_path.name}")
    print("-" * 80)

    assert file_path.exists(), f"File does not exist: {file_path}"
    with open(file_path, "r", encoding="utf-8") as f:
        content = f.read()

    lines = [line.strip() for line in content.strip().split("\n") if line.strip()]

    # 1. Syntax Compliance Check
    print("  [Check 1: Syntax Compliance]")
    assert lines[0] == "memory_initialization_radix=10;", f"Invalid radix header: {lines[0]}"
    assert lines[1] == "memory_initialization_vector=", f"Invalid vector header: {lines[1]}"
    assert content.strip().endswith(";"), "File does NOT terminate with a semicolon ';'"
    print("    * Radix Header    : 'memory_initialization_radix=10;' [MATCH]")
    print("    * Vector Header   : 'memory_initialization_vector='   [MATCH]")
    print("    * Termination     : Semicolon ';' at EOF              [MATCH]")

    # Extract all values
    vector_text = "".join(lines[2:]).rstrip(";")
    tokens = [tok.strip() for tok in vector_text.split(",") if tok.strip()]

    # 2. Dimension Alignment Check
    expected_count = expected_shape[0] * expected_shape[1]
    actual_count = len(tokens)
    print("\n  [Check 2: Dimension Alignment]")
    print(f"    * Matrix Dimensions (N x M) : {expected_shape[0]} x {expected_shape[1]} = {expected_count:,}")
    print(f"    * COE Vector Integer Count  : {actual_count:,}")
    assert actual_count == expected_count, f"Dimension mismatch: expected {expected_count}, got {actual_count}"
    print("    * BRAM Memory Alignment     : EXACT 1-TO-1 MATCH [PASS]")

    # 3. Data Type Consistency Check
    print("\n  [Check 3: Data Type Consistency]")
    int_values = []
    for tok in tokens:
        # Check no decimal point
        assert "." not in tok, f"Floating point found in INT8 COE file: '{tok}'"
        val = int(tok)
        assert -128 <= val <= 127, f"Value out of INT8 range [-128, 127]: {val}"
        int_values.append(val)

    print(f"    * Data Type Format  : Signed Decimal INT8 without decimal points")
    print(f"    * Range Observed    : Min = {min(int_values)}, Max = {max(int_values)} (Within [-128, 127])")
    print(f"    * Preview (First 8) : {int_values[:8]}")
    print(f"    * Preview (Last 8)  : {int_values[-8:]}")
    print("    * Data Type Check   : ALL VALUES VALID INT8 [PASS]")
    print("-" * 80)
    print(f"  [OK] {file_path.name} is 100% compliant with Xilinx Vivado BRAM specifications!")


def export_and_verify_all():
    base_dir = pathlib.Path(__file__).resolve().parent
    coe_dir = base_dir / "coe"
    coe_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 80)
    print("  XILINX COE BRAM WEIGHT EXTRACTION & VERIFICATION PIPELINE")
    print("=" * 80)

    # 1. Load trained model
    ckpt_path = CKPT_DIR / "snn_baseline_mnist.pt"
    model = SpikingNet()
    ckpt = torch.load(ckpt_path, map_location="cpu")
    model.load_state_dict(ckpt["model_state_dict"])
    model.eval()

    # 2. Calibrate and convert to INT8
    transform = transforms.Compose([transforms.ToTensor(), transforms.Normalize((0.1307,), (0.3081,))])
    train_dataset = datasets.MNIST(root=str(DATA_DIR), train=True, download=False, transform=transform)
    train_loader = DataLoader(train_dataset, batch_size=BATCH_SIZE, shuffle=True)
    quantized_model = quantize_model_ptq(model, train_loader, num_calib_batches=2)

    # 3. Extract INT8 weights
    w1_int = quantized_model.fc1.weight().int_repr()  # [128, 784]
    w2_int = quantized_model.fc2.weight().int_repr()  # [10, 128]

    # 4. Write COE files
    fc1_coe = coe_dir / "fc1_weights.coe"
    fc2_coe = coe_dir / "fc2_weights.coe"

    print("\n" + "=" * 80)
    print("  GENERATING XILINX COE FILES FOR ARTIX-7 BRAM BLOCKS")
    print("=" * 80)
    write_coe_file(fc1_coe, w1_int, layer_name="Layer 1 (fc1: 784 -> 128)")
    write_coe_file(fc2_coe, w2_int, layer_name="Layer 2 (fc2: 128 -> 10)")

    # 5. Run Verification on both files
    verify_coe_file(fc1_coe, expected_shape=(128, 784))
    verify_coe_file(fc2_coe, expected_shape=(10, 128))

    print("\n" + "=" * 80)
    print("  [SUCCESS] All Xilinx .coe memory initialization files generated and verified.")
    print("=" * 80 + "\n")


if __name__ == "__main__":
    export_and_verify_all()

