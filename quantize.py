"""
Post-Training Static Quantization (PTQ) for SNN with HardwareLIF
================================================================
Loads the trained FP32 SNN checkpoint, applies PyTorch PTQ to quantize
the synaptic weights (nn.Linear) to INT8, calibrates on training samples,
inspects state_dict dtypes, and evaluates final INT8 validation accuracy.
"""

import pathlib
import torch
import torch.nn as nn
from torch.utils.data import DataLoader
from torchvision import datasets, transforms
import torch.ao.quantization as quantization

from train import SpikingNet, quantize_model_ptq, evaluate, encode_batch, DATA_DIR, CKPT_DIR, BATCH_SIZE


def main():
    print("=" * 80)
    print("  STANDALONE INT8 POST-TRAINING STATIC QUANTIZATION (PTQ)")
    print("=" * 80)

    # 1. Load trained FP32 checkpoint
    ckpt_path = CKPT_DIR / "snn_baseline_mnist.pt"
    if not ckpt_path.exists():
        print(f"[error] Checkpoint not found at {ckpt_path}. Please run train.py first.")
        return

    model = SpikingNet()
    ckpt = torch.load(ckpt_path, map_location="cpu")
    model.load_state_dict(ckpt["model_state_dict"])
    model.eval()
    print(f"[load] Successfully loaded trained weights from {ckpt_path}")

    # 2. Data loader for calibration and evaluation
    transform = transforms.Compose([
        transforms.ToTensor(),
        transforms.Normalize((0.1307,), (0.3081,)),
    ])
    train_dataset = datasets.MNIST(root=str(DATA_DIR), train=True, download=True, transform=transform)
    test_dataset = datasets.MNIST(root=str(DATA_DIR), train=False, download=True, transform=transform)
    train_loader = DataLoader(train_dataset, batch_size=BATCH_SIZE, shuffle=True)
    test_loader = DataLoader(test_dataset, batch_size=BATCH_SIZE, shuffle=False)

    # 3. Evaluate FP32 baseline
    print("\n[eval] Evaluating FP32 baseline model...")
    fp32_loss, fp32_acc = evaluate(model, test_loader)
    print(f"[eval] FP32 Test Accuracy: {fp32_acc:.2f}%  (loss: {fp32_loss:.4f})")

    # 4. Apply PTQ Quantization
    quantized_model = quantize_model_ptq(model, train_loader, num_calib_batches=2)

    # 5. Evaluate INT8 model
    print("\n" + "=" * 80)
    print("  [EVALUATION] RUNNING TEST SET EVALUATION ON INT8 QUANTIZED MODEL...")
    print("=" * 80)
    q_loss, q_acc = evaluate(quantized_model, test_loader)
    print(f"\n  [PTQ FINAL RESULT] INT8 Validation Accuracy: {q_acc:.2f}%  (loss: {q_loss:.4f})")
    print(f"  Accuracy change after INT8 PTQ: {q_acc - fp32_acc:+.2f}%")
    print("=" * 80 + "\n")

    # 6. Save INT8 checkpoint
    q_ckpt_path = CKPT_DIR / "snn_quantized_int8_mnist.pt"
    torch.save(quantized_model.state_dict(), q_ckpt_path)
    print(f"[save] INT8 Quantized Checkpoint -> {q_ckpt_path}\n")


if __name__ == "__main__":
    main()
