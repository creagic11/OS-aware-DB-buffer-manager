# SNN Baseline & Hardware Deployment — MNIST

A feedforward Spiking Neural Network (SNN) baseline trained on rate-coded MNIST
spike trains, featuring **learnable sub-threshold hardware pruning** and **INT8 Post-Training Static Quantization (PTQ)**.

## Project Structure

```
snn_baseline/
├── train.py                   # Full training, autograd check, evaluation & PTQ pipeline
├── hardware_lif.py            # Custom HardwareLIF neuron (LIF dynamics + learnable V_sub pruning)
├── test_hardware_lif.py       # Standalone 10-timestep neuron test with trace inspection
├── quantize.py                # Standalone INT8 PTQ quantization script
├── export_coe.py              # Xilinx .coe coefficient exporter (INT8 radix-10)
├── requirements.txt           # Python dependencies
├── README.md
├── coe/                       # Exported Xilinx .coe memory initialization files
│   ├── fc1_weights.coe        # Layer 1 weights (100,352 INT8 integers)
│   └── fc2_weights.coe        # Layer 2 weights (1,280 INT8 integers)
├── checkpoints/
│   ├── snn_baseline_mnist.pt          # Trained FP32 model checkpoint
│   └── snn_quantized_int8_mnist.pt    # Finalized INT8 quantized model
└── logs/
    └── training_log.json      # Training loss, accuracy & V_sub tracking
```

## Quick Start

```bash
# 1. Run standalone neuron test (Check 1 & Check 2)
python test_hardware_lif.py

# 2. Run full training + autograd validation + PTQ pipeline (Check 3 & Check 4)
python train.py

# 3. (Optional) Run standalone INT8 PTQ quantization on saved weights
python quantize.py

# 4. Export INT8 weights to Xilinx .coe files for Vivado / FPGA BRAM
python export_coe.py
```

## Architecture & Hardware Features

| Layer | Type | Size | Hardware Detail |
|---|---|---|---|
| Input | Rate-coded Spikes | 784 (28×28) | Bernoulli spike encoding over 25 time-steps |
| FC1 | `QuantStub` → Linear → `DeQuantStub` | 128 | **INT8 Quantized Synaptic Weights (`torch.qint8`)** |
| LIF1 | `HardwareLIF` | 128 | $U[t+1] = \beta U[t] + I[t] - S\cdot\theta$, Learnable $V_{sub}$, Pruning surrogate |
| FC2 | `QuantStub` → Linear → `DeQuantStub` | 10 | **INT8 Quantized Synaptic Weights (`torch.qint8`)** |
| LIF2 | `HardwareLIF` | 10 | $U[t+1] = \beta U[t] + I[t] - S\cdot\theta$, Learnable $V_{sub}$, Pruning surrogate |

- **Sub-threshold Pruning**: Evaluates $H(U' - V_{sub})$ with surrogate gradients to zero out sub-threshold potentials and learn layer-specific hardware gating voltages.
- **PTQ Quantization**: Calibrates activation distributions across training batches and quantizes synaptic weight tensors to 8-bit integer precision.

## Performance Benchmark

| Model Stage | Precision | Test Accuracy | Delta |
|---|---|---|---|
| **FP32 Trained Model** | 32-bit Float | **87.35%** | Baseline |
| **INT8 PTQ Quantized Model** | **8-bit Integer (`torch.qint8`)** | **87.18%** | **-0.17%** (Preserved) |
  
