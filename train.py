"""
SNN with Hardware-Oriented Pruning -- MNIST Classification
==========================================================
A feedforward Spiking Neural Network using a custom HardwareLIF neuron
that implements sub-threshold pruning (V_sub gating) for hardware deployment.
Uses surrogate gradient descent to learn through the non-differentiable spike function.

Project structure:
    snn_baseline/
    ├── train.py              <- this script (training + evaluation)
    ├── hardware_lif.py       <- custom LIF neuron with pruning
    ├── test_hardware_lif.py  <- standalone neuron test
    ├── data/                 <- auto-created by torchvision (MNIST download)
    ├── checkpoints/          <- saved model weights
    └── logs/                 <- training logs (loss / accuracy)
"""

import os
import time
import json
import pathlib

import torch
import torch.nn as nn
from torch.utils.data import DataLoader
from torchvision import datasets, transforms

import torch.ao.quantization as quantization
import snntorch as snn
from snntorch import spikegen
from snntorch import surrogate
from snntorch import utils as snn_utils

from hardware_lif import HardwareLIF
from test_hardware_lif import test_hardware_lif

# ─────────────────────────── Hyperparameters ──────────────────────────────────

NUM_EPOCHS = 3
BATCH_SIZE = 128
LEARNING_RATE = 1e-3
NUM_STEPS = 25          # number of time-steps in the spike train
BETA = 0.9              # membrane potential decay rate for LIF neurons
NUM_HIDDEN = 128        # hidden-layer size
NUM_OUTPUTS = 10        # MNIST has 10 digit classes
PRINT_EVERY = 50        # print loss every N batches
V_SUB = 0.1             # sub-threshold pruning voltage (hardware gating)

# Surrogate gradient -- fast sigmoid is a common, hardware-friendly choice
SPIKE_GRAD = surrogate.fast_sigmoid(slope=25)

# ─────────────────────────── Directory Setup ──────────────────────────────────

BASE_DIR = pathlib.Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
CKPT_DIR = BASE_DIR / "checkpoints"
LOG_DIR = BASE_DIR / "logs"

for d in (DATA_DIR, CKPT_DIR, LOG_DIR):
    d.mkdir(parents=True, exist_ok=True)

DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")
print(f"[info] Using device: {DEVICE}")

# ─────────────────────────── 1. Dataset ───────────────────────────────────────

transform = transforms.Compose([
    transforms.ToTensor(),
    transforms.Normalize((0.1307,), (0.3081,)),   # MNIST global mean & std
])

train_dataset = datasets.MNIST(
    root=str(DATA_DIR), train=True, download=True, transform=transform,
)
test_dataset = datasets.MNIST(
    root=str(DATA_DIR), train=False, download=True, transform=transform,
)

train_loader = DataLoader(
    train_dataset, batch_size=BATCH_SIZE, shuffle=True, drop_last=True,
)
test_loader = DataLoader(
    test_dataset, batch_size=BATCH_SIZE, shuffle=False, drop_last=False,
)

print(f"[info] Train samples: {len(train_dataset):,}  |  Test samples: {len(test_dataset):,}")


# ─────────────────────────── 2. Rate-coded spike encoding ─────────────────────

def encode_batch(images: torch.Tensor) -> torch.Tensor:
    """Convert a batch of images (B, 1, 28, 28) → rate-coded spike trains
    of shape (NUM_STEPS, B, 784).

    Each pixel intensity is treated as the firing probability at every time-step.
    """
    flat = images.view(images.size(0), -1)          # (B, 784)
    # Clamp to [0, 1] so it can be used as a probability
    flat = flat.clamp(0.0, 1.0)
    spike_train = spikegen.rate(flat, num_steps=NUM_STEPS)  # (T, B, 784)
    return spike_train


# ------------- 3. SNN Model (with HardwareLIF + QuantStubs) ------------------

class SpikingNet(nn.Module):
    """Two-layer feedforward SNN with hardware-oriented pruning and quantization stubs.

    Architecture:
        784 -> [QuantStub] -> Linear -> [DeQuantStub] -> HardwareLIF
            -> 128 -> [QuantStub] -> Linear -> [DeQuantStub] -> HardwareLIF -> 10

    Each HardwareLIF neuron:
      - Implements the discrete LIF update:  U[t+1] = beta*U[t] + I[t] - spike*thr
      - Uses surrogate gradients for both spiking and learnable V_sub pruning
    """

    def __init__(self):
        super().__init__()
        # Layer 1
        self.quant1 = quantization.QuantStub()
        self.fc1 = nn.Linear(784, NUM_HIDDEN)
        self.dequant1 = quantization.DeQuantStub()
        self.lif1 = HardwareLIF(beta=BETA, threshold=1.0,
                                v_sub=V_SUB, spike_grad=SPIKE_GRAD)

        # Layer 2 (output)
        self.quant2 = quantization.QuantStub()
        self.fc2 = nn.Linear(NUM_HIDDEN, NUM_OUTPUTS)
        self.dequant2 = quantization.DeQuantStub()
        self.lif2 = HardwareLIF(beta=BETA, threshold=1.0,
                                v_sub=V_SUB, spike_grad=SPIKE_GRAD)

    def forward(self, spike_train: torch.Tensor):
        """
        Args:
            spike_train: (NUM_STEPS, B, 784)

        Returns:
            spk_rec: (NUM_STEPS, B, 10)  -- output spikes at each time-step
            mem_rec: (NUM_STEPS, B, 10)  -- membrane potentials at each step
        """
        # Initialise membrane potentials to zeros matching batch dims
        first_input = spike_train[0]                  # (B, 784)
        mem1 = self.lif1.init_membrane(
            torch.zeros(first_input.size(0), NUM_HIDDEN,
                        device=first_input.device)
        )
        mem2 = self.lif2.init_membrane(
            torch.zeros(first_input.size(0), NUM_OUTPUTS,
                        device=first_input.device)
        )

        spk_rec = []
        mem_rec = []

        for step in range(spike_train.size(0)):
            x = spike_train[step]                    # (B, 784)

            # Layer 1: QuantStub -> Linear -> DeQuantStub -> HardwareLIF
            q_x1 = self.quant1(x)
            cur1_q = self.fc1(q_x1)
            cur1 = self.dequant1(cur1_q)
            spk1, mem1 = self.lif1(cur1, mem1)

            # Layer 2: QuantStub -> Linear -> DeQuantStub -> HardwareLIF
            q_x2 = self.quant2(spk1)
            cur2_q = self.fc2(q_x2)
            cur2 = self.dequant2(cur2_q)
            spk2, mem2 = self.lif2(cur2, mem2)

            spk_rec.append(spk2)
            mem_rec.append(mem2)

        return torch.stack(spk_rec), torch.stack(mem_rec)


# ─────────────────────────── 4. Training utilities ────────────────────────────

def cross_entropy_count_loss(spk_rec: torch.Tensor, targets: torch.Tensor):
    """Cross-entropy applied to the *total spike count* over time.

    spike counts → log-softmax → NLL, which is equivalent to CE on counts.
    This is a standard loss for rate-coded SNNs.
    """
    spike_counts = spk_rec.sum(dim=0)                # (B, 10)
    log_probs = torch.log_softmax(spike_counts, dim=1)
    return nn.functional.nll_loss(log_probs, targets)


def count_correct(spk_rec: torch.Tensor, targets: torch.Tensor) -> int:
    """Predict by the class with the highest total spike count."""
    spike_counts = spk_rec.sum(dim=0)                # (B, 10)
    preds = spike_counts.argmax(dim=1)
    return (preds == targets).sum().item()


# ─────────────────────────── 5. Training Loop ─────────────────────────────────

def train_one_epoch(model, loader, optimiser, epoch):
    model.train()
    epoch_loss = 0.0
    epoch_correct = 0
    total_samples = 0

    for batch_idx, (images, labels) in enumerate(loader):
        images, labels = images.to(DEVICE), labels.to(DEVICE)

        # Rate-code the batch into spike trains
        spike_train = encode_batch(images).to(DEVICE)

        # Forward pass
        spk_rec, mem_rec = model(spike_train)

        # Loss & backward
        loss = cross_entropy_count_loss(spk_rec, labels)
        optimiser.zero_grad()
        loss.backward()
        optimiser.step()

        # Book-keeping
        epoch_loss += loss.item()
        epoch_correct += count_correct(spk_rec, labels)
        total_samples += labels.size(0)

        if (batch_idx + 1) % PRINT_EVERY == 0:
            running_acc = 100.0 * epoch_correct / total_samples
            print(
                f"  Epoch {epoch+1}/{NUM_EPOCHS} | "
                f"Batch {batch_idx+1:>4d}/{len(loader)} | "
                f"Loss: {loss.item():.4f} | "
                f"Acc so far: {running_acc:.2f}%"
            )

    avg_loss = epoch_loss / len(loader)
    accuracy = 100.0 * epoch_correct / total_samples
    return avg_loss, accuracy


# ─────────────────────────── 6. Evaluation ────────────────────────────────────

@torch.no_grad()
def evaluate(model, loader):
    model.eval()
    total_correct = 0
    total_samples = 0
    total_loss = 0.0

    for images, labels in loader:
        images, labels = images.to(DEVICE), labels.to(DEVICE)
        spike_train = encode_batch(images).to(DEVICE)

        spk_rec, mem_rec = model(spike_train)

        total_loss += cross_entropy_count_loss(spk_rec, labels).item()
        total_correct += count_correct(spk_rec, labels)
        total_samples += labels.size(0)

    avg_loss = total_loss / len(loader)
    accuracy = 100.0 * total_correct / total_samples
    return avg_loss, accuracy


# ─────────────────────────── 7. Post-Training Static Quantization (PTQ) ──────

def quantize_model_ptq(trained_model: nn.Module, calibration_loader: DataLoader, num_calib_batches: int = 2):
    """Apply Post-Training Static Quantization (PTQ) to quantize synaptic weights to INT8.

    Steps:
      1. Configure supported INT8 qconfig ('onednn', 'fbgemm', or 'x86').
      2. Apply qconfig exclusively to the nn.Linear layers & QuantStubs, leaving LIF neurons in FP32.
      3. Insert observers via quantization.prepare.
      4. Calibrate activation distributions on a small subset of MNIST (e.g. 2 batches).
      5. Finalize INT8 model via quantization.convert.
    """
    print("\n" + "=" * 80)
    print("  [PTQ] POST-TRAINING STATIC QUANTIZATION (INT8 SYNAPTIC WEIGHTS)")
    print("=" * 80)

    # 1. Determine supported quantization backend engine & default INT8 qconfig
    supported = torch.backends.quantized.supported_engines
    engine = 'onednn' if 'onednn' in supported else ('fbgemm' if 'fbgemm' in supported else 'qnnpack')
    torch.backends.quantized.engine = engine
    qconfig = quantization.get_default_qconfig(engine)
    print(f"  * Quantization Engine: '{engine}' (Supported: {supported})")
    print(f"  * Default INT8 qconfig: {type(qconfig).__name__}")

    # 2. Put model in eval mode and assign qconfig EXCLUSIVELY to linear layers
    trained_model.eval()
    trained_model.qconfig = None
    trained_model.quant1.qconfig = qconfig
    trained_model.fc1.qconfig = qconfig
    trained_model.dequant1.qconfig = qconfig
    trained_model.quant2.qconfig = qconfig
    trained_model.fc2.qconfig = qconfig
    trained_model.dequant2.qconfig = qconfig

    # Explicitly ensure custom LIF neurons remain unquantized
    trained_model.lif1.qconfig = None
    trained_model.lif2.qconfig = None

    # 3. Insert observers with prepare
    print("  * Inserting observers into linear layers via quantization.prepare()...")
    prepared_model = quantization.prepare(trained_model, inplace=False)

    # 4. Calibration loop on small subset of data (Check 1)
    print(f"  * Running calibration loop on {num_calib_batches} batch(es) of training data...")
    with torch.no_grad():
        for b_idx, (images, _) in enumerate(calibration_loader):
            if b_idx >= num_calib_batches:
                break
            images = images.to(DEVICE)
            spike_train = encode_batch(images).to(DEVICE)
            prepared_model(spike_train)
    print("  * [OK] Calibration completed successfully without crashing.")

    # 5. Convert to INT8 quantized model
    print("  * Converting to finalized INT8 quantized model via quantization.convert()...")
    quantized_model = quantization.convert(prepared_model, inplace=False)
    print("  * [OK] Model converted successfully.")

    # 6. Print state_dict keys and data types (Check 2)
    print("\n" + "-" * 80)
    print("  [CHECK 2] QUANTIZED MODEL STATE_DICT & DATA TYPES:")
    print(f"  {'Key':40s} | {'Type / Dtype':25s} | {'Quantized Dtype':15s}")
    print("-" * 80)
    for k, v in quantized_model.state_dict().items():
        dtype_str = str(v.dtype) if hasattr(v, 'dtype') else type(v).__name__
        qtype_str = ""
        if hasattr(v, 'qscheme'):
            qtype_str = str(v.dtype)
        print(f"  {k:40s} | {dtype_str:25s} | {qtype_str:15s}")

    # Explicitly check and print the weight dtypes of the linear layers
    fc1_weight_dtype = str(quantized_model.fc1.weight().dtype)
    fc2_weight_dtype = str(quantized_model.fc2.weight().dtype)
    print("\n  >> Linear Synaptic Weight Dtypes:")
    print(f"     Layer 1 (fc1.weight()): {fc1_weight_dtype}  (8-bit Quantized Weight)")
    print(f"     Layer 2 (fc2.weight()): {fc2_weight_dtype}  (8-bit Quantized Weight)")
    print("-" * 80)

    return quantized_model


# ─────────────────────────── Main ─────────────────────────────────────────────

def main():
    # ── Step 0: Run Check 1 & Check 2 standalone neuron test ────────────
    test_hardware_lif()

    print("\n" + "=" * 80)
    print("  [CHECK 3] SNN Model Training & Autograd Graph Validation (3 Epochs)")
    print("=" * 80)

    model = SpikingNet().to(DEVICE)
    optimiser = torch.optim.Adam(model.parameters(), lr=LEARNING_RATE)

    # Autograd test with dummy batch
    print("[autograd] Running autograd graph sanity check with dummy batch...")
    dummy_input = torch.rand(NUM_STEPS, 2, 784, device=DEVICE)
    dummy_target = torch.tensor([1, 7], device=DEVICE)
    spk_dummy, _ = model(dummy_input)
    dummy_loss = cross_entropy_count_loss(spk_dummy, dummy_target)
    dummy_loss.backward()
    optimiser.zero_grad()
    print("[autograd] Success: Backward pass completed with zero graph errors.\n")

    # Model summary & initial V_sub parameter values
    total_params = sum(p.numel() for p in model.parameters())
    print(f"[model] Parameters: {total_params:,}")
    print(f"[config] epochs={NUM_EPOCHS}  batch={BATCH_SIZE}  "
          f"lr={LEARNING_RATE}  steps={NUM_STEPS}  beta={BETA}\n")

    # ── CHECK 1: Print V_sub parameter value BEFORE training starts ────────
    v_sub_l1_init = model.lif1.v_sub.item()
    v_sub_l2_init = model.lif2.v_sub.item()
    print("-" * 80)
    print("  [CHECK 1] LEARNABLE V_SUB BEFORE TRAINING:")
    print(f"    Layer 1 (lif1.v_sub) : {v_sub_l1_init:.6f}  (Initial target: 0.1)")
    print(f"    Layer 2 (lif2.v_sub) : {v_sub_l2_init:.6f}  (Initial target: 0.1)")
    print("-" * 80 + "\n")

    log_records = []

    for epoch in range(NUM_EPOCHS):
        t0 = time.time()
        train_loss, train_acc = train_one_epoch(model, train_loader, optimiser, epoch)
        elapsed = time.time() - t0

        val_loss, val_acc = evaluate(model, test_loader)

        v_sub_l1_cur = model.lif1.v_sub.item()
        v_sub_l2_cur = model.lif2.v_sub.item()

        print(
            f"\n  >> Epoch {epoch+1}/{NUM_EPOCHS} done in {elapsed:.1f}s  |  "
            f"Train loss: {train_loss:.4f}  Acc: {train_acc:.2f}%  |  "
            f"Val loss: {val_loss:.4f}  Acc: {val_acc:.2f}%  |  "
            f"lif1.v_sub: {v_sub_l1_cur:.5f}  lif2.v_sub: {v_sub_l2_cur:.5f}\n"
        )

        log_records.append({
            "epoch": epoch + 1,
            "train_loss": round(train_loss, 4),
            "train_acc": round(train_acc, 2),
            "val_loss": round(val_loss, 4),
            "val_acc": round(val_acc, 2),
            "lif1_v_sub": round(v_sub_l1_cur, 5),
            "lif2_v_sub": round(v_sub_l2_cur, 5),
            "time_s": round(elapsed, 1),
        })

    # ── CHECK 3 & 4: Print V_sub parameter value AFTER training + Final Accuracy ──
    v_sub_l1_final = model.lif1.v_sub.item()
    v_sub_l2_final = model.lif2.v_sub.item()

    # ── Save checkpoint ──────────────────────────────────────────────────
    ckpt_path = CKPT_DIR / "snn_baseline_mnist.pt"
    torch.save({
        "model_state_dict": model.state_dict(),
        "optimiser_state_dict": optimiser.state_dict(),
        "hyperparams": {
            "num_steps": NUM_STEPS,
            "beta": BETA,
            "num_hidden": NUM_HIDDEN,
            "lr": LEARNING_RATE,
            "lif1_v_sub_final": v_sub_l1_final,
            "lif2_v_sub_final": v_sub_l2_final,
        },
    }, ckpt_path)
    print(f"[save] Checkpoint -> {ckpt_path}")

    # ── Save training log ────────────────────────────────────────────────
    log_path = LOG_DIR / "training_log.json"
    with open(log_path, "w") as f:
        json.dump(log_records, f, indent=2)
    print(f"[save] Training log -> {log_path}")

    # ── Final evaluation (FP32) ──────────────────────────────────────────
    final_loss, final_acc = evaluate(model, test_loader)
    print("\n" + "=" * 80)
    print("  [CHECK 3 & 4 FINAL DELIVERABLES SUMMARY]")
    print("=" * 80)
    print(f"  * Initial V_sub (before training):  Layer 1 = {v_sub_l1_init:.6f}, Layer 2 = {v_sub_l2_init:.6f}")
    print(f"  * Adapted V_sub (after training):   Layer 1 = {v_sub_l1_final:.6f}, Layer 2 = {v_sub_l2_final:.6f}")
    print(f"  * Difference (delta from 0.1):      Layer 1 = {v_sub_l1_final - 0.1:+.6f}, Layer 2 = {v_sub_l2_final - 0.1:+.6f}")
    print(f"  * [OK] Final FP32 Test Accuracy:    {final_acc:.2f}%  (loss: {final_loss:.4f})")
    print("=" * 80 + "\n")

    # ── Step 8: Post-Training Static Quantization (PTQ) to INT8 ──────────
    quantized_model = quantize_model_ptq(model, train_loader, num_calib_batches=2)

    # ── Final evaluation on INT8 model ───────────────────────────────────
    print("\n" + "=" * 80)
    print("  [EVALUATION] RUNNING TEST SET EVALUATION ON INT8 QUANTIZED MODEL...")
    print("=" * 80)
    q_loss, q_acc = evaluate(quantized_model, test_loader)
    print(f"\n  [PTQ FINAL RESULT] INT8 Validation Accuracy: {q_acc:.2f}%  (loss: {q_loss:.4f})")
    print(f"  Accuracy change after INT8 PTQ: {q_acc - final_acc:+.2f}%")
    print("=" * 80 + "\n")

    # Save quantized checkpoint
    q_ckpt_path = CKPT_DIR / "snn_quantized_int8_mnist.pt"
    torch.save(quantized_model.state_dict(), q_ckpt_path)
    print(f"[save] INT8 Quantized Checkpoint -> {q_ckpt_path}\n")


if __name__ == "__main__":
    main()
