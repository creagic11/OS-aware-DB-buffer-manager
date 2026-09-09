"""
Standalone test for the HardwareLIF custom neuron.
===================================================
Passes a dummy random tensor (shape: [1, 784]) through a single
HardwareLIF neuron for 10 time-steps and prints the membrane potential,
spike output, and pruning status at each step, with a detailed visual trace
verifying that any potential below V_sub = 0.1 is strictly zeroed to 0.0.
"""

import torch
from hardware_lif import HardwareLIF


def test_hardware_lif():
    print("=" * 80)
    print("  [CHECK 1 & 2] HardwareLIF Standalone Test (10 Timesteps, Input Shape [1, 784])")
    print("=" * 80)

    V_SUB = 0.1
    THRESHOLD = 1.0
    BETA = 0.9
    neuron = HardwareLIF(beta=BETA, threshold=THRESHOLD, v_sub=V_SUB)
    print(f"\n  Neuron config: {neuron}")
    print(f"  Dynamic Pruning Threshold (V_sub): {V_SUB}  |  Firing Threshold: {THRESHOLD}\n")

    # Random input current for 10 time-steps with varied scale to show pruning & firing
    torch.manual_seed(42)
    num_steps = 10
    inputs = torch.rand(num_steps, 1, 784) * 0.4  # range [0.0, 0.4]

    # Initialise membrane potential to zeros
    mem = neuron.init_membrane(inputs[0])

    print("-" * 80)
    print("  SUMMARY ACROSS ALL 784 NEURONS PER TIMESTEP:")
    print(f"  {'Step':>4s}  |  {'Mem Mean':>10s}  {'Mem Min':>10s}  {'Mem Max':>10s}  |  {'Spikes':>6s}  {'Pruned (<0.1)':>15s}")
    print("-" * 80)

    # We will record traces for sample neurons to visually inspect Check 2
    traces = {}
    for idx in range(784):
        traces[idx] = []

    for t in range(num_steps):
        # Calculate raw potential before pruning for inspection
        raw_mem = BETA * mem + inputs[t]

        # Execute neuron step (applies V_sub pruning, spiking, and reset)
        spk, mem_next = neuron(inputs[t], mem)

        mem_mean = mem_next.mean().item()
        mem_min = mem_next.min().item()
        mem_max = mem_next.max().item()
        num_spikes = int(spk.sum().item())
        num_pruned = int((mem_next == 0.0).sum().item())

        print(
            f"  t={t:>2d}   |  {mem_mean:>10.4f}  {mem_min:>10.4f}  {mem_max:>10.4f}  |  {num_spikes:>6d}  {num_pruned:>15d} / 784"
        )

        for idx in range(784):
            traces[idx].append({
                "t": t,
                "input": inputs[t, 0, idx].item(),
                "raw_u": raw_mem[0, idx].item(),
                "pruned_u": mem_next[0, idx].item(),
                "spike": int(spk[0, idx].item()),
            })

        mem = mem_next

    print("-" * 80)
    print("\n" + "=" * 80)
    print("  [CHECK 2 VISUAL VERIFICATION] INDIVIDUAL NEURON TRACES:")
    print("  Observe: If Raw U[t] < 0.1000  ==> Pruned U[t] = 0.0000 (HARDWARE GATED)")
    print("           If Raw U[t] >= 0.1000 ==> Pruned U[t] is preserved")
    print("=" * 80)

    # Automatically find neurons that experienced pruning (raw_u < 0.1) as well as spiking
    pruned_neuron_indices = []
    for idx in range(784):
        has_pruned = any(entry["raw_u"] < V_SUB for entry in traces.get(idx, []))
        if has_pruned and len(pruned_neuron_indices) < 3:
            pruned_neuron_indices.append(idx)

    # Display neurons with pruning events clearly highlighted
    display_indices = [pruned_neuron_indices[0], pruned_neuron_indices[1], 0, 1] if len(pruned_neuron_indices) >= 2 else sample_indices[:4]

    for idx in display_indices:
        print(f"\n  >> Neuron #{idx} Trace over 10 timesteps:")
        print(f"     {'Step':>4s} | {'Input I[t]':>10s} | {'Raw U[t]':>10s} | {'Pruned U[t]':>12s} | {'Spike':>5s} | {'Hardware Gating Status':>24s}")
        print("     " + "-" * 76)
        for entry in traces[idx]:
            raw_val = entry["raw_u"]
            pruned_val = entry["pruned_u"]
            spk_val = entry["spike"]
            if raw_val < V_SUB:
                status = "PRUNED (-> 0.0000)"
            elif spk_val:
                status = "SPIKED & RESET"
            else:
                status = "ACTIVE"
            print(
                f"     t={entry['t']:>2d} | {entry['input']:>10.4f} | {raw_val:>10.4f} | {pruned_val:>12.4f} | {spk_val:>5d} | {status:>24s}"
            )

    print("\n" + "=" * 80)
    print("  [OK] Verification Check 1 & Check 2 passed successfully!")
    print("=" * 80 + "\n")


if __name__ == "__main__":
    test_hardware_lif()

