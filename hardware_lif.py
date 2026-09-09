"""
HardwareLIF and HysteresisLIF Custom Spiking Neurons
====================================================
1. HardwareLIF: Custom Leaky Integrate-and-Fire neuron with learnable/static sub-threshold pruning.
2. HysteresisLIF: Custom LIF neuron with 2-bit hysteresis counter (grace period of 3 timesteps)
   to prevent premature pruning and suppress sub-threshold noise fluctuations.
"""

import torch
import torch.nn as nn
from snntorch import surrogate


class HysteresisLIF(nn.Module):
    """Leaky Integrate-and-Fire neuron with a 2-bit hysteresis counter (grace period = 3).

    Instead of instantly zeroing the membrane potential when U[t] < V_sub,
    each neuron maintains a 2-bit counter (values 0..3):
      - If U[t] >= V_sub: reset counter to 3 (active queue).
      - If U[t] < V_sub: decrement counter by 1.
      - If counter > 0: keep neuron active and preserve membrane potential.
      - If counter == 0: hard mask membrane potential to 0.0 (pruned from active queue).

    Parameters
    ----------
    beta : float
        Membrane potential decay factor (0 < beta < 1).
    threshold : float
        Firing threshold (default 1.0).
    v_sub : float
        Sub-threshold pruning voltage (default 0.1).
    max_count : int
        Maximum value of the 2-bit counter (default 3, giving a 3-timestep grace period).
    spike_grad : callable | None
        Surrogate gradient for the Heaviside spike step.
    """

    def __init__(
        self,
        beta: float = 0.9,
        threshold: float = 1.0,
        v_sub: float = 0.1,
        max_count: int = 3,
        spike_grad=None,
    ):
        super().__init__()
        self.beta = beta
        self.threshold = threshold
        self.v_sub = float(v_sub)
        self.max_count = float(max_count)
        self.spike_grad = spike_grad or surrogate.fast_sigmoid(slope=25)

    def init_state(self, input_tensor: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """Initialize persistent state tensors: zero membrane potential and counter initialized to max_count (3)."""
        mem = torch.zeros_like(input_tensor)
        counter = torch.full_like(input_tensor, self.max_count, dtype=torch.float32)
        return mem, counter

    def forward(
        self,
        current_input: torch.Tensor,
        mem: torch.Tensor,
        counter: torch.Tensor,
    ) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        """Advance neuron state by one discrete timestep with 2-bit hysteresis decay.

        Parameters
        ----------
        current_input : Tensor
            Synaptic input current I[t].
        mem : Tensor
            Persistent membrane potential U[t].
        counter : Tensor
            Persistent 2-bit tracking counter C[t] (values 0.0 .. 3.0).

        Returns
        -------
        spk : Tensor
            Output spikes (1 if fired, else 0).
        mem_next : Tensor
            Updated persistent membrane potential U[t+1].
        counter_next : Tensor
            Updated persistent 2-bit tracking counter C[t+1].
        """
        # 1. Leakage and Synaptic Integration
        mem_updated = self.beta * mem + current_input

        # 2. 2-Bit Counter State Machine Update:
        #    If U >= V_sub ==> Reset counter to 3
        #    If U < V_sub  ==> Decrement counter by 1 (clamped to 0)
        is_above = (mem_updated >= self.v_sub).float()
        counter_next = is_above * self.max_count + (1.0 - is_above) * torch.clamp(counter - 1.0, min=0.0)

        # 3. Hysteresis Pruning Mask:
        #    Keep neuron active as long as counter > 0; prune to 0.0 only when counter reaches 0
        active_mask = (counter_next > 0.0).float()
        mem_pruned = mem_updated * active_mask

        # 4. Spike Emission Check & Hard Reset
        mem_shifted = mem_pruned - self.threshold
        spk = self.spike_grad(mem_shifted)
        mem_next = mem_pruned - spk * self.threshold

        return spk, mem_next, counter_next

    def extra_repr(self) -> str:
        return (
            f"beta={self.beta}, threshold={self.threshold}, "
            f"v_sub={self.v_sub}, 2-bit counter max={int(self.max_count)}"
        )


class HardwareLIF(nn.Module):
    """Standard HardwareLIF neuron with learnable/static sub-threshold pruning."""

    def __init__(
        self,
        beta: float = 0.9,
        threshold: float = 1.0,
        v_sub: float = 0.1,
        spike_grad=None,
        prune_grad=None,
    ):
        super().__init__()
        self.beta = beta
        self.threshold = threshold

        if isinstance(v_sub, torch.Tensor):
            self.v_sub = nn.Parameter(v_sub.clone().detach().float())
        else:
            self.v_sub = nn.Parameter(torch.tensor(float(v_sub), dtype=torch.float32))

        self.spike_grad = spike_grad or surrogate.fast_sigmoid(slope=25)
        self.prune_grad = prune_grad or surrogate.fast_sigmoid(slope=25)

    def init_membrane(self, input_: torch.Tensor | None = None) -> torch.Tensor:
        if input_ is not None:
            return torch.zeros_like(input_)
        return torch.tensor(0.0)

    def forward(
        self,
        current_input: torch.Tensor,
        mem: torch.Tensor,
    ) -> tuple[torch.Tensor, torch.Tensor]:
        mem_updated = self.beta * mem + current_input
        prune_mask = self.prune_grad(mem_updated - self.v_sub)
        mem_updated = mem_updated * prune_mask
        mem_shifted = mem_updated - self.threshold
        spk = self.spike_grad(mem_shifted)
        mem_next = mem_updated - spk * self.threshold
        return spk, mem_next

    def extra_repr(self) -> str:
        v_sub_val = self.v_sub.item() if hasattr(self.v_sub, "item") else self.v_sub
        return f"beta={self.beta}, threshold={self.threshold}, v_sub={v_sub_val:.4f}"
